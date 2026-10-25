#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Wi-Fi station + Claude Watch server client. */
void      net_init(void);                      /* starts Wi-Fi; reconnects forever in the background */
bool      net_connected(void);
int       net_rssi(void);                     /* dBm, 0 if not associated */
void      net_ip(char *buf, size_t len);
/* POST /ask. Blocks up to ~95 s. On success *answer is a malloc'd string (free it). */
void net_next(void);
void net_set_active(bool on);
bool net_active(void);
void net_scan(void (*emit)(const char *));
#include <stdint.h>
typedef struct {
    char *heard, *answer;          /* malloc'd; caller frees */
    bool  retry;
    char  task[96];                /* tasks mode: the cleaned-up task title ("" if none) */
    char  ev_date[12], ev_time[16], ev_title[96];   /* calendar mode: what was added ("" if none) */
} voice_result_t;
/* context: NULL (plain question), "calendar" (add an event) or "tasks" (add a task) */
esp_err_t net_ask_voice(const int16_t *pcm, size_t samples, const char *context, voice_result_t *out, int *http_status);
esp_err_t net_ask(const char *question, char **answer, int *http_status);
/* GET /health; true if the server answered. */
bool      net_health(void);
