/*
 * ui.c — KAST Home screen (LVGL 9, esp_lvgl_port).
 *
 * Layout (480x480):
 *   [status bar]  KAST .................... WIFI -- · OBS --
 *   [title]      KICK AUXILIARY / STREAMING TERMINAL over a breathing bar
 *   [modules]    CHAT / SCENES / SFX — large rounded buttons, green keyline,
 *                green fill + dark text while pressed
 *   [towers]     Kick-'K' double-helix towers on the screen edges —
 *                float-math pseudo-3D, two-pass z-sort, flat Kick lime,
 *                green fire rising behind the strands and above the top
 *   [eq strip]   seven animated bars under the column — bars slamming
 *                the top release a "K" that floats up and fades
 *   [footer]     status line
 *
 * Theme: Kick green on near-black. All motion is small-area; nothing
 * here redraws the full frame outside LVGL's own dirty tracking.
 */
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
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
#define KAST_TOWERS   2
/* Kick-'K' helix towers on the screen edges */
#define KAST_TOWER_W     44
#define KAST_TOWER_H     188     /* canvas: 148 helix + 40px flame headroom above */
#define KAST_HELIX_Y0    40      /* helix band starts this far down the canvas */
#define KAST_HELIX_STEP_Y 13    /* vertical breathing room between nodes */
#define KAST_HELIX_AMP    10.0f /* strand horizontal amplitude, px */
#define KAST_HELIX_TWIST  0.45f /* radians of phase between nodes */
#define KAST_HELIX_DEPTH  0.35f /* how much z scales size: 1.0f +/- this */
#define KAST_ROT_SPEED    0.03f /* radians per frame, per tower */

static lv_display_t *s_disp;
static lv_obj_t *s_status_label;   /* footer status line */
static lv_obj_t *s_net_label;      /* status-bar Wi-Fi/OBS state */
static lv_obj_t *s_eq_bars[KAST_EQ_BARS];
static lv_obj_t *s_tower_canvas[KAST_TOWERS];
/* Packed RGB565 pixel buffers the tower renderer draws into (internal RAM) */
static uint16_t s_tower_buf[KAST_TOWERS][KAST_TOWER_W * KAST_TOWER_H];
static float s_tower_rot[KAST_TOWERS] = {0.0f, 3.14159265f};
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

static lv_obj_t *module_button(lv_obj_t *parent, const char *name)
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
    return btn;
}

/* ------------------------------------------------------------------ */
/* Equalizer strip — lightweight motion                                */
/* ------------------------------------------------------------------ */

#define KAST_MAX_PARTICLES 8
#define KAST_PARTICLE_LIFE 50   /* ticks (~1.5 s at 30 Hz) */

typedef struct {
    lv_obj_t *obj;
    float x, y;
    float vx, vy;
    int life;
} k_particle_t;

static k_particle_t s_particles[KAST_MAX_PARTICLES];
static uint32_t s_bar_cooldown[KAST_EQ_BARS];

/* A bar slammed the top: release a "K" just above it that floats off
 * and fades. Pool is small; spawn bursts just skip when full. */
static void spawn_k_particle(int x, int y)
{
    for (int i = 0; i < KAST_MAX_PARTICLES; i++) {
        if (s_particles[i].obj == NULL) {
            lv_obj_t *label = lv_label_create(lv_screen_active());
            lv_label_set_text(label, "K");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
            lv_obj_set_style_text_color(label, KAST_COLOR_GREEN, 0);
            lv_obj_set_pos(label, x, y);
            s_particles[i].obj = label;
            s_particles[i].x = (float)x;
            s_particles[i].y = (float)y;
            s_particles[i].vx = ((float)(esp_random() % 200) - 100.0f) / 100.0f;
            s_particles[i].vy = -2.2f;
            s_particles[i].life = KAST_PARTICLE_LIFE;
            return;
        }
    }
}

static void update_k_particles(void)
{
    for (int i = 0; i < KAST_MAX_PARTICLES; i++) {
        k_particle_t *p = &s_particles[i];
        if (p->obj == NULL) {
            continue;
        }
        p->life--;
        if (p->life <= 0 || p->y < -40.0f) {
            lv_obj_delete(p->obj);
            p->obj = NULL;
            continue;
        }
        p->vy *= 0.96f;   /* ease the rise */
        p->y += p->vy;
        p->x += p->vx;
        lv_obj_set_pos(p->obj, (int32_t)p->x, (int32_t)p->y);
        lv_obj_set_style_opa(p->obj, (lv_opa_t)((p->life * 255) / KAST_PARTICLE_LIFE), 0);
    }
}

static void eq_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t t = lv_tick_get();
    for (int i = 0; i < KAST_EQ_BARS; i++) {
        /* two blended sines per bar: organic, deterministic, cheap */
        float ph = (float)t / 1000.0f;
        float v = sinf(ph * 2.1f + i * 0.9f) * 0.5f +
                  sinf(ph * 3.7f + i * 1.7f) * 0.5f;
        float norm = (v + 1.0f) / 2.0f;          /* 0..1 across the full strip */
        int h = 10 + (int)(norm * 30.0f);        /* 10..40 px, fits the strip */
        lv_obj_set_height(s_eq_bars[i], h);
        lv_obj_align_to(s_eq_bars[i], lv_obj_get_parent(s_eq_bars[i]),
                        LV_ALIGN_BOTTOM_MID, (i - (KAST_EQ_BARS - 1) / 2) * 22, 0);

        /* bar slammed the top: pop a "K" off it (per-bar cooldown) */
        if (h >= 38 && t - s_bar_cooldown[i] > 1500) {
            s_bar_cooldown[i] = t;
            const int bx = 240 + (i - (KAST_EQ_BARS - 1) / 2) * 22 - 12;
            const int by = 396 + (48 - h) - 26;
            spawn_k_particle(bx, by);
        }
    }
    update_k_particles();
}

/* ------------------------------------------------------------------ */
/* Kick-'K' helix towers — two-pass pseudo-3D                          */
/* ------------------------------------------------------------------ */
/* One DNA-style strand pair per screen edge, nodes drawn as stylized
 * Kick "K" logos instead of dots. X/Y/Z stay float through the math
 * and cast to int only at the brush. Flat Kick-lime color throughout —
 * depth is read from size scaling and draw order, not brightness.
 * Per frame: pass 0 paints the thin rungs and the shrunken back-facing
 * K's; pass 1 paints the thick rungs and the enlarged front-facing K's —
 * so overlap is correct with no depth sort. */

/* Kick lime (0x53FC18) packed for the RGB565 canvas buffer — flat color,
 * depth is read from size and draw order only. */
#define KAST_KICK_LIME  ((uint16_t)0x57E3)
/* Flame color ramp: deep green -> Kick lime -> hot yellow-green */
#define KAST_FLAMES     28      /* fire particles per tower */
#define KAST_FLAME_HOT  (rgb565(170, 255, 80))
#define KAST_FLAME_DIM  (rgb565(16, 60, 16))

typedef struct {
    float x, y;
    float vy;
    int life;
    int max_life;
} flame_t;

static flame_t s_flames[KAST_TOWERS][KAST_FLAMES];

static uint16_t *s_draw_buf;          /* tower being drawn (set per frame) */

static inline void k_brush(int x, int y, int r, uint16_t color);

/* Pack 8-bit RGB into the RGB565 word the canvas buffer stores. */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Linear blend between two packed RGB565 colors. */
static uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
    const int r1 = (a >> 11) & 0x1F, g1 = (a >> 5) & 0x3F, b1 = a & 0x1F;
    const int r2 = (b >> 11) & 0x1F, g2 = (b >> 5) & 0x3F, b2 = b & 0x1F;
    return (uint16_t)((r1 + (int)((float)(r2 - r1) * t)) << 11 |
                      (g1 + (int)((float)(g2 - g1) * t)) << 5 |
                      (b1 + (int)((float)(b2 - b1) * t)));
}

/* Fire ramp by remaining-life fraction: hottest when youngest. */
static inline uint16_t flame_col(float f)
{
    if (f > 0.66f) {
        return lerp565(KAST_KICK_LIME, KAST_FLAME_HOT, (f - 0.66f) / 0.34f);
    }
    return lerp565(KAST_FLAME_DIM, KAST_KICK_LIME, f / 0.66f);
}

static void draw_flame(int x, int y, float f)
{
    const uint16_t c = flame_col(f);
    k_brush(x, y, f > 0.4f ? 1 : 0, c);
    if (f > 0.6f) {
        k_brush(x, y - 2, 0, c);   /* hot tongues stretch upward */
    }
}

/* Advance the fire one tick: respawn dead flames at the base, rise and
 * flicker the live ones, draw behind the helix so K's silhouette. */
static void update_flames(int idx)
{
    for (int i = 0; i < KAST_FLAMES; i++) {
        flame_t *f = &s_flames[idx][i];
        if (f->life <= 0) {
            f->x = 6.0f + (float)(esp_random() % (KAST_TOWER_W - 12));
            f->y = (float)(KAST_TOWER_H - 2);
            f->vy = -2.0f - (float)(esp_random() % 120) / 100.0f;
            f->max_life = 55 + (int)(esp_random() % 45);
            f->life = f->max_life;
        }
        f->life--;
        f->y += f->vy;
        f->x += ((float)(esp_random() % 100) - 50.0f) / 200.0f;
        if (f->life > 0) {
            draw_flame((int)f->x, (int)f->y, (float)f->life / (float)f->max_life);
        }
    }
}

static inline void k_brush(int x, int y, int r, uint16_t color)
{
    if (x < 0 || x >= KAST_TOWER_W || y < 0 || y >= KAST_TOWER_H) {
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int px = x + dx;
            const int py = y + dy;
            if (px >= 0 && px < KAST_TOWER_W && py >= 0 && py < KAST_TOWER_H) {
                s_draw_buf[py * KAST_TOWER_W + px] = color;
            }
        }
    }
}

static void k_line(float x1, float y1, float x2, float y2, float thick, uint16_t color)
{
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len = sqrtf(dx * dx + dy * dy);
    const int steps = (int)(len * 2.0f) + 1;
    const int r = (int)(thick * 0.5f + 0.5f);
    for (int s = 0; s <= steps; s++) {
        const float t = (float)s / (float)steps;
        k_brush((int)(x1 + dx * t), (int)(y1 + dy * t), r, color);
    }
}

/* Stylized "K": one vertical stem, two angled arms; size follows z_scale.
 * Arms point outward (away from the strand crossing) so the two K's
 * never collide when the strands overlap. */
static void draw_kick_k(int x, int y, float z_scale, uint16_t color)
{
    const float s = z_scale;
    const float th = 2.0f * s;
    const float dir = (x < KAST_TOWER_W / 2) ? -1.0f : 1.0f;
    k_line((float)x, (float)y - 4.0f * s, (float)x, (float)y + 4.0f * s, th, color);
    k_line((float)x, (float)y - 0.5f * s, (float)x + dir * 3.8f * s, (float)y - 4.0f * s, th, color);
    k_line((float)x, (float)y + 0.5f * s, (float)x + dir * 3.8f * s, (float)y + 4.0f * s, th, color);
}

static void draw_helix(lv_obj_t *canvas, int idx)
{
    s_draw_buf = s_tower_buf[idx];
    const int npix = KAST_TOWER_W * KAST_TOWER_H;
    const uint16_t bg = rgb565(5, 8, 5);
    for (int i = 0; i < npix; i++) {
        s_draw_buf[i] = bg;
    }

    const int32_t cx = KAST_TOWER_W / 2;
    const float rot = s_tower_rot[idx];

    update_flames(idx);   /* fire first: the helix draws over it */

    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        for (int y = KAST_HELIX_Y0 + 8; y <= KAST_TOWER_H - 8; y += KAST_HELIX_STEP_Y, n++) {
            const float ph = rot + (float)n * KAST_HELIX_TWIST;
            const float sn = sinf(ph);
            const float cs = cosf(ph);            /* z of strand A */
            const int x1 = cx + (int)(KAST_HELIX_AMP * sn);
            const int x2 = cx - (int)(KAST_HELIX_AMP * sn);

            if (pass == 0) {
                if (cs < 0.0f) {
                    /* strand A is back: thin rung now, thick rung in pass 1 */
                    k_line((float)x1, (float)y, (float)x2, (float)y, 1.2f, KAST_KICK_LIME);
                    draw_kick_k(x1, y, 1.0f + KAST_HELIX_DEPTH * cs, KAST_KICK_LIME);
                } else {
                    /* strand B is back */
                    draw_kick_k(x2, y, 1.0f - KAST_HELIX_DEPTH * cs, KAST_KICK_LIME);
                }
            } else if (cs >= 0.0f) {
                k_line((float)x1, (float)y, (float)x2, (float)y, 2.2f, KAST_KICK_LIME);
                draw_kick_k(x1, y, 1.0f + KAST_HELIX_DEPTH * cs, KAST_KICK_LIME);
            }
        }
    }
    lv_obj_invalidate(canvas);
}

static void build_tower(lv_obj_t *scr, lv_align_t align, int32_t x_ofs, int idx)
{
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_size(canvas, KAST_TOWER_W, KAST_TOWER_H);
    lv_obj_align(canvas, align, x_ofs, 10);   /* helix sits same screen spot as before */
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, s_tower_buf[idx], KAST_TOWER_W, KAST_TOWER_H,
                         LV_COLOR_FORMAT_RGB565);
    draw_helix(canvas, idx);
    s_tower_canvas[idx] = canvas;
}

static void tower_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    for (int i = 0; i < KAST_TOWERS; i++) {
        s_tower_rot[i] += KAST_ROT_SPEED;    /* ~6 s per turn at 30 Hz */
        draw_helix(s_tower_canvas[i], i);
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

    s_net_label = lv_label_create(bar);
    lv_label_set_text(s_net_label, "WIFI --  \xC2\xB7  OBS --");
    lv_obj_set_style_text_color(s_net_label, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_net_label, LV_ALIGN_RIGHT_MID, -16, 0);

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

    module_button(menu, "CHAT");
    module_button(menu, "SCENES");
    module_button(menu, "SFX");

    /* Kick-'K' helix towers on the screen edges, clear of the buttons */
    build_tower(scr, LV_ALIGN_CENTER, -216, 0);
    build_tower(scr, LV_ALIGN_CENTER, 216, 1);
    lv_timer_create(tower_timer_cb, 33, NULL);

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
    lv_timer_create(eq_timer_cb, 33, NULL);

    /* Footer: status line */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "READY");
    lv_obj_set_style_text_color(s_status_label, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 16, -16);
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

/* Called from the Wi-Fi event handler (its own task) — must take the
 * LVGL lock before touching widgets. connected flips the label to Kick
 * green; text overrides the Wi-Fi word while disconnected (WIFI ... /
 * WIFI RETRY / WIFI FAIL). NULL restores the neutral placeholder. */
void kast_ui_net_status(bool connected, const char *text)
{
    const char *wifi_text = connected ? "WIFI OK"
                          : (text ? text : "WIFI --");
    char buf[32];
    snprintf(buf, sizeof(buf), "%s  \xC2\xB7  OBS --", wifi_text);

    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_label_set_text(s_net_label, buf);
    lv_obj_set_style_text_color(s_net_label,
                                connected ? KAST_COLOR_GREEN : KAST_COLOR_TEXT_DIM, 0);
    lvgl_port_unlock();
}

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
