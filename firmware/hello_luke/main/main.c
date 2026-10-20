/*
 * Hello Luke - minimal clock face for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Shows "Hello Luke" plus the current time and date, read from the on-board
 * PCF85063 real-time clock. If the RTC has never been set (or holds a time
 * older than this firmware's build time) it is seeded with the build time.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "qmi8658.h"
#include "net.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "hello_luke";
#define ENABLE_NET 1   /* 1 = join Wi-Fi at boot and talk to the Pi (needs main/secrets.h) */
typedef enum { FACE_CLOCK = 0, FACE_HOME, FACE_STOPWATCH } face_t;
static face_t s_face;
static void face_set(face_t face);
static void face_toggle(void);
static void face_reset_instant(void);
static void screen_set(bool on);
static bool s_screen_on = true;

/* ---------------------------------------------------------------------- */
/* Event log: last EVLOG_N touch/controller/screen events, kept in noinit   */
/* RAM so it survives a software reset. Dump over serial with "DUMP".       */
/* ---------------------------------------------------------------------- */
#define EVLOG_N      512
#define EVLOG_MAGIC  0x4C554B45u   /* "LUKE" */
typedef struct { uint32_t t_ms; char kind; int16_t a; int16_t b; } evlog_entry_t;
static __NOINIT_ATTR uint32_t      s_evlog_magic;
static __NOINIT_ATTR uint32_t      s_evlog_head;     /* next write index, grows forever */
static __NOINIT_ATTR evlog_entry_t s_evlog[EVLOG_N];
static portMUX_TYPE s_evlog_mux = portMUX_INITIALIZER_UNLOCKED;

static void evlog_add(char kind, int a, int b)
{
    portENTER_CRITICAL(&s_evlog_mux);
    evlog_entry_t *e = &s_evlog[s_evlog_head % EVLOG_N];
    e->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->kind = kind; e->a = (int16_t)a; e->b = (int16_t)b;
    s_evlog_head++;
    portEXIT_CRITICAL(&s_evlog_mux);
}

static void evlog_init(void)
{
    if (s_evlog_magic != EVLOG_MAGIC || s_evlog_head > 0x7FFFFFFF) {
        s_evlog_magic = EVLOG_MAGIC;
        s_evlog_head = 0;
    }
    evlog_add('B', (int)esp_reset_reason(), 0);   /* boot marker + why we reset */
}
/* Touch-derived actions (cover, wake, taps, swipes) are ignored until this time.
 * Set after a screen toggle, and briefly after a palm-rejection window ends,
 * because the controller emits a ghost press as it comes back. */
static int64_t s_touch_quiet_until_us = 0;
#define TOUCH_QUIET_AFTER_TOGGLE_MS   1500
#define TOUCH_QUIET_AFTER_NACK_MS     400
static inline bool touch_quiet(void) { return esp_timer_get_time() < s_touch_quiet_until_us; }
static void corner_tap(int x, int y);
static void claude_ask(const char *question);
static void claude_button(void);
static void clock_tick(lv_timer_t *timer);
static bool s_raw_diff = false;   /* unused now; kept for the RAW serial command */
static bool s_calib_log = false;  /* calibration logging, "LOG ON|OFF" over serial */

/* ---------------------------------------------------------------------- */
/* PCF85063 RTC (I2C address 0x51)                                        */
/* ---------------------------------------------------------------------- */
#define PCF85063_ADDR       0x51
#define PCF85063_CTRL1_REG  0x00
#define PCF85063_SEC_REG    0x04
#define PCF85063_OS_FLAG    0x80   /* seconds reg bit7: oscillator stopped */

static i2c_master_dev_handle_t s_rtc = NULL;

static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t rtc_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "BSP I2C bus not available");

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_rtc), TAG, "add RTC device");

    /* CTRL1 = 0: oscillator running, 24-hour mode, no test/reset bits. */
    uint8_t ctrl[2] = { PCF85063_CTRL1_REG, 0x00 };
    return i2c_master_transmit(s_rtc, ctrl, sizeof(ctrl), 100);
}

static esp_err_t rtc_read(struct tm *t)
{
    uint8_t reg = PCF85063_SEC_REG;
    uint8_t b[7];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_rtc, &reg, 1, b, sizeof(b), 100), TAG, "rtc read");
    if (b[0] & PCF85063_OS_FLAG) {
        return ESP_ERR_INVALID_STATE;   /* clock integrity not guaranteed */
    }
    memset(t, 0, sizeof(*t));
    t->tm_sec  = bcd2dec(b[0] & 0x7F);
    t->tm_min  = bcd2dec(b[1] & 0x7F);
    t->tm_hour = bcd2dec(b[2] & 0x3F);
    t->tm_mday = bcd2dec(b[3] & 0x3F);
    t->tm_wday = b[4] & 0x07;
    t->tm_mon  = bcd2dec(b[5] & 0x1F) - 1;
    t->tm_year = bcd2dec(b[6]) + 100;    /* RTC stores 00-99 => 2000-2099 */
    return ESP_OK;
}

static esp_err_t rtc_write(const struct tm *t)
{
    uint8_t b[8] = {
        PCF85063_SEC_REG,
        dec2bcd(t->tm_sec),             /* also clears the OS flag */
        dec2bcd(t->tm_min),
        dec2bcd(t->tm_hour),
        dec2bcd(t->tm_mday),
        (uint8_t)t->tm_wday,
        dec2bcd(t->tm_mon + 1),
        dec2bcd(t->tm_year - 100),
    };
    return i2c_master_transmit(s_rtc, b, sizeof(b), 100);
}

/* System time is kept in "wall clock" units with TZ left at UTC, so the
 * values stored in the RTC round-trip through mktime/localtime unchanged. */
static void set_system_time(const struct tm *t)
{
    struct tm tmp = *t;
    struct timeval tv = { .tv_sec = mktime(&tmp), .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void clock_setup(void)
{
    struct tm build = {0};
    sscanf(BUILD_STAMP, "%d-%d-%d %d:%d:%d",
           &build.tm_year, &build.tm_mon, &build.tm_mday,
           &build.tm_hour, &build.tm_min, &build.tm_sec);
    build.tm_year -= 1900;
    build.tm_mon  -= 1;
    struct tm tmp = build;
    time_t build_epoch = mktime(&tmp);
    build = tmp;                         /* fills tm_wday */

    if (rtc_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC not reachable; using build time %s", BUILD_STAMP);
        set_system_time(&build);
        return;
    }

    struct tm now;
    esp_err_t err = rtc_read(&now);
    time_t rtc_epoch = 0;
    if (err == ESP_OK) {
        tmp = now;
        rtc_epoch = mktime(&tmp);
    }

    if (err != ESP_OK || rtc_epoch < build_epoch) {
        ESP_LOGI(TAG, "RTC unset/stale (err=%s); seeding with build time %s",
                 esp_err_to_name(err), BUILD_STAMP);
        if (rtc_write(&build) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write RTC");
        }
        now = build;
    } else {
        ESP_LOGI(TAG, "RTC time %04d-%02d-%02d %02d:%02d:%02d",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec);
    }
    set_system_time(&now);
}

/* ---------------------------------------------------------------------- */
/* AXP2101 PMU: battery gauge (I2C 0x34)                                  */
/* ---------------------------------------------------------------------- */
#define AXP2101_ADDR        0x34
#define AXP_STATUS1         0x00   /* bit3: battery present            */
#define AXP_STATUS2         0x01   /* bits7:5: 01 charging, 10 discharging */
#define AXP_ADC_CTRL        0x30   /* bit0: enable VBAT ADC            */
#define AXP_VBAT_H          0x34   /* [4:0] high bits, 0x35 low byte   */
#define AXP_BAT_PERCENT     0xA4
static i2c_master_dev_handle_t s_pmu = NULL;

static esp_err_t pmu_rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, 50);
}

static esp_err_t pmu_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_pmu), TAG, "add PMU");
    uint8_t adc = 0;
    if (pmu_rd(AXP_ADC_CTRL, &adc) == ESP_OK) {
        uint8_t w[2] = { AXP_ADC_CTRL, (uint8_t)(adc | 0x01) };
        i2c_master_transmit(s_pmu, w, 2, 50);
    }
    return ESP_OK;
}

/* Returns false if the PMU can't be read. state: 'C' charging, 'D' discharging, 'S' standby/full */
static bool pmu_read(int *mv, int *pct, char *state)
{
    if (!s_pmu) return false;
    uint8_t s1 = 0, s2 = 0, h = 0, l = 0, p = 0;
    if (pmu_rd(AXP_STATUS1, &s1) != ESP_OK || pmu_rd(AXP_STATUS2, &s2) != ESP_OK) return false;
    pmu_rd(AXP_VBAT_H, &h); pmu_rd(AXP_VBAT_H + 1, &l); pmu_rd(AXP_BAT_PERCENT, &p);
    bool batt = s1 & 0x08;
    *mv  = batt ? (((h & 0x1F) << 8) | l) : 0;
    *pct = batt ? p : -1;
    int chg = (s2 >> 5) & 0x03;
    *state = chg == 1 ? 'C' : chg == 2 ? 'D' : 'S';
    return true;
}

/* ---------------------------------------------------------------------- */
/* Serial console: lets the host set the clock precisely over USB.        */
/*   SETTIME YYYY-MM-DD HH:MM:SS   -> sets RTC + system time, replies OK  */
/*   GETTIME                       -> replies with the current time       */
/* ---------------------------------------------------------------------- */
static void console_reply(const char *msg)
{
    usb_serial_jtag_write_bytes(msg, strlen(msg), pdMS_TO_TICKS(100));
}

static void console_handle_line(char *line)
{
    struct tm t = {0};
    char out[64];
    if (sscanf(line, "SETTIME %d-%d-%d %d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
        t.tm_year -= 1900;
        t.tm_mon  -= 1;
        struct tm tmp = t;
        mktime(&tmp);                  /* normalise + fill tm_wday */
        t = tmp;
        esp_err_t err = (s_rtc != NULL) ? rtc_write(&t) : ESP_ERR_INVALID_STATE;
        set_system_time(&t);
        snprintf(out, sizeof(out), "OK %04d-%02d-%02d %02d:%02d:%02d rtc=%s\n",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec, esp_err_to_name(err));
        console_reply(out);
    } else if (strncmp(line, "GETTIME", 7) == 0) {
        time_t now = time(NULL);
        localtime_r(&now, &t);
        snprintf(out, sizeof(out), "TIME %04d-%02d-%02d %02d:%02d:%02d\n",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
        console_reply(out);
    } else if (strncmp(line, "TIMETAP", 7) == 0) {
        if (bsp_display_lock(100)) { if (s_face != FACE_STOPWATCH) face_toggle(); bsp_display_unlock(); }
        console_reply("OK timetap\n");
    } else if (strncmp(line, "ASK ", 4) == 0) {
        if (bsp_display_lock(200)) { if (!s_screen_on) screen_set(true); claude_ask(line + 4); bsp_display_unlock(); }
        console_reply("OK asking\n");
    } else if (strncmp(line, "BOOT", 4) == 0) {
        claude_button();
        console_reply("OK boot\n");
    } else if (strncmp(line, "SCAN", 4) == 0) {
        net_scan(console_reply);
    } else if (strncmp(line, "NEXTNET", 7) == 0) {
        net_next();
        console_reply("OK switching network\n");
    } else if (strncmp(line, "WIFI", 4) == 0) {
        char out[96], ip[20];
        net_ip(ip, sizeof(ip));
        snprintf(out, sizeof(out), "WIFI %s ip %s rssi %d dBm, server %s\n", net_connected() ? "connected" : "down", ip, net_rssi(), net_health() ? "ok" : "unreachable");
        console_reply(out);
    } else if (strncmp(line, "HEAP", 4) == 0) {
        char out[96];
        snprintf(out, sizeof(out), "HEAP internal %u free (largest %u), dma %u free (largest %u), psram %u free\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        console_reply(out);
    } else if (strncmp(line, "BATT", 4) == 0) {
        int mv, pct; char st;
        char out[64];
        if (pmu_read(&mv, &pct, &st)) snprintf(out, sizeof(out), "BATT %d%% %d mV %s\n", pct, mv,
                                               st == 'C' ? "charging" : st == 'D' ? "discharging" : "standby");
        else snprintf(out, sizeof(out), "ERR pmu\n");
        console_reply(out);
    } else if (strncmp(line, "DUMP", 4) == 0) {
        uint32_t head = s_evlog_head;
        uint32_t n = head < EVLOG_N ? head : EVLOG_N;
        char out[64];
        snprintf(out, sizeof(out), "EVLOG %lu events (uptime %lld s)\n", (unsigned long)n, esp_timer_get_time() / 1000000);
        console_reply(out);
        for (uint32_t i = head - n; i < head; i++) {
            const evlog_entry_t *e = &s_evlog[i % EVLOG_N];
            snprintf(out, sizeof(out), "%8lu.%03lu %c %d %d\n", (unsigned long)(e->t_ms / 1000), (unsigned long)(e->t_ms % 1000), e->kind, e->a, e->b);
            console_reply(out);
            vTaskDelay(1);
        }
        console_reply("END\n");
    } else if (strncmp(line, "DTAP", 4) == 0) {
        if (bsp_display_lock(100)) { screen_set(!s_screen_on); bsp_display_unlock(); }
        console_reply("OK dtap\n");
    } else if (strncmp(line, "SWIPE LEFT", 10) == 0) {
        if (bsp_display_lock(100)) { if (s_face == FACE_HOME) face_set(FACE_STOPWATCH); bsp_display_unlock(); }
        console_reply("OK swipe left\n");
    } else if (strncmp(line, "SWIPE RIGHT", 11) == 0) {
        if (bsp_display_lock(100)) { if (s_face == FACE_STOPWATCH) face_set(FACE_HOME); bsp_display_unlock(); }
        console_reply("OK swipe right\n");
    } else if (strncmp(line, "TAP", 3) == 0) {
        if (bsp_display_lock(100)) { face_toggle(); bsp_display_unlock(); }
        console_reply("OK tap\n");
    } else if (strncmp(line, "RAW ON", 6) == 0) {
        s_raw_diff = true;   console_reply("OK raw on\n");
    } else if (strncmp(line, "RAW OFF", 7) == 0) {
        s_raw_diff = false;  console_reply("OK raw off\n");
    } else if (strncmp(line, "LOG ON", 6) == 0) {
        s_calib_log = true;  console_reply("OK log on\n");
    } else if (strncmp(line, "LOG OFF", 7) == 0) {
        s_calib_log = false; console_reply("OK log off\n");
    } else if (line[0] != '\0') {
        console_reply("ERR unknown command\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB serial console unavailable");
        vTaskDelete(NULL);
        return;
    }
    char line[64];
    size_t len = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            console_handle_line(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* UI                                                                     */
/* ---------------------------------------------------------------------- */
static lv_obj_t *s_time_label;
static lv_obj_t *s_ampm_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_batt_label;          /* CLOCK face: live battery line */
static volatile int  s_batt_pct = -1, s_batt_mv = 0;
static volatile char s_batt_state = 0;  /* C charging, D discharging, S standby, 0 unknown */

/* ---------------------------------------------------------------------- */
/* Power modes                                                            */
/*   screen ON : CPU pinned at 240 MHz, no light sleep, full-rate polling   */
/*   screen OFF: locks released -> DFS down to 80 MHz and automatic light   */
/*               sleep between events (not while USB is connected); LVGL   */
/*               clock timers paused, touch polled slower, IMU suspended.   */
/* ---------------------------------------------------------------------- */
static esp_pm_lock_handle_t s_lock_cpu_max, s_lock_no_sleep;
static TaskHandle_t  s_imu_task = NULL;
static lv_timer_t   *s_clock_timer = NULL, *s_sw_timer = NULL;
static volatile int  s_touch_poll_ms = 20;
#define TOUCH_POLL_ON_MS       20
#define TOUCH_POLL_OFF_MS      60
#define LVGL_INDEV_OFF_MS      60

static void power_init(void)
{
    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = true };
    esp_err_t err = esp_pm_configure(&pm);
    ESP_LOGI(TAG, "PM configure: %s", esp_err_to_name(err));
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui_cpu", &s_lock_cpu_max);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui_awake", &s_lock_no_sleep);
    esp_pm_lock_acquire(s_lock_cpu_max);
    esp_pm_lock_acquire(s_lock_no_sleep);
    /* a touch (INT low) wakes the chip from light sleep immediately */
    gpio_wakeup_enable(BSP_LCD_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

static void power_mode(bool on)
{
    static bool current = true;
    if (on == current) return;
    current = on;
    lv_indev_t *indev = bsp_display_get_input_dev();
    lv_timer_t *read_timer = indev ? lv_indev_get_read_timer(indev) : NULL;
    if (on) {
        esp_pm_lock_acquire(s_lock_cpu_max);
        esp_pm_lock_acquire(s_lock_no_sleep);
        if (s_clock_timer) lv_timer_resume(s_clock_timer);
        if (s_sw_timer)    lv_timer_resume(s_sw_timer);
        if (read_timer)    lv_timer_set_period(read_timer, LV_DEF_REFR_PERIOD);
        s_touch_poll_ms = TOUCH_POLL_ON_MS;
        if (s_imu_task) vTaskResume(s_imu_task);
    } else {
        if (s_clock_timer) lv_timer_pause(s_clock_timer);
        if (s_sw_timer)    lv_timer_pause(s_sw_timer);
        if (read_timer)    lv_timer_set_period(read_timer, LVGL_INDEV_OFF_MS);
        s_touch_poll_ms = TOUCH_POLL_OFF_MS;
        if (s_imu_task) vTaskSuspend(s_imu_task);
        esp_pm_lock_release(s_lock_no_sleep);
        esp_pm_lock_release(s_lock_cpu_max);
    }
    ESP_LOGI(TAG, "Power mode: %s", on ? "awake" : "low-power");
}

/* ---------------------------------------------------------------------- */
/* Screen power                                                           */
/* ---------------------------------------------------------------------- */
static void screen_set(bool on)
{
    s_screen_on = on;
    if (on) {
        power_mode(true);
        clock_tick(NULL);          /* timers were paused: refresh before showing */
        bsp_display_backlight_on();
    } else {
        bsp_display_backlight_off();
        face_reset_instant();      /* next wake always starts on the photo face */
        power_mode(false);
    }
    ESP_LOGI(TAG, "Screen %s", on ? "on" : "off");
    evlog_add('S', on ? 1 : 0, 0);
}

static void screen_press_cb(lv_event_t *e)
{
    (void)e;
}

/* Tap on the background: reserved for future UI actions; currently just logs. */
static void screen_tap_cb(lv_event_t *e)
{
    (void)e;
    if (touch_quiet()) return;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    ESP_LOGI(TAG, "Tap at (%d, %d)", (int)pt.x, (int)pt.y);
    evlog_add('T', pt.x, pt.y);
    corner_tap(pt.x, pt.y);
}

/* Tap on the time itself: photo face <-> black clock face. */
static void time_tap_cb(lv_event_t *e)
{
    (void)e;
    if (!s_screen_on || touch_quiet()) return;
    if (s_face == FACE_STOPWATCH) return;
    ESP_LOGI(TAG, "Time tapped");
    face_toggle();
}

/* Drag right-to-left on the photo face: stopwatch. Drag left-to-right: back. */
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_indev_wait_release(indev);          /* don't let the swipe also register as a click */
    if (!s_screen_on || touch_quiet()) return;
    switch (dir) {
    case LV_DIR_LEFT:
        ESP_LOGI(TAG, "Swipe left");
        if (s_face == FACE_HOME) face_set(FACE_STOPWATCH);
        break;
    case LV_DIR_RIGHT:
        ESP_LOGI(TAG, "Swipe right");
        if (s_face == FACE_STOPWATCH) face_set(FACE_HOME);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Screen on/off: double-tap the bottom-right corner of the glass.         */
/* ---------------------------------------------------------------------- */
#define FT3168_ADDR            0x38
#define FT_REG_TD_STATUS       0x02
#define FT_MAX_POINTS          5
#define SCREEN_W               410
#define SCREEN_H               502
#define IMU_BOOT_IGNORE_MS     1500   /* first IMU samples after boot are junk   */
#define TP_RST_GPIO            BSP_LCD_TOUCH_RST
#define TP_WATCHDOG_MS         10000   /* controller silent this long -> reset it */
static i2c_master_dev_handle_t s_tp_dev = NULL;

/* Program the FT3168 the way we want it: driver thresholds, and auto-monitor
 * mode OFF (0x86=0) so it never dozes and swallows the first tap of a double tap. */
static void tp_configure(const char *why)
{
    if (!s_tp_dev) return;
    const uint8_t init[][2] = { {0x80, 70}, {0x81, 60}, {0x82, 16}, {0x83, 60}, {0x84, 10}, {0x85, 20},
                                {0x86, 0x00}, {0x87, 0xFF}, {0xA5, 0x00} };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        i2c_master_transmit(s_tp_dev, init[i], 2, 50);
    }
    uint8_t r = 0x86, ctrl[2] = {0xFF, 0xFF};
    i2c_master_transmit_receive(s_tp_dev, &r, 1, ctrl, 2, 50);
    ESP_LOGI(TAG, "FT3168 configured (%s): ctrl=%02x t_mon=%02x", why, ctrl[0], ctrl[1]);
}

/* Hardware reset of the touch controller (GPIO 9; verified harmless to the display). */
static void tp_reset(const char *why)
{
    ESP_LOGW(TAG, "Touch controller reset (%s)", why);
    evlog_add('R', 0, 0);
    gpio_set_level(TP_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    tp_configure("after reset");
}

#define CORNER_X_MIN           (SCREEN_W * 60 / 100)   /* right 40 % of the width   */
#define CORNER_Y_MIN           (SCREEN_H * 70 / 100)   /* bottom 30 % of the height */
#define DTAP_MAX_GAP_MS        500
static int64_t s_corner_tap1_us = -1;

static void corner_tap(int x, int y)
{
    if (x < CORNER_X_MIN || y < CORNER_Y_MIN) {
        s_corner_tap1_us = -1;          /* a tap elsewhere breaks the sequence */
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_corner_tap1_us >= 0 && now - s_corner_tap1_us <= DTAP_MAX_GAP_MS * 1000LL) {
        s_corner_tap1_us = -1;
        s_touch_quiet_until_us = now + TOUCH_QUIET_AFTER_TOGGLE_MS * 1000LL;
        ESP_LOGI(TAG, "DOUBLE-TAP bottom-right -> screen %s", s_screen_on ? "off" : "on");
        evlog_add('D', s_screen_on ? 0 : 1, 0);
        screen_set(!s_screen_on);       /* called from the LVGL task: already locked */
    } else {
        s_corner_tap1_us = now;
    }
}

/* s_raw_diff: "RAW ON|OFF" reports FT3168 registers that change vs idle */

static void i2c_recover(int *errors, const char *who)
{
    if (++(*errors) == 5) {
        ESP_LOGW(TAG, "%s: 5 consecutive I2C failures, resetting bus", who);
        i2c_master_bus_reset(bsp_i2c_get_handle());
        vTaskDelay(pdMS_TO_TICKS(100));
        *errors = 0;
    }
}

static void touch_poll_task(void *arg)
{
    (void)arg;
    i2c_master_dev_handle_t tp = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT3168_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &tp) != ESP_OK) {
        ESP_LOGW(TAG, "Touch poll: cannot add FT3168 device");
        vTaskDelete(NULL);
        return;
    }

    s_tp_dev = tp;
    tp_configure("boot");

    uint8_t reg = 0x01;                         /* gesture, td_status, then 6 bytes per point */
    uint8_t buf[2 + 6 * FT_MAX_POINTS];
    bool was_touching = false;
    int errors = 0;
    int64_t err_start_us = 0;
    int64_t contact_start_us = 0;
    int contact_polls = 0, contact_max_n = 0, contact_minx = 9999, contact_maxx = -1, contact_miny = 9999, contact_maxy = -1;


    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_touch_poll_ms));
        esp_err_t rd = i2c_master_transmit_receive(tp, &reg, 1, buf, sizeof(buf), 50);
        if (rd != ESP_OK) {
            if (errors++ == 0) {
                err_start_us = esp_timer_get_time();
                ESP_LOGW(TAG, "TPERR start: %s", esp_err_to_name(rd));
                evlog_add('E', (int)(rd & 0x7FFF), 0);
            }
            int64_t now = esp_timer_get_time();
            if (now - err_start_us > TP_WATCHDOG_MS * 1000LL) {
                evlog_add('e', (int)((now - err_start_us) / 1000), errors);
                if (rd == ESP_ERR_TIMEOUT) {
                    ESP_LOGW(TAG, "I2C timeouts: resetting bus");
                    i2c_master_bus_reset(bsp_i2c_get_handle());
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                tp_reset("watchdog");
                errors = 0;
            }
            continue;
        }
        if (errors > 0) {
            int64_t end_us = esp_timer_get_time();
            int64_t q = end_us + TOUCH_QUIET_AFTER_NACK_MS * 1000LL;
            if (q > s_touch_quiet_until_us) s_touch_quiet_until_us = q;
            ESP_LOGW(TAG, "TPERR end: %d failures over %lld ms", errors, (end_us - err_start_us) / 1000);
            evlog_add('e', (int)((end_us - err_start_us) / 1000), errors);
        }
        /* The FT3168 returns one constant byte across the whole frame while a palm
         * covers the glass (palm rejection). Log it; do not treat it as a bus fault. */
        bool constant = true;
        for (size_t i = 1; i < sizeof(buf); i++) if (buf[i] != buf[0]) { constant = false; break; }
        errors = 0;
        if (constant && buf[0] != 0x00) {
            static int64_t last_const_log_us = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_const_log_us > 250000) {
                ESP_LOGI(TAG, "PALMFRAME byte=0x%02x", buf[0]);
                last_const_log_us = now;
            }
            continue;
        }

        uint8_t gesture = buf[0], status = buf[1];
        int n = status & 0x0F;
        if (n > FT_MAX_POINTS) n = 0;
        /* Corrupt frame (seen as all-0xFF while the controller recovers): drop it. */
        bool corrupt = false;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = &buf[2 + i * 6];
            int x = ((p[0] & 0x0F) << 8) | p[1];
            int y = ((p[2] & 0x0F) << 8) | p[3];
            if (x >= SCREEN_W || y >= SCREEN_H) corrupt = true;
        }
        if (corrupt) continue;

        if (n == 0) {
            if (was_touching && s_calib_log) {
                ESP_LOGI(TAG, "CONTACT end: %lld ms, %d polls, max_n=%d, travel=%dx%d",
                         (esp_timer_get_time() - contact_start_us) / 1000, contact_polls, contact_max_n,
                         contact_maxx - contact_minx, contact_maxy - contact_miny);
            }
            was_touching = false;
            continue;
        }

        if (!was_touching) {
            contact_start_us = esp_timer_get_time();
            {
                const uint8_t *p0 = &buf[2];
                evlog_add('C', ((p0[0] & 0x0F) << 8) | p0[1], ((p0[2] & 0x0F) << 8) | p0[3]);
            }
            contact_polls = 0; contact_max_n = 0;
            contact_minx = 9999; contact_maxx = -1; contact_miny = 9999; contact_maxy = -1;
        }
        was_touching = true;
        contact_polls++;
        if (n > contact_max_n) contact_max_n = n;

        char detail[96] = "";
        int pos = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = &buf[2 + i * 6];
            int x = ((p[0] & 0x0F) << 8) | p[1];
            int y = ((p[2] & 0x0F) << 8) | p[3];
            int ev = p[0] >> 6, id = p[2] >> 4;
            if (x < contact_minx) contact_minx = x;
            if (x > contact_maxx) contact_maxx = x;
            if (y < contact_miny) contact_miny = y;
            if (y > contact_maxy) contact_maxy = y;
            if (pos < (int)sizeof(detail) - 20) {
                pos += snprintf(detail + pos, sizeof(detail) - pos, " [%d:%d,%d e%d w%d a%d]", id, x, y, ev, p[4], p[5] >> 4);
            }
        }
        if (s_calib_log && contact_polls == 1) {
            ESP_LOGI(TAG, "TOUCH st=0x%02x g=0x%02x n=%d%s", status, gesture, n, detail);
        }

    }
}

static void imu_task(void *arg)
{
    (void)arg;
    static qmi8658_dev_t dev;
    if (qmi8658_init(&dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed");
        vTaskDelete(NULL);
        return;
    }
    qmi8658_set_accel_range(&dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&dev, QMI8658_ACCEL_ODR_500HZ);

    int64_t last_log_us = 0;
    float window_peak = 0;
    float px = 0, py = 0, pz = 0;
    int stale = 0, errors = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(8));
        float x, y, z;
        if (qmi8658_read_accel(&dev, &x, &y, &z) != ESP_OK) { i2c_recover(&errors, "imu"); continue; }
        float mag = sqrtf(x * x + y * y + z * z) / 1000.0f;   /* driver reports milli-g */
        /* A wedged bus returns frozen or absurd samples: a live sensor at 500 Hz never
         * repeats the exact same reading many times, and |a| stays within the 8 g range. */
        if (x == px && y == py && z == pz) { if (++stale >= 10) { i2c_recover(&errors, "imu"); stale = 0; } continue; }
        stale = 0; errors = 0; px = x; py = y; pz = z;
        if (mag < 0.2f || mag > 7.9f) continue;
        float dev_g = fabsf(mag - 1.0f);
        if (esp_timer_get_time() < IMU_BOOT_IGNORE_MS * 1000LL) continue;
        if (dev_g > window_peak) window_peak = dev_g;

        int64_t now = esp_timer_get_time();
        if (s_calib_log && now - last_log_us > 500000) {   /* twice a second */
            if (window_peak >= 0.15f) {
                ESP_LOGI(TAG, "ACCEL peak %.2f g (last 0.5 s)", window_peak);
            }
            window_peak = 0;
            last_log_us = now;
        }
    }
}

/* Faces:
 *   FACE_HOME      : random photo, time (no seconds) tucked at the top   (boot default)
 *   FACE_CLOCK     : black, "Hello Luke", big time with seconds, AM/PM, date
 *   FACE_STOPWATCH : black sheet slid in from the right over the photo face
 * Tapping the time flips HOME <-> CLOCK with an animation. */

#define HOME_BG_COLOR        0x1B3A5C     /* solid background on the home face */
#define FACE_ANIM_MS         450
#define TIME_Y_CLOCK         (-10)        /* offset from centre on the clock face */
#define TIME_Y_HOME          28           /* offset from top on the home face    */

static lv_obj_t *s_hello_label;
static lv_obj_t *s_photo;               /* full-screen image behind everything on the home face */
static int       s_photo_idx = -1;

/* Embedded pictures (see main/CMakeLists.txt). Symbols are _binary_pic_NN_bin_start. */
#define PIC_SYM(n) _binary_pic_##n##_bin_start
#define PIC_DECL(n) extern const uint8_t PIC_SYM(n)[];
#define PIC_REF(n)  PIC_SYM(n),
#if PIC_COUNT > 0
PIC_DECL(00)
#endif
#if PIC_COUNT > 1
PIC_DECL(01)
#endif
#if PIC_COUNT > 2
PIC_DECL(02)
#endif
#if PIC_COUNT > 3
PIC_DECL(03)
#endif
#if PIC_COUNT > 4
PIC_DECL(04)
#endif
#if PIC_COUNT > 5
PIC_DECL(05)
#endif
#if PIC_COUNT > 6
PIC_DECL(06)
#endif
#if PIC_COUNT > 7
PIC_DECL(07)
#endif
#if PIC_COUNT > 8
PIC_DECL(08)
#endif
#if PIC_COUNT > 9
PIC_DECL(09)
#endif
#if PIC_COUNT > 10
PIC_DECL(10)
#endif
#if PIC_COUNT > 11
PIC_DECL(11)
#endif
#if PIC_COUNT > 12
#error "add more PIC_DECL/PIC_REF entries for additional pictures"
#endif
static const uint8_t *const s_pic_data[] = {
#if PIC_COUNT > 0
    PIC_REF(00)
#endif
#if PIC_COUNT > 1
    PIC_REF(01)
#endif
#if PIC_COUNT > 2
    PIC_REF(02)
#endif
#if PIC_COUNT > 3
    PIC_REF(03)
#endif
#if PIC_COUNT > 4
    PIC_REF(04)
#endif
#if PIC_COUNT > 5
    PIC_REF(05)
#endif
#if PIC_COUNT > 6
    PIC_REF(06)
#endif
#if PIC_COUNT > 7
    PIC_REF(07)
#endif
#if PIC_COUNT > 8
    PIC_REF(08)
#endif
#if PIC_COUNT > 9
    PIC_REF(09)
#endif
#if PIC_COUNT > 10
    PIC_REF(10)
#endif
#if PIC_COUNT > 11
    PIC_REF(11)
#endif
};
static lv_image_dsc_t s_pic_dsc[PIC_COUNT > 0 ? PIC_COUNT : 1];

static void pics_init(void)
{
    for (int i = 0; i < PIC_COUNT; i++) {
        s_pic_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_pic_dsc[i].header.cf     = LV_COLOR_FORMAT_RGB565;
        s_pic_dsc[i].header.flags  = 0;
        s_pic_dsc[i].header.w      = SCREEN_W;
        s_pic_dsc[i].header.h      = SCREEN_H;
        s_pic_dsc[i].header.stride = SCREEN_W * 2;
        s_pic_dsc[i].data_size     = SCREEN_W * SCREEN_H * 2;
        s_pic_dsc[i].data          = s_pic_data[i];
    }
}

/* Pick a random picture, never the same one twice in a row. */
static void photo_pick_random(void)
{
    if (PIC_COUNT == 0) return;
    int idx = s_photo_idx;
    if (PIC_COUNT > 1) {
        while (idx == s_photo_idx) idx = (int)(esp_random() % PIC_COUNT);
    } else {
        idx = 0;
    }
    s_photo_idx = idx;
    lv_image_set_src(s_photo, &s_pic_dsc[idx]);
    ESP_LOGI(TAG, "Home photo #%d", idx);
}

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    int hour12 = t.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = t.tm_hour < 12 ? "AM" : "PM";

    char buf[40];
    if (s_face == FACE_CLOCK) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", hour12, t.tm_min, t.tm_sec);
        lv_label_set_text(s_time_label, buf);
        lv_label_set_text(s_ampm_label, ampm);
        strftime(buf, sizeof(buf), "%A, %b %d %Y", &t);
        lv_label_set_text(s_date_label, buf);
        if (s_batt_pct < 0) {
            lv_label_set_text(s_batt_label, "Battery --");
        } else {
            snprintf(buf, sizeof(buf), "%s %d%%  %d.%02d V", s_batt_state == 'C' ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL,
                     s_batt_pct, s_batt_mv / 1000, (s_batt_mv % 1000) / 10);
            lv_label_set_text(s_batt_label, buf);
        }
        lv_obj_set_style_text_color(s_batt_label,
            s_batt_state == 'C' ? lv_color_hex(0x81C784) : s_batt_pct >= 0 && s_batt_pct <= 15 ? lv_color_hex(0xEF5350) : lv_color_hex(0xB0BEC5), 0);
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d %s", hour12, t.tm_min, ampm);
        lv_label_set_text(s_time_label, buf);
    }
}

/* ---- animation helpers ---- */
static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_hide_when_done(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void anim_time_y_cb(void *obj, int32_t v)
{
    /* v is the label's absolute y; keep it horizontally centred */
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_obj_set_pos(o, (SCREEN_W - lv_obj_get_width(o)) / 2, v);
}

static void anim_bg_cb(void *obj, int32_t v)
{
    lv_color_t c = lv_color_mix(lv_color_hex(HOME_BG_COLOR), lv_color_black(), (lv_opa_t)v);
    lv_obj_set_style_bg_color((lv_obj_t *)obj, c, 0);
}

static void fade(lv_obj_t *obj, bool in)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_duration(&a, FACE_ANIM_MS);
    if (in) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_anim_set_values(&a, 0, 255);
    } else {
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_completed_cb(&a, anim_hide_when_done);
    }
    lv_anim_start(&a);
}

static int32_t time_label_target_y(face_t face)
{
    int32_t h = lv_obj_get_height(s_time_label);
    return face == FACE_HOME ? TIME_Y_HOME : (SCREEN_H - h) / 2 + TIME_Y_CLOCK;
}

/* ---------------------------------------------------------------------- */
/* Stopwatch face                                                         */
/* ---------------------------------------------------------------------- */
static lv_obj_t *s_sw_panel, *s_sw_label, *s_sw_startstop_label;
static bool      s_sw_running = false;
static int64_t   s_sw_accum_us = 0;      /* time accumulated while stopped   */
static int64_t   s_sw_start_us = 0;      /* esp_timer value at last start     */

static int64_t sw_elapsed_us(void)
{
    return s_sw_accum_us + (s_sw_running ? esp_timer_get_time() - s_sw_start_us : 0);
}

static void sw_render(void)
{
    int64_t ms = sw_elapsed_us() / 1000;
    int tenths = (ms / 100) % 10;
    int secs   = (ms / 1000) % 60;
    int mins   = (ms / 60000) % 60;
    int hours  = (int)(ms / 3600000);
    char buf[24];
    if (hours > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d.%d", hours, mins, secs, tenths);
    else           snprintf(buf, sizeof(buf), "%02d:%02d.%d", mins, secs, tenths);
    lv_label_set_text(s_sw_label, buf);
}

static void sw_tick(lv_timer_t *t)
{
    (void)t;
    if (s_sw_running) sw_render();
}

static void sw_startstop_cb(lv_event_t *e)
{
    (void)e;
    if (s_sw_running) {
        s_sw_accum_us += esp_timer_get_time() - s_sw_start_us;
        s_sw_running = false;
        lv_label_set_text(s_sw_startstop_label, "Start");
    } else {
        s_sw_start_us = esp_timer_get_time();
        s_sw_running = true;
        lv_label_set_text(s_sw_startstop_label, "Stop");
    }
    ESP_LOGI(TAG, "Stopwatch %s", s_sw_running ? "started" : "stopped");
    sw_render();
}

static void sw_reset_cb(lv_event_t *e)
{
    (void)e;
    s_sw_running = false;
    s_sw_accum_us = 0;
    lv_label_set_text(s_sw_startstop_label, "Start");
    ESP_LOGI(TAG, "Stopwatch reset");
    sw_render();
}

static lv_obj_t *sw_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 150, 64);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 32, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    if (out_label) *out_label = lbl;
    return btn;
}

static void sw_build(lv_obj_t *scr)
{
    /* Full-screen solid black sheet that slides in from the right and covers the photo. */
    s_sw_panel = lv_obj_create(scr);
    lv_obj_set_size(s_sw_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_sw_panel, SCREEN_W, 0);
    lv_obj_set_style_bg_color(s_sw_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sw_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sw_panel, 0, 0);
    lv_obj_set_style_border_width(s_sw_panel, 0, 0);
    lv_obj_set_style_pad_all(s_sw_panel, 0, 0);
    lv_obj_remove_flag(s_sw_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sw_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_sw_panel, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_sw_panel);
    lv_label_set_text(title, "Stopwatch");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xB0BEC5), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 110);   /* below the time strip */

    s_sw_label = lv_label_create(s_sw_panel);
    lv_obj_set_style_text_font(s_sw_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_sw_label, lv_color_white(), 0);
    lv_obj_align(s_sw_label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *b1 = sw_button(s_sw_panel, "Start", 0x2E7D32, sw_startstop_cb, &s_sw_startstop_label);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_t *b2 = sw_button(s_sw_panel, "Reset", 0x546E7A, sw_reset_cb, NULL);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 0, -30);

    sw_render();
    s_sw_timer = lv_timer_create(sw_tick, 100, NULL);

    lv_obj_move_foreground(s_time_label);   /* time stays visible above the black sheet */
}

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void sw_slide(bool in)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sw_panel);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_duration(&a, FACE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (in) {
        lv_obj_remove_flag(s_sw_panel, LV_OBJ_FLAG_HIDDEN);
        lv_anim_set_values(&a, SCREEN_W, 0);
    } else {
        lv_anim_set_values(&a, 0, SCREEN_W);
        lv_anim_set_completed_cb(&a, anim_hide_when_done);
    }
    lv_anim_start(&a);
}

/* ---------------------------------------------------------------------- */
/* Face transitions                                                       */
/* ---------------------------------------------------------------------- */
static void face_set(face_t face)
{
    if (face == s_face) return;
    face_t prev = s_face;
    s_face = face;
    ESP_LOGI(TAG, "Face -> %s", face == FACE_HOME ? "home" : face == FACE_STOPWATCH ? "stopwatch" : "clock");

    /* Stopwatch panel slides over the home face; nothing else changes. */
    if (face == FACE_STOPWATCH) { sw_slide(true);  return; }
    if (prev == FACE_STOPWATCH) { sw_slide(false); return; }

    lv_obj_t *scr = lv_screen_active();
    bool home = (face == FACE_HOME);

    /* text first so the label has its new width before we animate position */
    clock_tick(NULL);
    lv_obj_update_layout(s_time_label);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_time_label);
    lv_anim_set_exec_cb(&a, anim_time_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_time_label), time_label_target_y(face));
    lv_anim_set_duration(&a, FACE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    if (PIC_COUNT > 0) {
        /* photo stays; a fresh one is picked only on wake (face_reset_instant) */
        fade(s_photo, home);
    } else {
        lv_anim_init(&a);
        lv_anim_set_var(&a, scr);
        lv_anim_set_exec_cb(&a, anim_bg_cb);
        lv_anim_set_values(&a, home ? 0 : 255, home ? 255 : 0);
        lv_anim_set_duration(&a, FACE_ANIM_MS);
        lv_anim_start(&a);
    }

    fade(s_hello_label, !home);
    fade(s_date_label,  !home);
    fade(s_ampm_label,  !home);
    fade(s_batt_label,  !home);
}

/* Jump straight to the photo face with a fresh picture, no animation (used while dark). */
static void face_reset_instant(void)
{
    lv_anim_delete(s_time_label, NULL);
    lv_anim_delete(s_hello_label, NULL);
    lv_anim_delete(s_date_label, NULL);
    lv_anim_delete(s_ampm_label, NULL);
    lv_anim_delete(s_batt_label, NULL);
    lv_anim_delete(s_photo, NULL);
    lv_anim_delete(s_sw_panel, NULL);
    lv_anim_delete(lv_screen_active(), NULL);

    s_face = FACE_HOME;
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);

    if (PIC_COUNT > 0) {
        photo_pick_random();
        lv_obj_remove_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_photo, 255, 0);
    }
    lv_obj_set_x(s_sw_panel, SCREEN_W);
    lv_obj_add_flag(s_sw_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hide[] = { s_hello_label, s_date_label, s_ampm_label, s_batt_label };
    for (size_t i = 0; i < sizeof(hide) / sizeof(hide[0]); i++) {
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(hide[i], 0, 0);
    }

    clock_tick(NULL);
    lv_obj_update_layout(s_time_label);
    anim_time_y_cb(s_time_label, time_label_target_y(FACE_HOME));
}

static void face_toggle(void)
{
    face_set(s_face == FACE_CLOCK ? FACE_HOME : FACE_CLOCK);
}

/* ---------------------------------------------------------------------- */
/* Claude overlay (WATCH_DESIGN.md section 1)                              */
/*   LISTENING : orange ring around the edge                                */
/*   WORKING   : the ring pulses while the Pi asks Claude                   */
/*   ANSWER    : orange fills the screen, answer in a scrollable box        */
/* Tap the screen or press BOOT to dismiss.                                 */
/* ---------------------------------------------------------------------- */
#define CLAUDE_ORANGE        0xD97757
#define CLAUDE_BOX_BG        0xFFF4EC
#define CLAUDE_RING_RADIUS   120    /* match the glass's rounded corners */
#define CLAUDE_RING_WIDTH    16
#define BOOT_BUTTON_GPIO     GPIO_NUM_0

typedef enum { CL_IDLE = 0, CL_MENU, CL_LISTENING, CL_WORKING, CL_ANSWER } claude_state_t;
static claude_state_t s_cl_state = CL_IDLE;
static lv_obj_t *s_cl_layer, *s_cl_ring, *s_cl_box, *s_cl_text, *s_cl_menu;
#define CLAUDE_DIM_OPA       LV_OPA_70   /* how much the photo is darkened behind the menu */
static lv_anim_t s_cl_pulse;

static void anim_border_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void claude_set_state(claude_state_t st, const char *text)
{
    s_cl_state = st;
    lv_anim_delete(s_cl_ring, anim_border_opa_cb);
    switch (st) {
    case CL_IDLE:
        lv_obj_add_flag(s_cl_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cl_menu, LV_OBJ_FLAG_HIDDEN);
        break;
    case CL_MENU:
        lv_obj_remove_flag(s_cl_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_cl_layer, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_cl_layer, CLAUDE_DIM_OPA, 0);
        lv_obj_add_flag(s_cl_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cl_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_opa(s_cl_ring, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cl_menu, LV_OBJ_FLAG_HIDDEN);
        break;
    case CL_LISTENING:
    case CL_WORKING:
        lv_obj_remove_flag(s_cl_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_cl_layer, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_cl_layer, CLAUDE_DIM_OPA, 0);
        lv_obj_add_flag(s_cl_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cl_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cl_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_opa(s_cl_ring, LV_OPA_COVER, 0);
        if (st == CL_WORKING) {
            lv_anim_init(&s_cl_pulse);
            lv_anim_set_var(&s_cl_pulse, s_cl_ring);
            lv_anim_set_exec_cb(&s_cl_pulse, anim_border_opa_cb);
            lv_anim_set_values(&s_cl_pulse, 255, 60);
            lv_anim_set_duration(&s_cl_pulse, 600);
            lv_anim_set_playback_duration(&s_cl_pulse, 600);
            lv_anim_set_repeat_count(&s_cl_pulse, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&s_cl_pulse);
        }
        break;
    case CL_ANSWER:
        lv_obj_remove_flag(s_cl_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cl_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cl_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_cl_layer, lv_color_hex(CLAUDE_ORANGE), 0);
        lv_obj_set_style_bg_opa(s_cl_layer, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cl_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_cl_text, text ? text : "");
        lv_obj_scroll_to_y(s_cl_box, 0, LV_ANIM_OFF);
        break;
    }
}

static void claude_layer_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_cl_state == CL_ANSWER) {
        ESP_LOGI(TAG, "Answer dismissed");
        claude_set_state(CL_IDLE, NULL);
    }
}

/* Runs in its own task: asks the Pi, then shows the answer. */
static void ask_task(void *arg)
{
    char *question = (char *)arg;
    char *answer = NULL;
    int status = 0;
    ESP_LOGI(TAG, "Asking: %s", question);
    esp_err_t err = net_ask(question, &answer, &status);
    ESP_LOGI(TAG, "Answer (%s, http %d): %s", esp_err_to_name(err), status, answer ? answer : "");
    evlog_add('Q', (int)(err == ESP_OK), status);
    if (bsp_display_lock(500)) {
        if (!s_screen_on) screen_set(true);
        claude_set_state(CL_ANSWER, answer);
        bsp_display_unlock();
    }
    free(answer);
    free(question);
    vTaskDelete(NULL);
}

static void claude_ask(const char *question)
{
    if (s_cl_state == CL_WORKING) return;
    claude_set_state(CL_WORKING, NULL);
    xTaskCreateWithCaps(ask_task, "ask", 8192, strdup(question), 3, NULL, MALLOC_CAP_SPIRAM);
}

/* BOOT button: opens the Claude menu; pressing it again (from any Claude state)
 * closes everything and returns to the watch face. */
static void claude_button(void)
{
    if (!bsp_display_lock(200)) return;
    if (!s_screen_on) screen_set(true);
    if (s_cl_state == CL_IDLE) {
        ESP_LOGI(TAG, "BOOT: menu");
        claude_set_state(CL_MENU, NULL);
    } else if (s_cl_state != CL_WORKING) {
        ESP_LOGI(TAG, "BOOT: close");
        claude_set_state(CL_IDLE, NULL);
    }
    bsp_display_unlock();
}

static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int stable = 1, last = 1;
    int64_t changed_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int v = gpio_get_level(BOOT_BUTTON_GPIO);
        int64_t now = esp_timer_get_time();
        if (v != last) { last = v; changed_us = now; continue; }
        if (v != stable && now - changed_us > 40000) {      /* 40 ms debounce */
            stable = v;
            if (stable == 0) claude_button();                /* active low */
        }
    }
}

static const char *const s_menu_questions[] = {
    "What's today's date?",
    "What's the weather right now?",
    "What's my latest email about? One sentence.",
    "What's on my calendar today?",
};
static const char *const s_menu_labels[] = { "Date", "Weather", "Latest email", "Calendar today" };

static void menu_button_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Menu: %s", s_menu_labels[idx]);
    claude_ask(s_menu_questions[idx]);
}

static void claude_build(lv_obj_t *scr)
{
    s_cl_layer = lv_obj_create(scr);
    lv_obj_set_size(s_cl_layer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_cl_layer, 0, 0);
    lv_obj_set_style_bg_color(s_cl_layer, lv_color_hex(CLAUDE_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_cl_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cl_layer, 0, 0);
    lv_obj_set_style_radius(s_cl_layer, 0, 0);
    lv_obj_set_style_pad_all(s_cl_layer, 0, 0);
    lv_obj_remove_flag(s_cl_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cl_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cl_layer, claude_layer_tap_cb, LV_EVENT_CLICKED, NULL);

    s_cl_ring = lv_obj_create(s_cl_layer);
    lv_obj_set_size(s_cl_ring, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_cl_ring, 0, 0);
    lv_obj_set_style_bg_opa(s_cl_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_cl_ring, lv_color_hex(CLAUDE_ORANGE), 0);
    lv_obj_set_style_border_width(s_cl_ring, CLAUDE_RING_WIDTH, 0);
    lv_obj_set_style_radius(s_cl_ring, CLAUDE_RING_RADIUS, 0);
    lv_obj_set_style_pad_all(s_cl_ring, 0, 0);
    lv_obj_remove_flag(s_cl_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_cl_box = lv_obj_create(s_cl_layer);
    lv_obj_set_size(s_cl_box, SCREEN_W - 50, SCREEN_H - 110);
    lv_obj_center(s_cl_box);
    lv_obj_set_style_bg_color(s_cl_box, lv_color_hex(CLAUDE_BOX_BG), 0);
    lv_obj_set_style_bg_opa(s_cl_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cl_box, 0, 0);
    lv_obj_set_style_radius(s_cl_box, 26, 0);
    lv_obj_set_style_pad_all(s_cl_box, 18, 0);
    lv_obj_set_scroll_dir(s_cl_box, LV_DIR_VER);
    lv_obj_add_flag(s_cl_box, LV_OBJ_FLAG_EVENT_BUBBLE);  /* taps on the box also dismiss */

    s_cl_menu = lv_obj_create(s_cl_layer);
    lv_obj_set_size(s_cl_menu, SCREEN_W - 2 * CLAUDE_RING_WIDTH - 20, LV_SIZE_CONTENT);
    lv_obj_center(s_cl_menu);
    lv_obj_set_style_bg_opa(s_cl_menu, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cl_menu, 0, 0);
    lv_obj_set_style_pad_all(s_cl_menu, 0, 0);
    lv_obj_set_style_pad_row(s_cl_menu, 16, 0);
    lv_obj_set_flex_flow(s_cl_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cl_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_cl_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cl_menu, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < (int)(sizeof(s_menu_labels) / sizeof(s_menu_labels[0])); i++) {
        lv_obj_t *btn = lv_button_create(s_cl_menu);
        lv_obj_set_size(btn, 290, 66);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CLAUDE_ORANGE), 0);
        lv_obj_set_style_radius(btn, 33, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, menu_button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_menu_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
    }

    s_cl_text = lv_label_create(s_cl_box);
    lv_obj_set_width(s_cl_text, SCREEN_W - 50 - 36);
    lv_label_set_long_mode(s_cl_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_cl_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cl_text, lv_color_hex(0x2B2118), 0);
    lv_obj_set_style_text_line_space(s_cl_text, 6, 0);
    lv_label_set_text(s_cl_text, "");
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);   /* AMOLED: black = off */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, screen_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   /* let swipes become gestures, not scrolls */

    pics_init();
    s_photo = lv_image_create(scr);
    lv_obj_set_size(s_photo, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_photo, 0, 0);
    lv_obj_remove_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);   /* gestures/taps go to the screen */
    lv_obj_set_style_opa(s_photo, 0, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_hello_label = lv_label_create(scr);
    lv_label_set_text(s_hello_label, "Hello Luke");
    lv_obj_set_style_text_font(s_hello_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_hello_label, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(s_hello_label, LV_ALIGN_TOP_MID, 0, 110);

    s_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_time_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time_label, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_time_label, 14, 0);
    lv_obj_set_style_pad_hor(s_time_label, 14, 0);
    lv_obj_set_style_pad_ver(s_time_label, 6, 0);
    lv_obj_add_flag(s_time_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_time_label, time_tap_cb, LV_EVENT_CLICKED, NULL);

    s_ampm_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_ampm_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ampm_label, lv_color_hex(0xB0BEC5), 0);
    lv_obj_align(s_ampm_label, LV_ALIGN_CENTER, 0, 35);

    s_date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xB0BEC5), 0);
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 80);

    s_batt_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(s_batt_label, "Battery --");
    lv_obj_align(s_batt_label, LV_ALIGN_CENTER, 0, 125);

    s_clock_timer = lv_timer_create(clock_tick, 250, NULL);
    sw_build(scr);

    claude_build(scr);

    s_face = FACE_CLOCK;          /* so the reset below actually lays out HOME */
    face_reset_instant();
}

static void battery_task(void *arg)
{
    (void)arg;
    if (pmu_init() != ESP_OK) { ESP_LOGW(TAG, "PMU init failed"); vTaskDelete(NULL); return; }
    char last_state = 0;
    int64_t last_log_us = -600LL * 1000000;
    for (;;) {
        int mv, pct; char st;
        if (pmu_read(&mv, &pct, &st)) {
            s_batt_pct = pct; s_batt_mv = mv; s_batt_state = st;   /* read by clock_tick on the CLOCK face */
            int64_t now = esp_timer_get_time();
            if (st != last_state) {
                evlog_add('P', st, pct);
                ESP_LOGI(TAG, "Battery %d%% %d mV, %s", pct, mv, st == 'C' ? "charging" : st == 'D' ? "discharging" : "standby");
                last_state = st;
            }
            if (now - last_log_us >= 300LL * 1000000) {
                evlog_add('V', pct, mv / 10);
                last_log_us = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void)
{
    evlog_init();
    power_init();
#if ENABLE_NET
    /* Wi-Fi first: it needs ~70 KB of contiguous internal RAM, which only exists
     * before the display takes its DMA draw buffer. */
    net_init();
#endif
    ESP_LOGI(TAG, "before display: internal free %u (largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bsp_display_start();
    clock_setup();

    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();

    xTaskCreateWithCaps(console_task, "console", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(touch_poll_task, "touch_poll", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(imu_task, "imu", 4096, NULL, 4, &s_imu_task, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(battery_task, "battery", 3072, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(boot_button_task, "boot_btn", 3072, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Hello Luke clock running");
}
