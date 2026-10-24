/*
 * display.h — KAST display bring-up interface.
 * ST7701 RGB panel over ESP32-S3 LCD peripheral, 3-wire SPI command channel,
 * vendor timing table (see pins.h / PINOUT.md §1.4 — the Law).
 */
#pragma once

#include "esp_lcd_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the RGB panel + 3-wire SPI command channel. Panel starts OFF. */
esp_err_t kast_display_init(esp_lcd_panel_handle_t *out_panel);

/* Backlight (GPIO38, LEDC PWM). Call kast_backlight_init() before set(). */
esp_err_t kast_backlight_init(void);
void kast_backlight_set(uint8_t percent);

#ifdef __cplusplus
}
#endif
