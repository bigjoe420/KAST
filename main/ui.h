/*
 * ui.h — KAST user interface on LVGL 9 (via esp_lvgl_port).
 *
 * Kick green on near-black (D-14). Motion budget is deliberately small:
 * an animated equalizer strip and a slow accent glow — captivating without
 * hammering the PSRAM framebuffer or the GT911 analog front-end.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start LVGL (own FreeRTOS task), adopt the RGB panel's framebuffers in
 * tear-free mode, attach touch, and build the Home screen.
 * Call after kast_display_init() and kast_touch_init().
 */
esp_err_t kast_ui_init(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
