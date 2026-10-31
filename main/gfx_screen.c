/*
 * gfx_screen.c — KAST motion-graphics page (Phase 4, overlay mode).
 *
 * A grid of tiles, one per graphic in the relay's graphics library. The
 * catalog arrives as {"type":"gfx.catalog","items":"id:Name,..."} pushed
 * on connect (relay.c owns the store); this screen polls the store's
 * version and rebuilds the grid only when it changes (K-4).
 *
 * A tap sends {"type":"gfx.play","id"}; the relay overlays the clip on the
 * live OBS scene — revealing the KAST GFX item, restarting the media, and
 * hiding it again after the graphic's configured duration — and fires the
 * bound sound through the SFX path when one is set. The program scene
 * never changes (owner-approved overlay mode, 2026-10-31).
 */
#include <stdio.h>

#include "lvgl.h"

#include "relay.h"
#include "ui.h"

#define GFX_TILE_W    146
#define GFX_TILE_H    86
#define GFX_PAD       6
#define GFX_POLL_MS   500

static lv_obj_t *s_grid;
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static uint32_t  s_built_version;
static uint32_t  s_tile_count;

/* snapshot the id at build time — the relay store is mutex-guarded and
   can change under us */
static char s_tile_id[KAST_GFX_MAX][KAST_SFX_ID_LEN];

static void gfx_tile_cb(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id != NULL && id[0] != '\0') {
        kast_relay_gfx_play(id);
    }
}

static lv_obj_t *gfx_tile_add(lv_obj_t *parent, const char *name, const char *id,
                              uint32_t idx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, GFX_TILE_W, GFX_TILE_H);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 16, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, gfx_tile_cb, LV_EVENT_CLICKED,
                        (void *)s_tile_id[idx]);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, GFX_TILE_W - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
    return btn;
}

static void gfx_grid_rebuild(void)
{
    lv_obj_clean(s_grid);
    s_tile_count = 0;

    const uint32_t count = kast_relay_gfx_count();
    if (count == 0) {
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    for (uint32_t i = 0; i < count && i < KAST_GFX_MAX; i++) {
        char id[KAST_SFX_ID_LEN];
        char name[KAST_SFX_NAME_LEN];
        if (!kast_relay_gfx_get(i, id, sizeof(id), name, sizeof(name))) {
            continue;
        }
        snprintf(s_tile_id[i], sizeof(s_tile_id[i]), "%s", id);
        gfx_tile_add(s_grid, name[0] != '\0' ? name : id, s_tile_id[i], i);
        s_tile_count++;
    }
}

static void gfx_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (kast_relay_gfx_version() != s_built_version) {
        s_built_version = kast_relay_gfx_version();
        gfx_grid_rebuild();
    }
}

void kast_gfx_load(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, KAST_COLOR_BG, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "GFX");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, KAST_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *accent = lv_obj_create(parent);
    lv_obj_set_size(accent, 160, 4);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_radius(accent, 2, 0);
    lv_obj_set_style_bg_color(accent, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_shadow_color(accent, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_width(accent, 12, 0);

    s_hint = lv_label_create(parent);
    lv_label_set_text(s_hint,
                      "No graphics on the relay yet.\n"
                      "Add clips or images in the Companion\n"
                      "app's Graphics Library window.");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 20);

    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, KAST_H_RES, LV_SIZE_CONTENT);
    lv_obj_align(s_grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, GFX_PAD, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, GFX_PAD, 0);
    lv_obj_set_style_pad_column(s_grid, GFX_PAD, 0);
    lv_obj_set_scrollable(s_grid, false);

    s_built_version = kast_relay_gfx_version();
    gfx_grid_rebuild();
    s_poll = lv_timer_create(gfx_poll_cb, GFX_POLL_MS, NULL);
}

void kast_gfx_unload(void)
{
    lv_timer_delete(s_poll);
    s_poll = NULL;
    s_grid = NULL;
    s_hint = NULL;
    s_tile_count = 0;
    for (int i = 0; i < KAST_GFX_MAX; i++) {
        s_tile_id[i][0] = '\0';
    }
}
