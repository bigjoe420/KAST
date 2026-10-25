/*
 * ui.h — KAST user interface on LVGL 9 (via esp_lvgl_port).
 *
 * Kick green on near-black. Motion budget is deliberately small:
 * an animated equalizer strip and a slow accent glow — captivating without
 * hammering the PSRAM framebuffer or the GT911 analog front-end.
 */
#pragma once

#include <stdbool.h>
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

/*
 * Update the status-bar Wi-Fi word. Safe to call from any task — the
 * LVGL lock is taken internally. connected=true shows "WIFI OK" in Kick
 * green; otherwise `text` (e.g. "WIFI ...", "WIFI RETRY", "WIFI FAIL")
 * replaces the neutral "WIFI --" placeholder. May be called before the
 * UI exists; such updates are dropped.
 */
void kast_ui_net_status(bool connected, const char *text);

#ifdef __cplusplus
}
#endif
