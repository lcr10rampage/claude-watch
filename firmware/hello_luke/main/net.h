#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Wi-Fi station + Claude Watch server client. */
void      net_init(void);                      /* starts Wi-Fi; reconnects forever in the background */
bool      net_connected(void);
int       net_rssi(void);                     /* dBm, 0 if not associated */
void      net_ip(char *buf, size_t len);
/* POST /ask. Blocks up to ~95 s. On success *answer is a malloc'd string (free it). */
esp_err_t net_ask(const char *question, char **answer, int *http_status);
/* GET /health; true if the server answered. */
bool      net_health(void);
