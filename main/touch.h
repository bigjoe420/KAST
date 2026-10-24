/*
 * touch.h — KAST GT911 touch interface.
 * I2C addr 0x5D on GPIO19/45, no INT/RST lines (vendor touch.h: both -1).
 * Coordinates are LVGL-space with NO mirroring — empirical calibration
 * 2026-10-24 (owner corner taps). Do not add mirror flags without
 * re-calibrating on glass.
 */
#pragma once

#include "esp_lcd_touch.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize I2C master + GT911 driver. Returns touch handle. */
esp_err_t kast_touch_init(esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif
