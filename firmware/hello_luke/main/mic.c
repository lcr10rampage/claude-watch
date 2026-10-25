#include "mic.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"

static const char *TAG = "mic";
static esp_codec_dev_handle_t s_dev;
static int16_t *s_buf;                  /* PSRAM, MIC_RATE * MIC_MAX_SECS samples */
static volatile size_t s_len;           /* samples captured */
static volatile size_t s_limit;
static volatile bool s_run, s_active;
static volatile int s_level;
#define CHUNK 320                       /* 20 ms at 16 kHz */

esp_err_t mic_init(void)
{
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) { ESP_LOGW(TAG, "bsp_audio_init: %s", esp_err_to_name(err)); return err; }
    s_dev = bsp_audio_codec_microphone_init();
    if (!s_dev) { ESP_LOGW(TAG, "microphone codec init failed"); return ESP_FAIL; }
    s_buf = heap_caps_malloc(MIC_RATE * MIC_MAX_SECS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready: %d Hz mono, up to %d s", MIC_RATE, MIC_MAX_SECS);
    return ESP_OK;
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t stereo[CHUNK * 2];          /* the ES7210 delivers both mics; keep one in mono */
    int16_t chunk[CHUNK];
    int skip = 6;                              /* drop the first ~120 ms: the ADC's start-up thump */
    while (s_run && s_len < s_limit) {
        int ret = esp_codec_dev_read(s_dev, stereo, sizeof(stereo));
        if (ret != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "read %d", ret); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (skip > 0) { skip--; continue; }
        for (size_t i = 0; i < CHUNK; i++) chunk[i] = (int16_t)(((int)stereo[2 * i] + (int)stereo[2 * i + 1]) / 2);
        size_t n = CHUNK;
        if (s_len + n > s_limit) n = s_limit - s_len;
        memcpy(s_buf + s_len, chunk, n * sizeof(int16_t));
        s_len += n;
        double acc = 0;
        for (size_t i = 0; i < n; i++) acc += (double)chunk[i] * chunk[i];
        double rms = sqrt(acc / (n ? n : 1));                      /* 0..32767 */
        double db = 20.0 * log10(rms / 32768.0 + 1e-9);           /* -inf..0 dBFS */
        int lvl = (int)((db + 60.0) * (100.0 / 60.0));             /* -60 dBFS -> 0, 0 dBFS -> 100 */
        s_level = lvl < 0 ? 0 : lvl > 100 ? 100 : lvl;
    }
    esp_codec_dev_close(s_dev);
    s_level = 0;
    s_active = false;
    ESP_LOGI(TAG, "stopped: %.1f s", (double)s_len / MIC_RATE);
    vTaskDelete(NULL);
}

esp_err_t mic_start(int max_secs)
{
    if (!s_dev || !s_buf) return ESP_ERR_INVALID_STATE;
    if (s_active) return ESP_OK;
    if (max_secs <= 0 || max_secs > MIC_MAX_SECS) max_secs = MIC_MAX_SECS;
    esp_codec_dev_sample_info_t fs = { .sample_rate = MIC_RATE, .channel = 2, .bits_per_sample = 16 };   /* like Waveshare's example */
    int ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "open %d", ret); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_dev, 30.0f);               /* ES7210 range 0..37.5 dB */
    s_len = 0; s_limit = (size_t)max_secs * MIC_RATE; s_level = 0;
    s_run = true; s_active = true;
    if (xTaskCreatePinnedToCoreWithCaps(mic_task, "mic", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_run = false; s_active = false; esp_codec_dev_close(s_dev); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "recording (max %d s)", max_secs);
    return ESP_OK;
}

void mic_stop(void) { s_run = false; }
bool mic_recording(void) { return s_active; }
int mic_level(void) { return s_level; }
const int16_t *mic_data(size_t *samples) { if (samples) *samples = s_len; return s_buf; }
float mic_seconds(void) { return (float)s_len / MIC_RATE; }
