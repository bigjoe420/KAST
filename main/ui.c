/*
 * ui.c — KAST Home screen (LVGL 9, esp_lvgl_port).
 *
 * Layout (480x480):
 *   [status bar]  KAST .................... WIFI -- · OBS --
 *   [title]      KAST (48px, Kick green) over a slow-breathing accent bar
 *   [modules]    CHAT / SCENES / SFX — large rounded buttons, green keyline,
 *                green fill + dark text while pressed
 *   [eq strip]   seven animated bars — the "touch of 2D" (tiny invalidation
 *                area, ~20 Hz timer, sine-blended heights)
 *   [footer]     status line + live touch coordinates (axis-mapping proof)
 *
 * Theme (D-14): Kick green on near-black. All motion is small-area; nothing
 * here redraws the full frame outside LVGL's own dirty tracking.
 */
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include "ui.h"

static const char *TAG = "ui";

/* Kick green theme */
#define KAST_COLOR_BG       lv_color_hex(0x050805)
#define KAST_COLOR_CARD     lv_color_hex(0x0D1A0D)
#define KAST_COLOR_GREEN    lv_color_hex(0x53FC18)  /* Kick brand green */
#define KAST_COLOR_GREEN_DIM lv_color_hex(0x1E5A12)
#define KAST_COLOR_TEXT     lv_color_hex(0xE8F5E8)
#define KAST_COLOR_TEXT_DIM lv_color_hex(0x6E8A6E)

#define KAST_H_RES  480
#define KAST_V_RES  480
#define KAST_EQ_BARS 7

static lv_display_t *s_disp;
static lv_obj_t *s_status_label;   /* footer status line */
static lv_obj_t *s_eq_bars[KAST_EQ_BARS];
/* Thin adapter: lv_anim's exec_cb is (void *, int32_t); lv_obj_set_style_opa
 * takes a selector too — bridge them so the compiler doesn't cast. */
static void glow_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static lv_obj_t *s_glow_bar;       /* breathing accent under the title */

/* ------------------------------------------------------------------ */
/* Module buttons                                                      */
/* ------------------------------------------------------------------ */

static void module_btn_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s — module not wired yet", name);
    lv_label_set_text(s_status_label, buf);
}

static lv_obj_t *module_button(lv_obj_t *parent, const char *name, lv_flex_align_t align)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 290, 68);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    /* pressed state: green fill, dark text, glow */
    lv_obj_set_style_bg_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 18, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, module_btn_cb, LV_EVENT_CLICKED, (void *)name);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(label, KAST_COLOR_BG, LV_STATE_PRESSED);
    lv_obj_center(label);
    (void)align;
    return btn;
}

/* ------------------------------------------------------------------ */
/* Equalizer strip — lightweight motion                                */
/* ------------------------------------------------------------------ */

static void eq_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t t = lv_tick_get();
    for (int i = 0; i < KAST_EQ_BARS; i++) {
        /* two blended sines per bar: organic, deterministic, cheap */
        float ph = (float)t / 1000.0f;
        float v = sinf(ph * 2.1f + i * 0.9f) * 0.5f +
                  sinf(ph * 3.7f + i * 1.7f) * 0.5f;
        float norm = (v + 2.0f) / 4.0f;          /* 0..1 */
        int h = 10 + (int)(norm * 30.0f);        /* 10..40 px, fits the strip */
        lv_obj_set_height(s_eq_bars[i], h);
        lv_obj_align_to(s_eq_bars[i], lv_obj_get_parent(s_eq_bars[i]),
                        LV_ALIGN_BOTTOM_MID, (i - (KAST_EQ_BARS - 1) / 2) * 22, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Home screen                                                         */
/* ------------------------------------------------------------------ */

static void build_home_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, KAST_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, KAST_COLOR_TEXT, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, KAST_H_RES, 44);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(bar);
    lv_label_set_text(brand, "KAST");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(brand, KAST_COLOR_GREEN, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *net = lv_label_create(bar);
    lv_label_set_text(net, "WIFI --  \xC2\xB7  OBS --");
    lv_obj_set_style_text_color(net, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(net, LV_ALIGN_RIGHT_MID, -16, 0);

    /* Title block */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "KICK AUXILIARY");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, KAST_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "STREAMING TERMINAL");
    lv_obj_set_style_text_color(subtitle, KAST_COLOR_TEXT, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 98);

    /* Breathing accent bar (glow pulse) */
    s_glow_bar = lv_obj_create(scr);
    lv_obj_set_size(s_glow_bar, 220, 4);
    lv_obj_align(s_glow_bar, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_radius(s_glow_bar, 2, 0);
    lv_obj_set_style_bg_color(s_glow_bar, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(s_glow_bar, 0, 0);
    lv_obj_set_style_shadow_color(s_glow_bar, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_width(s_glow_bar, 14, 0);

    lv_anim_t glow;
    lv_anim_init(&glow);
    lv_anim_set_var(&glow, s_glow_bar);
    lv_anim_set_values(&glow, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_time(&glow, 1800);
    lv_anim_set_playback_time(&glow, 1800);
    lv_anim_set_repeat_count(&glow, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&glow, glow_opa_cb);
    lv_anim_start(&glow);

    /* Module buttons — column ends at 384, clear of the EQ strip (396+) */
    lv_obj_t *menu = lv_obj_create(scr);
    lv_obj_set_size(menu, 320, 3 * 68 + 2 * 14);
    lv_obj_align(menu, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(menu, 14, 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

    module_button(menu, "CHAT", LV_FLEX_ALIGN_CENTER);
    module_button(menu, "SCENES", LV_FLEX_ALIGN_CENTER);
    module_button(menu, "SFX", LV_FLEX_ALIGN_CENTER);

    /* Equalizer strip — y 396..444, below the buttons, above the footer */
    lv_obj_t *eq = lv_obj_create(scr);
    lv_obj_set_size(eq, KAST_EQ_BARS * 22, 48);
    lv_obj_align(eq, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_style_bg_opa(eq, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq, 0, 0);
    lv_obj_clear_flag(eq, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < KAST_EQ_BARS; i++) {
        s_eq_bars[i] = lv_obj_create(eq);
        lv_obj_set_size(s_eq_bars[i], 12, 20);
        lv_obj_set_style_radius(s_eq_bars[i], 3, 0);
        lv_obj_set_style_bg_color(s_eq_bars[i],
                                  (i % 2) ? KAST_COLOR_GREEN_DIM : KAST_COLOR_GREEN, 0);
        lv_obj_set_style_border_width(s_eq_bars[i], 0, 0);
    }
    lv_timer_create(eq_timer_cb, 50, NULL);

    /* Footer: status line */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "READY");
    lv_obj_set_style_text_color(s_status_label, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 16, -16);
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

esp_err_t kast_ui_init(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    /* Bounce-buffer mode: LVGL draws into its own partial buffers, flush
     * copies to the PSRAM fb, and the RGB DMA streams from internal-RAM
     * bounce buffers — no PSRAM pixel-clock reads, no flicker. Tear-free
     * double-fb mode can't combine with bb_mode, so it's off. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = KAST_H_RES * 60,
        .double_buffer = true,
        .hres = KAST_H_RES,
        .vres = KAST_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .full_refresh = false,
            .direct_mode = false,
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = false,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "display attach failed");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg) != NULL,
                        ESP_FAIL, TAG, "touch attach failed");

    lvgl_port_lock(0);
    build_home_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI up: Home screen (Kick green theme, LVGL %d.%d.%d)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}
