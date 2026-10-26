/*
 * ui.c — KAST chrome, screen router, and LVGL port bring-up.
 *
 * The chrome is persistent: status bar (KAST brand + WIFI/RELAY/OBS link
 * words) and a Home button that follows every module page. Below it sits
 * the content container the router fills — active screen loads its tree,
 * inactive screens unload (timers stopped, nothing allocated in the
 * switch path beyond LVGL widgets, K-4).
 *
 * Status-bar link words are a spangroup: each link renders dim when
 * down, Kick green when up, with an optional override word ("RETRY",
 * "FAIL"). Non-LVGL tasks update links through kast_ui_link_status(),
 * which takes the port lock internally.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "ui.h"

static const char *TAG = "ui";

static lv_display_t *s_disp;
static lv_obj_t *s_content;        /* router fills this */
static lv_obj_t *s_home_btn;
static lv_obj_t *s_links;          /* spangroup: WIFI · RELAY · OBS */
static char s_link_text[KAST_LINK_COUNT][16];
static bool s_link_ok[KAST_LINK_COUNT];
static const char *const s_link_names[KAST_LINK_COUNT] = {"WIFI", "RELAY", "OBS"};

/* Fixed span layout, built once: WIFI " · " RELAY " · " OBS.
 * render_links() rewrites text/color in place and refreshes.
 * Caller must hold the LVGL lock (router runs in LVGL task context). */
static lv_span_t *s_span_link[KAST_LINK_COUNT];
static lv_span_t *s_span_sep[KAST_LINK_COUNT - 1];

static void render_links(void)
{
    for (int i = 0; i < KAST_LINK_COUNT; i++) {
        lv_span_set_text_fmt(s_span_link[i], "%s %s", s_link_names[i],
                             s_link_ok[i] ? "OK"
                                          : (s_link_text[i][0] ? s_link_text[i] : "--"));
        lv_style_set_text_color(lv_span_get_style(s_span_link[i]),
                                s_link_ok[i] ? KAST_COLOR_GREEN : KAST_COLOR_TEXT_DIM);
    }
    lv_spangroup_refresh(s_links);
}

void kast_ui_link_status(kast_link_t link, bool ok, const char *text)
{
    if (link < 0 || link >= KAST_LINK_COUNT || !s_links) {
        return;
    }
    if (!lvgl_port_lock(-1)) {
        return;
    }
    s_link_ok[link] = ok;
    if (text) {
        snprintf(s_link_text[link], sizeof(s_link_text[link]), "%s", text);
    } else {
        s_link_text[link][0] = '\0';
    }
    render_links();
    lvgl_port_unlock();
}

lv_obj_t *kast_ui_content(void)
{
    return s_content;
}

/* ------------------------------------------------------------------ */
/* Router                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    void (*load)(lv_obj_t *parent);
    void (*unload)(void);
} kast_screen_def_t;

static const kast_screen_def_t s_screens[KAST_SCREEN_COUNT] = {
    [KAST_SCREEN_HOME]     = {kast_home_load, kast_home_unload},
    [KAST_SCREEN_CHAT]     = {kast_chat_load, kast_chat_unload},
    [KAST_SCREEN_SCENES]   = {kast_scenes_load, kast_scenes_unload},
    [KAST_SCREEN_SFX]      = {kast_sfx_load, kast_sfx_unload},
    [KAST_SCREEN_SETTINGS] = {kast_settings_load, kast_settings_unload},
};

static int s_current = -1;         /* no screen loaded yet */

static void home_btn_cb(lv_event_t *e)
{
    /* The button sits inside the status bar, which has its own CLICKED
     * handler (jump to Settings). Without this the event bubbles up and
     * the bar handler runs after ours, overriding HOME with Settings. */
    lv_event_stop_bubbling(e);
    kast_ui_show(KAST_SCREEN_HOME);
}

static void bar_cb(lv_event_t *e)
{
    (void)e;
    kast_ui_show(KAST_SCREEN_SETTINGS);
}

void kast_ui_show(kast_screen_t screen)
{
    if (screen < 0 || screen >= KAST_SCREEN_COUNT || screen == s_current) {
        return;
    }
    if (s_current >= 0) {
        s_screens[s_current].unload();
    }
    lv_obj_clean(s_content);
    s_screens[screen].load(s_content);
    s_current = screen;
}

/* ------------------------------------------------------------------ */
/* Chrome                                                              */
/* ------------------------------------------------------------------ */

static void build_chrome(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, KAST_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, KAST_COLOR_TEXT, 0);
    lv_obj_set_scrollable(scr, false);

    /* Status bar */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, KAST_H_RES, 44);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_scrollable(bar, false);
    /* Tap the status bar to reach the connection settings page */
    lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *brand = lv_label_create(bar);
    lv_label_set_text(brand, "KAST");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(brand, KAST_COLOR_GREEN, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 16, 0);

    s_links = lv_spangroup_create(bar);
    lv_obj_set_style_text_font(s_links, &lv_font_montserrat_12, 0);
    /* Right-align inside the fixed box: text must end at the right edge
     * no matter how wide the words render at the current font size. */
    lv_obj_set_style_text_align(s_links, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_links, 360, 30);
    lv_obj_align(s_links, LV_ALIGN_RIGHT_MID, -16, 0);
    for (int i = 0; i < KAST_LINK_COUNT; i++) {
        if (i > 0) {
            s_span_sep[i - 1] = lv_spangroup_add_span(s_links);
            lv_span_set_text(s_span_sep[i - 1], "  \xC2\xB7  ");
            lv_style_set_text_color(lv_span_get_style(s_span_sep[i - 1]), KAST_COLOR_TEXT_DIM);
        }
        s_span_link[i] = lv_spangroup_add_span(s_links);
    }
    render_links();

    /* Home button lives in the status bar — always reachable and never
     * overlapping page content (the old floating bottom-right slot sat
     * on top of the settings keyboard) */
    s_home_btn = lv_button_create(bar);
    lv_obj_set_size(s_home_btn, 40, 32);
    lv_obj_align(s_home_btn, LV_ALIGN_LEFT_MID, 78, 0);
    lv_obj_set_style_radius(s_home_btn, 8, 0);
    lv_obj_set_style_bg_color(s_home_btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(s_home_btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(s_home_btn, 2, 0);
    lv_obj_set_style_bg_color(s_home_btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_home_btn, home_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_label = lv_label_create(s_home_btn);
    lv_label_set_text(home_label, LV_SYMBOL_HOME);
    lv_obj_set_style_text_font(home_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(home_label, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(home_label, KAST_COLOR_BG, LV_STATE_PRESSED);
    lv_obj_center(home_label);

    /* Content container — the router's canvas */
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, KAST_H_RES, KAST_V_RES - 44);
    lv_obj_align(s_content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_scrollable(s_content, false);
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

    lvgl_port_lock(-1);
    build_chrome(lv_screen_active());
    kast_ui_show(KAST_SCREEN_HOME);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI up: chrome + router, Home loaded (LVGL %d.%d.%d)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}
