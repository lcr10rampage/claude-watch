#include "calstore.h"
#include <string.h>
#include <stdio.h>
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "calstore";
static const esp_partition_t *s_part;

typedef struct {
    char     magic[4];      /* "CALW" */
    uint32_t len;
    char     start[12];     /* "YYYY-MM-DD" */
    uint32_t pad[3];
} cal_hdr_t;                /* 32 bytes */

esp_err_t calstore_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part) { ESP_LOGW(TAG, "no 'storage' partition"); return ESP_ERR_NOT_FOUND; }
    ESP_LOGI(TAG, "storage partition at 0x%" PRIx32 ", %" PRIu32 " bytes; %d calendar slots",
             s_part->address, s_part->size, CALSTORE_SLOTS);
    return ESP_OK;
}

static bool read_hdr(int slot, cal_hdr_t *h)
{
    if (!s_part || esp_partition_read(s_part, (size_t)slot * CALSTORE_SLOT_SIZE, h, sizeof(*h)) != ESP_OK) return false;
    return memcmp(h->magic, "CALW", 4) == 0 && h->len <= CALSTORE_MAX_TEXT && h->start[10] == '\0';
}

int calstore_find(const char *start)
{
    cal_hdr_t h;
    for (int i = 0; i < CALSTORE_SLOTS; i++)
        if (read_hdr(i, &h) && strncmp(h.start, start, 10) == 0) return i;
    return -1;
}

esp_err_t calstore_write(const char *start, const char *text, size_t len)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (len > CALSTORE_MAX_TEXT) return ESP_ERR_INVALID_SIZE;
    int slot = calstore_find(start);
    if (slot < 0) {                                   /* first empty slot, else the oldest week */
        cal_hdr_t h; char oldest[12] = "9999-99-99"; int oldest_i = 0;
        for (int i = 0; i < CALSTORE_SLOTS && slot < 0; i++) {
            if (!read_hdr(i, &h)) slot = i;
            else if (strncmp(h.start, oldest, 10) < 0) { memcpy(oldest, h.start, 11); oldest_i = i; }
        }
        if (slot < 0) slot = oldest_i;
    }
    size_t off = (size_t)slot * CALSTORE_SLOT_SIZE;
    cal_hdr_t h = { .magic = { 'C', 'A', 'L', 'W' }, .len = (uint32_t)len };
    strncpy(h.start, start, 10); h.start[10] = '\0';
    esp_err_t err = esp_partition_erase_range(s_part, off, CALSTORE_SLOT_SIZE);
    if (err == ESP_OK) err = esp_partition_write(s_part, off, &h, sizeof(h));
    if (err == ESP_OK && len) err = esp_partition_write(s_part, off + sizeof(h), text, (len + 3) & ~3u);
    ESP_LOGI(TAG, "week %s -> slot %d (%u bytes): %s", h.start, slot, (unsigned)len, esp_err_to_name(err));
    return err;
}

size_t calstore_read(const char *start, char *buf, size_t cap)
{
    int slot = calstore_find(start);
    if (slot < 0 || cap == 0) return 0;
    cal_hdr_t h;
    if (!read_hdr(slot, &h)) return 0;
    size_t n = h.len < cap - 1 ? h.len : cap - 1;
    if (esp_partition_read(s_part, (size_t)slot * CALSTORE_SLOT_SIZE + sizeof(h), buf, n) != ESP_OK) return 0;
    buf[n] = '\0';
    return n;
}

void calstore_list(void (*emit)(const char *))
{
    cal_hdr_t h; char line[48]; int used = 0;
    for (int i = 0; i < CALSTORE_SLOTS; i++) {
        if (!read_hdr(i, &h)) continue;
        snprintf(line, sizeof(line), "CAL slot %d week %s %u bytes\n", i, h.start, (unsigned)h.len);
        emit(line); used++;
    }
    snprintf(line, sizeof(line), "CAL %d of %d slots used\n", used, CALSTORE_SLOTS);
    emit(line);
}

esp_err_t calstore_clear(void)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    return esp_partition_erase_range(s_part, 0, (size_t)CALSTORE_SLOTS * CALSTORE_SLOT_SIZE);
}
