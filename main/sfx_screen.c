/*
 * sfx_screen.c — KAST soundboard page.
 *
 * A grid of tiles, one per sound file the relay found in relay/sounds/.
 * The catalog arrives as {"type":"sfx.catalog","sounds":"id:Name,..."}
 * pushed on connect (relay.c owns the store); this screen polls the
 * store's version on a timer and rebuilds the grid only when it changes
 * (K-4: no allocation churn — tiles are rebuilt on change, never per tap).
 *
 * A tap sends {"type":"sfx.play","id"}; the relay restarts the matching
 * OBS media source. OBS supplies stream output and local monitoring. The
 * device itself stays silent — its I2S amp is for separate local alerts.
 */
#include <stdio.h>

#include "lvgl.h"

#include "relay.h"
#include "ui.h"

#define SFX_COLS      3
#define SFX_TILE_W    146
#define SFX_TILE_H    86
#define SFX_PAD       6
#define SFX_POLL_MS   500

static lv_obj_t *s_grid;
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static uint32_t  s_built_version;   /* catalog version the grid reflects */
static uint32_t  s_tile_count;

/* event user data carries the tile index into this fixed array — the
   relay store is mutex-guarded and can change under us, so snapshot the
   id string when the tile is built */
static char s_tile_id[KAST_SFX_MAX][KAST_SFX_ID_LEN];

static void sfx_tile_cb(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id != NULL && id[0] != '\0') {
        /* SFX belongs on the stream PC, not the optional desk speaker. */
        kast_relay_sfx_play(id);
    }
}

static lv_obj_t *sfx_tile_add(lv_obj_t *parent, const char *name, const char *id,
                              uint32_t idx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, SFX_TILE_W, SFX_TILE_H);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 16, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, sfx_tile_cb, LV_EVENT_CLICKED,
                        (void *)s_tile_id[idx]);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, SFX_TILE_W - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
    return btn;
}

/* rebuild the grid from the current relay catalog; the empty state
   explains where sounds come from, since the sounds dir is personal
   content the repo never ships */
static void sfx_grid_rebuild(void)
{
    lv_obj_clean(s_grid);
    s_tile_count = 0;

    const uint32_t count = kast_relay_sfx_count();
    if (count == 0) {
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    for (uint32_t i = 0; i < count && i < KAST_SFX_MAX; i++) {
        char id[KAST_SFX_ID_LEN];
        char name[KAST_SFX_NAME_LEN];
        if (!kast_relay_sfx_get(i, id, sizeof(id), name, sizeof(name))) {
            continue;
        }
        snprintf(s_tile_id[i], sizeof(s_tile_id[i]), "%s", id);
        sfx_tile_add(s_grid, name[0] != '\0' ? name : id, s_tile_id[i], i);
        s_tile_count++;
    }
}

static void sfx_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (kast_relay_sfx_version() != s_built_version) {
        s_built_version = kast_relay_sfx_version();
        sfx_grid_rebuild();
    }
}

void kast_sfx_load(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, KAST_COLOR_BG, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SFX");
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
                      "No sounds on the relay yet.\n"
                      "Drop mp3/wav/ogg/m4a files into\n"
                      "relay\\sounds\\ on the PC.");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 20);

    /* grid: content container is KAST_V_RES - 44 tall; height follows
       the tiles (max 4 rows at KAST_SFX_MAX) and anchors to the bottom */
    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, KAST_H_RES, LV_SIZE_CONTENT);
    lv_obj_align(s_grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, SFX_PAD, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, SFX_PAD, 0);
    lv_obj_set_style_pad_column(s_grid, SFX_PAD, 0);
    lv_obj_set_scrollable(s_grid, false);

    /* reflect whatever the store already holds (usually the catalog the
       relay pushed at connect), then poll for changes */
    s_built_version = kast_relay_sfx_version();
    sfx_grid_rebuild();
    s_poll = lv_timer_create(sfx_poll_cb, SFX_POLL_MS, NULL);
}

void kast_sfx_unload(void)
{
    lv_timer_delete(s_poll);
    s_poll = NULL;
    s_grid = NULL;
    s_hint = NULL;
    s_tile_count = 0;
    for (int i = 0; i < KAST_SFX_MAX; i++) {
        s_tile_id[i][0] = '\0';
    }
}
