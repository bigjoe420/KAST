/*
 * simple_screens.c — placeholder pages for CHAT / SCENES / SFX.
 *
 * Real content arrives with their phases (chat in Phase 3 with the PC
 * relay, scenes/SFX in Phase 4 over OBS WebSocket). These exist so the
 * router is fully navigable now and the nav path gets owner-verified
 * before the heavy modules land on top.
 */
#include "lvgl.h"

#include "ui.h"

static void placeholder_page(lv_obj_t *parent, const char *title, const char *note)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(t, KAST_COLOR_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *accent = lv_obj_create(parent);
    lv_obj_set_size(accent, 160, 4);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_radius(accent, 2, 0);
    lv_obj_set_style_bg_color(accent, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_shadow_color(accent, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_width(accent, 12, 0);

    lv_obj_t *n = lv_label_create(parent);
    lv_label_set_text(n, note);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(n, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 120);
}

void kast_chat_load(lv_obj_t *parent)
{
    placeholder_page(parent, "CHAT", "Live chat arrives with the PC relay (Phase 3)");
}

void kast_chat_unload(void)
{
}

void kast_scenes_load(lv_obj_t *parent)
{
    placeholder_page(parent, "SCENES", "OBS scene control arrives in Phase 4");
}

void kast_scenes_unload(void)
{
}

void kast_sfx_load(lv_obj_t *parent)
{
    placeholder_page(parent, "SFX", "Soundboard triggers arrive in Phase 4");
}

void kast_sfx_unload(void)
{
}
