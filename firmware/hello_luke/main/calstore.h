#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Calendar week storage: raw 16 KB slots at the start of the "storage" partition.
   Each slot holds the text lines for one week, keyed by its start date "YYYY-MM-DD". */
#define CALSTORE_SLOT_SIZE 16384
#define CALSTORE_SLOTS     12
#define CALSTORE_MAX_TEXT  (CALSTORE_SLOT_SIZE - 32)

esp_err_t calstore_init(void);
int       calstore_find(const char *start);                         /* slot index or -1 */
esp_err_t calstore_write(const char *start, const char *text, size_t len);
size_t    calstore_read(const char *start, char *buf, size_t cap);  /* bytes copied (NUL-terminated), 0 if absent */
void      calstore_list(void (*emit)(const char *));
esp_err_t calstore_clear(void);
