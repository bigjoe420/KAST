/*
 * ui.h — KAST user interface on LVGL 9 (via esp_lvgl_port).
 *
 * Kick green on near-black. Motion budget is deliberately small:
 * an animated equalizer strip and a slow accent glow — captivating without
 * hammering the PSRAM framebuffer or the GT911 analog front-end.
 *
 * Structure: a persistent chrome (status bar + Home button) owned here,
 * and a content container the screen router fills. Each screen module
 * provides load/unload; unload stops every timer/anim the screen started
 * (K-4: no allocation churn in the switch path, static buffers only).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kick green theme — shared by every screen module */
#define KAST_COLOR_BG        lv_color_hex(0x050805)
#define KAST_COLOR_CARD      lv_color_hex(0x0D1A0D)
#define KAST_COLOR_GREEN     lv_color_hex(0x53FC18)  /* Kick brand green */
#define KAST_COLOR_GREEN_DIM lv_color_hex(0x1E5A12)
#define KAST_COLOR_TEXT      lv_color_hex(0xE8F5E8)
#define KAST_COLOR_TEXT_DIM  lv_color_hex(0x6E8A6E)

#define KAST_H_RES 480
#define KAST_V_RES 480

typedef enum {
    KAST_SCREEN_HOME = 0,
    KAST_SCREEN_CHAT,
    KAST_SCREEN_SCENES,
    KAST_SCREEN_SFX,
    KAST_SCREEN_GFX,
    KAST_SCREEN_SETTINGS,
    KAST_SCREEN_COUNT
} kast_screen_t;

typedef enum {
    KAST_LINK_WIFI = 0,
    KAST_LINK_RELAY,
    KAST_LINK_OBS,
    KAST_LINK_COUNT
} kast_link_t;

/*
 * Start LVGL (own FreeRTOS task), adopt the RGB panel's framebuffers in
 * tear-free mode, attach touch, build the chrome, and load the Home
 * screen. Call after kast_display_init() and kast_touch_init().
 */
esp_err_t kast_ui_init(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch);

/*
 * Switch the content container to the given screen. LVGL task context
 * only (button events, timers) — the lock is already held there.
 */
void kast_ui_show(kast_screen_t screen);

/*
 * Update one status-bar link word (WIFI / RELAY / OBS). Safe from any
 * task — the LVGL lock is taken internally; never call from LVGL task
 * context (event handlers). ok paints the word Kick green; text
 * overrides the word (e.g. "RETRY"); NULL restores "--".
 */
void kast_ui_link_status(kast_link_t link, bool ok, const char *text);

/* Content container the active screen builds into (screen modules only). */
lv_obj_t *kast_ui_content(void);

/* ------------------------------------------------------------------ */
/* Screen modules — registered by the router in ui.c                   */
/* ------------------------------------------------------------------ */
void kast_home_load(lv_obj_t *parent);
void kast_home_unload(void);
void kast_chat_load(lv_obj_t *parent);
void kast_chat_unload(void);
void kast_scenes_load(lv_obj_t *parent);
void kast_scenes_unload(void);
void kast_sfx_load(lv_obj_t *parent);
void kast_sfx_unload(void);
void kast_gfx_load(lv_obj_t *parent);
void kast_gfx_unload(void);
void kast_settings_load(lv_obj_t *parent);
void kast_settings_unload(void);

#ifdef __cplusplus
}
#endif
