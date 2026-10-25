#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Microphone capture: ES7210 ADC over I2S via the BSP, 16 kHz mono 16-bit into PSRAM. */
#define MIC_RATE        16000
#define MIC_MAX_SECS    15

esp_err_t mic_init(void);                       /* once at boot (I2S + codec); cheap if it fails */
esp_err_t mic_start(int max_secs);              /* begin capture (stops by itself at max_secs) */
void      mic_stop(void);                       /* stop capture; data stays until the next start */
bool      mic_recording(void);
int       mic_level(void);                      /* 0..100 loudness of the last 20 ms, for the UI */
const int16_t *mic_data(size_t *samples);       /* last recording */
float     mic_seconds(void);
