/*
 * touch.h — KAST GT911 touch bring-up interface.
 * I2C addr 0x5D, no INT/RST lines (vendor touch.h: both -1).
 */
#pragma once

#include <stdbool.h>
#include "esp_lcd_touch.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize I2C master + GT911 driver. Returns touch handle. */
esp_err_t kast_touch_init(esp_lcd_touch_handle_t *out_touch);

/* Start a background task logging raw touch coordinates (bring-up aid). */
esp_err_t kast_touch_debug_start(esp_lcd_touch_handle_t touch);

/* Last few touch-downs, retained for the heartbeat replay.
 * Returns count (up to 6); arrays are internal, do not free. */
uint8_t kast_touch_last_taps(const uint16_t **x, const uint16_t **y);

/* Poll diagnostics: read attempts, touches seen, last read error. */
void kast_touch_stats(uint32_t *polls, uint32_t *hits, int *last_err);

/* Raw-diagnostics snapshot: last 0x814E status byte and the chip's
 * configured resolution (0 if the read failed). */
void kast_touch_raw_diag(uint8_t *status, uint16_t *xres, uint16_t *yres);

#ifdef __cplusplus
}
#endif
