#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "net.h"
#include "secrets.h"
#include "esp_heap_caps.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
#define CONNECTED_BIT BIT0
static int s_retry_delay_ms = 1000;
static bool s_wifi_enabled = false;   /* radio is stopped while the screen is dark */

/* Known networks, tried in order. After two failed attempts on one, move to the next. */
typedef struct { const char *ssid, *pass; } wifi_cred_t;
static const wifi_cred_t s_nets[] = {
    { WIFI_SSID, WIFI_PASS },
#ifdef WIFI_SSID2
    { WIFI_SSID2, WIFI_PASS2 },
#endif
#ifdef WIFI_SSID3
    { WIFI_SSID3, WIFI_PASS3 },
#endif
};
#define N_NETS (sizeof(s_nets) / sizeof(s_nets[0]))
static int s_net = 0, s_fails = 0;

static void apply_net(int i)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_nets[i].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_nets[i].pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, CONNECTED_BIT);
        if (!s_wifi_enabled) return;          /* we stopped it on purpose: no retries */
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d); retry in %d ms",
                 ((wifi_event_sta_disconnected_t *)data)->reason, s_retry_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(s_retry_delay_ms));
        if (s_retry_delay_ms < 30000) s_retry_delay_ms *= 2;
        if (++s_fails >= 2 && N_NETS > 1) {
            s_fails = 0;
            s_net = (s_net + 1) % N_NETS;
            apply_net(s_net);
            ESP_LOGI(TAG, "switching to network %d: %s", s_net, s_nets[s_net].ssid);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_delay_ms = 1000;
        s_fails = 0;
        xEventGroupSetBits(s_ev, CONNECTED_BIT);
    }
}

void net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 6;     /* internal RAM is shared with the display's DMA buffer */
    cfg.dynamic_rx_buf_num = 16;
    cfg.dynamic_tx_buf_num = 16;
    ESP_LOGI(TAG, "before wifi init: internal free %u (largest %u), dma largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s; running without Wi-Fi", esp_err_to_name(err));
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_net(s_net);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   /* idle: doze between beacons; net_ask() goes full power for the request */
    s_wifi_enabled = true;
    err = esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi start: %s, %u known networks, first %s; internal free now %u",
             esp_err_to_name(err), (unsigned)N_NETS, s_nets[0].ssid, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* Serial test hook: drop the current network and try the next known one. */
void net_next(void)
{
    if (N_NETS < 2) return;
    s_net = (s_net + 1) % N_NETS;
    s_fails = -1;            /* the forced disconnect below must not count as a failure */
    s_retry_delay_ms = 1000;
    apply_net(s_net);
    ESP_LOGI(TAG, "NEXTNET: switching to network %d: %s", s_net, s_nets[s_net].ssid);
    esp_wifi_disconnect();   /* the disconnect handler reconnects with the new config */
}

/* Serial test hook: blocking scan, print what the radio can see. */
void net_scan(void (*emit)(const char *))
{
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    uint16_t n = 12;
    wifi_ap_record_t recs[12];
    char line[96];
    if (err != ESP_OK || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        snprintf(line, sizeof(line), "SCAN failed: %s\n", esp_err_to_name(err));
        emit(line);
        return;
    }
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "SCAN %-32s ch%-2d %4d dBm auth %d\n", (char *)recs[i].ssid, recs[i].primary, recs[i].rssi, recs[i].authmode);
        emit(line);
    }
    snprintf(line, sizeof(line), "SCAN done, %u networks\n", (unsigned)n);
    emit(line);
}

/* Screen off -> radio off (the single biggest battery saver). Screen on -> start it again;
   reconnecting takes a few seconds, which hides inside the Claude button's wait. */
void net_set_active(bool on)
{
    if (on == s_wifi_enabled) return;
    s_wifi_enabled = on;
    if (on) {
        s_retry_delay_ms = 1000;
        ESP_LOGI(TAG, "Wi-Fi on: %s", esp_err_to_name(esp_wifi_start()));
    } else {
        ESP_LOGI(TAG, "Wi-Fi off: %s", esp_err_to_name(esp_wifi_stop()));
        xEventGroupClearBits(s_ev, CONNECTED_BIT);
    }
}

bool net_active(void) { return s_wifi_enabled; }

bool net_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & CONNECTED_BIT);
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void net_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    else snprintf(buf, len, "none");
}

/* ---- HTTP ---- */
typedef struct { char *buf; size_t len, cap; } body_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    body_t *b = (body_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + evt->data_len + 1 > b->cap) {
            size_t ncap = (b->len + evt->data_len + 1) * 2;
            char *nb = realloc(b->buf, ncap);
            if (!nb) return ESP_ERR_NO_MEM;
            b->buf = nb; b->cap = ncap;
        }
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t request_raw(const char *path, esp_http_client_method_t method, const void *data, size_t data_len,
                             const char *content_type, int timeout_ms, body_t *body, int *status);

static esp_err_t request(const char *path, esp_http_client_method_t method, const char *json_body,
                         int timeout_ms, body_t *body, int *status)
{
    return request_raw(path, method, json_body, json_body ? strlen(json_body) : 0, "application/json", timeout_ms, body, status);
}

static const char *s_context_header = NULL;   /* X-Watch-Context for the next request (calendar / tasks) */

static esp_err_t request_raw(const char *path, esp_http_client_method_t method, const void *data, size_t data_len,
                             const char *content_type, int timeout_ms, body_t *body, int *status)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", WATCH_SERVER_URL, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach, /* trust public CAs for the https tunnel */
        .event_handler = http_event,
        .user_data = body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(c, ESP_FAIL, TAG, "http init");
    esp_http_client_set_header(c, "X-Watch-Token", WATCH_TOKEN);
    if (s_context_header) esp_http_client_set_header(c, "X-Watch-Context", s_context_header);
    if (data) {
        esp_http_client_set_header(c, "Content-Type", content_type);
        esp_http_client_set_post_field(c, (const char *)data, (int)data_len);
    }
    esp_err_t err = esp_http_client_perform(c);
    if (status) *status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err;
}

bool net_health(void)
{
    if (!net_connected()) return false;
    body_t b = { 0 };
    int status = 0;
    esp_err_t err = request("/health", HTTP_METHOD_GET, NULL, 5000, &b, &status);
    free(b.buf);
    return err == ESP_OK && status == 200;
}

esp_err_t net_ask(const char *question, char **answer, int *http_status)
{
    *answer = NULL;
    for (int i = 0; i < 80 && !net_connected() && s_wifi_enabled; i++) vTaskDelay(pdMS_TO_TICKS(100));   /* radio may still be joining after wake */
    if (!net_connected()) {
        *answer = strdup("No Wi-Fi.");
        return ESP_ERR_INVALID_STATE;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "question", question);
    char *req_s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    body_t b = { 0 };
    int status = 0;
    esp_err_t err = request("/ask", HTTP_METHOD_POST, req_s, 95000, &b, &status);
    free(req_s);
    if (http_status) *http_status = status;

    if (err != ESP_OK) {
        free(b.buf);
        *answer = strdup("Can't reach the Pi.");
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return err;
    }
    cJSON *resp = b.buf ? cJSON_Parse(b.buf) : NULL;
    const cJSON *a = resp ? cJSON_GetObjectItem(resp, "answer") : NULL;
    const cJSON *e = resp ? cJSON_GetObjectItem(resp, "error") : NULL;
    if (cJSON_IsString(a))      *answer = strdup(a->valuestring);
    else if (cJSON_IsString(e)) *answer = strdup(e->valuestring);
    else                        *answer = strdup(status == 200 ? "Empty answer." : "Server error.");
    cJSON_Delete(resp);
    free(b.buf);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return status == 200 ? ESP_OK : ESP_FAIL;
}

/* Voice question: POST raw 16 kHz mono PCM to /ask-voice. Fills *heard (may be ""), *answer and
   *retry (server wants another take). Caller frees heard/answer. */
esp_err_t net_ask_voice(const int16_t *pcm, size_t samples, const char *context, voice_result_t *out, int *http_status)
{
    char **heard = &out->heard, **answer = &out->answer; bool *retry = &out->retry;
    memset(out, 0, sizeof(*out));
    s_context_header = context;
    for (int i = 0; i < 80 && !net_connected() && s_wifi_enabled; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!net_connected()) { *answer = strdup("No Wi-Fi."); return ESP_ERR_INVALID_STATE; }
    esp_wifi_set_ps(WIFI_PS_NONE);
    body_t b = { 0 };
    int status = 0;
    ESP_LOGI(TAG, "uploading %u bytes of audio", (unsigned)(samples * 2));
    esp_err_t err = request_raw("/ask-voice", HTTP_METHOD_POST, pcm, samples * sizeof(int16_t),
                                "application/octet-stream", 120000, &b, &status);
    if (http_status) *http_status = status;
    s_context_header = NULL;
    if (err != ESP_OK) {
        free(b.buf);
        *answer = strdup("Can't reach the Pi.");
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return err;
    }
    cJSON *resp = b.buf ? cJSON_Parse(b.buf) : NULL;
    const cJSON *task = resp ? cJSON_GetObjectItem(resp, "task") : NULL;
    const cJSON *ev = resp ? cJSON_GetObjectItem(resp, "event") : NULL;
    if (cJSON_IsString(task)) strlcpy(out->task, task->valuestring, sizeof(out->task));
    if (cJSON_IsObject(ev)) {
        const cJSON *d = cJSON_GetObjectItem(ev, "date"), *t = cJSON_GetObjectItem(ev, "time"), *ti = cJSON_GetObjectItem(ev, "title");
        if (cJSON_IsString(d))  strlcpy(out->ev_date, d->valuestring, sizeof(out->ev_date));
        if (cJSON_IsString(t))  strlcpy(out->ev_time, t->valuestring, sizeof(out->ev_time));
        if (cJSON_IsString(ti)) strlcpy(out->ev_title, ti->valuestring, sizeof(out->ev_title));
    }
    const cJSON *h = resp ? cJSON_GetObjectItem(resp, "heard") : NULL;
    const cJSON *a = resp ? cJSON_GetObjectItem(resp, "answer") : NULL;
    const cJSON *e = resp ? cJSON_GetObjectItem(resp, "error") : NULL;
    const cJSON *r = resp ? cJSON_GetObjectItem(resp, "retry") : NULL;
    *heard = strdup(cJSON_IsString(h) ? h->valuestring : "");
    if (cJSON_IsString(a))      *answer = strdup(a->valuestring);
    else if (cJSON_IsString(e)) *answer = strdup(e->valuestring);
    else                        *answer = strdup(status == 200 ? "Empty answer." : "Server error.");
    *retry = cJSON_IsTrue(r);
    cJSON_Delete(resp);
    free(b.buf);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return status == 200 ? ESP_OK : ESP_FAIL;
}
