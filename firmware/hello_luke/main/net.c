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
#include "cJSON.h"
#include "net.h"
#include "secrets.h"
#include "esp_heap_caps.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
#define CONNECTED_BIT BIT0
static int s_retry_delay_ms = 1000;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d); retry in %d ms",
                 ((wifi_event_sta_disconnected_t *)data)->reason, s_retry_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(s_retry_delay_ms));
        if (s_retry_delay_ms < 30000) s_retry_delay_ms *= 2;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_delay_ms = 1000;
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

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);        /* power save made the link too laggy for TCP; revisit for battery */
    err = esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi start: %s, ssid %s; internal free now %u",
             esp_err_to_name(err), WIFI_SSID, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

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

static esp_err_t request(const char *path, esp_http_client_method_t method, const char *json_body,
                         int timeout_ms, body_t *body, int *status)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", WATCH_SERVER_URL, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = http_event,
        .user_data = body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(c, ESP_FAIL, TAG, "http init");
    esp_http_client_set_header(c, "X-Watch-Token", WATCH_TOKEN);
    if (json_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, json_body, strlen(json_body));
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
    if (!net_connected()) {
        *answer = strdup("No Wi-Fi.");
        return ESP_ERR_INVALID_STATE;
    }
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
    return status == 200 ? ESP_OK : ESP_FAIL;
}
