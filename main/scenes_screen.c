/*
 * scenes_screen.c — KAST Scene Controller page (Phase 4).
 *
 * Tiles are the user's own OBS scenes, chosen in the Companion app's
 * Scene Tiles picker; the relay pushes {"type":"scenes.catalog","scenes",
 * "current"} on connect and on every OBS program-scene change, so the
 * active scene stays highlighted even when it was switched from OBS or
 * another controller. A tap sends {"type":"scene.switch","name"}.
 *
 * The device only displays and switches: scenes are created, named, and
 * deleted in OBS — KAST never mutates the user's scene list.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "relay.h"
#include "ui.h"

#define SCENE_TILE_W   226
#define SCENE_TILE_H   62
#define SCENE_PAD      6
#define SCENE_POLL_MS  500

static lv_obj_t *s_grid;
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static uint32_t  s_built_version;
static uint32_t  s_tile_count;

/* snapshot names at build time — the relay store changes under us */
static char s_tile_name[KAST_SCENES_MAX][KAST_SCENE_NAME_LEN];
static lv_obj_t *s_tiles[KAST_SCENES_MAX];

static void scene_tile_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    if (name != NULL && name[0] != '\0') {
        kast_relay_scene_switch(name);
    }
}

/* Active-scene highlight: green fill + dark text on the live tile. Runs
   on the poll tick — cheap enough to re-apply every catalog change. */
static void scenes_highlight(void)
{
    char current[KAST_SCENE_NAME_LEN] = "";
    kast_relay_scenes_current(current, sizeof(current));
    for (uint32_t i = 0; i < s_tile_count; i++) {
        const bool active = strcmp(s_tile_name[i], current) == 0;
        lv_obj_set_style_bg_color(s_tiles[i],
                                  active ? KAST_COLOR_GREEN : KAST_COLOR_CARD, 0);
        lv_obj_set_style_border_color(s_tiles[i],
                                      active ? KAST_COLOR_GREEN : KAST_COLOR_GREEN_DIM, 0);
        lv_obj_t *label = lv_obj_get_child(s_tiles[i], 0);
        if (label != NULL) {
            lv_obj_set_style_text_color(label,
                                        active ? KAST_COLOR_BG : KAST_COLOR_TEXT, 0);
        }
    }
}

static void scene_tile_add(lv_obj_t *parent, const char *name, uint32_t idx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, SCENE_TILE_W, SCENE_TILE_H);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 16, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, scene_tile_cb, LV_EVENT_CLICKED,
                        (void *)s_tile_name[idx]);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, SCENE_TILE_W - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
    s_tiles[idx] = btn;
}

static void scenes_grid_rebuild(void)
{
    lv_obj_clean(s_grid);
    s_tile_count = 0;

    const uint32_t count = kast_relay_scenes_count();
    if (count == 0) {
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    for (uint32_t i = 0; i < count && i < KAST_SCENES_MAX; i++) {
        if (!kast_relay_scenes_get(i, s_tile_name[i], sizeof(s_tile_name[i]))) {
            continue;
        }
        scene_tile_add(s_grid, s_tile_name[i], i);
        s_tile_count++;
    }
    scenes_highlight();
}

static void scenes_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (kast_relay_scenes_version() != s_built_version) {
        s_built_version = kast_relay_scenes_version();
        scenes_grid_rebuild();
    }
}

void kast_scenes_load(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, KAST_COLOR_BG, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SCENES");
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
                      "No scene tiles yet.\n"
                      "Pick your OBS scenes in the Companion\n"
                      "app's Scene Tiles window.");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 20);

    /* two columns of wider tiles — scene names run longer than sound ids */
    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, KAST_H_RES, LV_SIZE_CONTENT);
    lv_obj_align(s_grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, SCENE_PAD, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, SCENE_PAD, 0);
    lv_obj_set_style_pad_column(s_grid, SCENE_PAD, 0);
    lv_obj_set_scrollable(s_grid, false);

    s_built_version = kast_relay_scenes_version();
    scenes_grid_rebuild();
    s_poll = lv_timer_create(scenes_poll_cb, SCENE_POLL_MS, NULL);
}

void kast_scenes_unload(void)
{
    lv_timer_delete(s_poll);
    s_poll = NULL;
    s_grid = NULL;
    s_hint = NULL;
    s_tile_count = 0;
    for (int i = 0; i < KAST_SCENES_MAX; i++) {
        s_tile_name[i][0] = '\0';
        s_tiles[i] = NULL;
    }
}
