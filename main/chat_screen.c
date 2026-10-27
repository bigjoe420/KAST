/*
 * chat_screen.c — KAST live chat terminal (Phase 3).
 *
 * Slice 1 (receive): relay.c keeps a fixed ring of recent lines (K-6:
 * nothing allocates per message); this screen drains it on a 200 ms poll
 * while loaded and appends rows to a fixed label pool — once the pool
 * fills, the oldest label is recycled and moved under the newest, a
 * classic terminal ring. Rows are single recolor-enabled labels
 * ("#53FC18 user#  message"); the view auto-follows the newest line.
 *
 * Slice 2 (send): a composer strip above the footer arms on tap — stock
 * QWERTY keyboard (panel-sized), SEND + close buttons, OK key sends.
 * Armed, the strip moves up directly above the keyboard (it would
 * otherwise render under the 200 px keyboard overlay) and the list
 * shrinks so messages stay visible; disarmed everything returns.
 * Text goes to the relay as {"type":"chat.send","text"}; the Kick
 * injection lives on the PC (D-4). In --simulate-chat bench mode the
 * relay bounces the line back as user "you", closing the loop on glass.
 * A failed send prints "send failed — relay link down" on the footer
 * for 3 s instead of failing silently.
 *
 * Slice 3 (macros): a four-chip quick-reply row sits between the list
 * and the composer. Tap a set chip to send it, long-press (or tap an
 * empty chip) to edit: the composer arms with the slot text and a SET
 * M# button replaces SEND — storing the textarea to NVS (empty clears
 * the slot). Chips show truncated text, or a dim M# when unset.
 *
 * Slice 4 (moderation): long-press any chat row to open a moderation
 * card — DELETE / TIMEOUT / BAN go to the relay as
 * {"type":"chat.mod","action","id","user"} (timeout duration is the
 * relay's default). Rows carry the relay-normalized message id; the
 * simulate-mode relay acks with {"type":"mod.ack"} and the footer shows
 * the confirmation. Rows without an id predate the protocol and can't
 * be moderated.
 *
 * Slice 5 (emotes): rows are wrap-flow containers — a recolor label plus
 * two fixed 24x24 img slots. Chat frames carry a comma-separated emotes
 * list; cached codes (RAM pool mirrored to /sdcard/emotes/) render as
 * images with their :code: token stripped, uncached codes ask the relay
 * ({"type":"emote.req"}) and stay as text until the tile lands. The relay
 * pushes raw RGB565 base64 — nothing decodes PNG on-device.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"

#include "config.h"
#include "emote.h"
#include "relay.h"
#include "ui.h"

static const char *TAG = "chat";

#define KAST_CHAT_ROWS   11
#define KAST_POLL_MS     200
#define KAST_BATCH_MAX   8
#define KAST_INPUT_MAX   200

static lv_obj_t     *s_list;
static lv_obj_t     *s_rows[KAST_CHAT_ROWS];      /* row containers */
static lv_obj_t     *s_row_label[KAST_CHAT_ROWS]; /* text half of a row */
static lv_obj_t     *s_row_img[KAST_CHAT_ROWS][2];/* inline emote slots */
static uint32_t      s_used;     /* rows currently holding a message */
static lv_obj_t     *s_empty;    /* waiting-for-frames placeholder */
static lv_obj_t     *s_counter;  /* footer: rendered message count */
static lv_timer_t   *s_poll;
static uint32_t      s_seen;
static kast_chat_line_t s_batch[KAST_BATCH_MAX];

/* composer (send) widgets */
static lv_obj_t     *s_strip;
static lv_obj_t     *s_ta;
static lv_obj_t     *s_kb;
static lv_obj_t     *s_send_btn;
static lv_obj_t     *s_close_btn;
static bool          s_armed;
static uint32_t      s_hint_until;   /* footer failure hint expiry (lv_tick) */

/* quick-reply macro row (slice 3) */
static lv_obj_t     *s_macro_row;
static lv_obj_t     *s_macro_btn[KAST_MACRO_COUNT];
static lv_obj_t     *s_macro_label[KAST_MACRO_COUNT];
static char          s_macro_text[KAST_MACRO_COUNT][KAST_MACRO_TEXT_LEN];
static int           s_macro_edit = -1;   /* slot being edited, -1 = normal */
static lv_obj_t     *s_macro_save_btn;
static lv_obj_t     *s_macro_save_label;

/* moderation card (slice 4) */
typedef struct {
    char id[KAST_CHAT_ID_LEN];
    char user[KAST_CHAT_USER_LEN];
    char text[KAST_CHAT_TEXT_LEN];
} row_meta_t;
static row_meta_t    s_row_meta[KAST_CHAT_ROWS];  /* keyed to s_rows index */
static lv_obj_t     *s_mod_overlay;
static lv_obj_t     *s_mod_title;
static lv_obj_t     *s_mod_preview;
static char          s_mod_id[KAST_CHAT_ID_LEN];
static char          s_mod_user[KAST_CHAT_USER_LEN];

#define KAST_KB_H  200   /* stock keyboard height */
/* disarmed stack (content 436 tall): list 0..312, macro row 316..352,
 * composer strip 356..400, footer 400..436 */
#define KAST_LIST_H_DISARMED 312
#define KAST_LIST_H_ARMED    (KAST_V_RES - 44 /*status*/ - KAST_KB_H - 44 /*composer*/ - 8 /*gap*/)

static void copy_clipped(char *dst, size_t dst_sz, const char *src)
{
    size_t n = strnlen(src, dst_sz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Remove every ":code:" occurrence from text (in place) — used once an
 * emote renders as an image so the token doesn't show twice. */
static void strip_token(char *text, const char *code)
{
    char pat[20];
    snprintf(pat, sizeof(pat), ":%s:", code);
    const size_t plen = strlen(pat);
    char *p;
    while ((p = strstr(text, pat)) != NULL) {
        memmove(p, p + plen, strlen(p + plen) + 1);
    }
}

/* Emote half of a row: for each code in the line's comma list, use the
 * cached image when we have it (token stripped from the text) or ask the
 * relay and leave the token visible for next time. Renders at most the
 * two fixed img slots (K-6). Returns how many slots were filled. */
static int row_apply_emotes(uint32_t row_idx, char *text, const char *emotes)
{
    int shown = 0;
    if (emotes[0] == '\0') {
        return 0;
    }
    char list[40];
    copy_clipped(list, sizeof(list), emotes);
    char *save = NULL;
    for (char *code = strtok_r(list, ",", &save);
         code != NULL && shown < 2;
         code = strtok_r(NULL, ",", &save)) {
        const lv_image_dsc_t *dsc = kast_emote_get(code);
        if (dsc != NULL) {
            strip_token(text, code);
            lv_image_set_src(s_row_img[row_idx][shown], dsc);
            lv_obj_set_hidden(s_row_img[row_idx][shown], false);
            shown++;
        } else {
            kast_emote_request(code);
        }
    }
    return shown;
}

static void chat_append(const kast_chat_line_t *line)
{
    lv_obj_set_hidden(s_empty, true);

    lv_obj_t *row;
    uint32_t idx;
    if (s_used < KAST_CHAT_ROWS) {
        idx = s_used++;
        row = s_rows[idx];
    } else {
        /* pool full: recycle the oldest row, move it under the newest */
        row = s_rows[0];
        lv_obj_move_to_index(row, -1);
        memmove(&s_rows[0], &s_rows[1], sizeof(s_rows[0]) * (KAST_CHAT_ROWS - 1));
        idx = KAST_CHAT_ROWS - 1;
        s_rows[idx] = row;
    }

    /* emotes resolve against a mutable text copy: cached codes render as
     * images with their token stripped, uncached ones stay as text while
     * the relay is asked */
    char text[KAST_CHAT_TEXT_LEN];
    copy_clipped(text, sizeof(text), line->text);
    lv_obj_set_hidden(s_row_img[idx][0], true);
    lv_obj_set_hidden(s_row_img[idx][1], true);
    const int shown = row_apply_emotes(idx, text, line->emotes);

    lv_label_set_text_fmt(s_row_label[idx], "#53FC18 %s#  %s", line->user, text);
    /* give the label the remaining width so imgs sit on the same line */
    lv_obj_set_width(s_row_label[idx], KAST_H_RES - 12 - shown * (KAST_EMOTE_W + 4));

    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    copy_clipped(s_row_meta[idx].user, sizeof(s_row_meta[idx].user), line->user);
    copy_clipped(s_row_meta[idx].text, sizeof(s_row_meta[idx].text), text);
    copy_clipped(s_row_meta[idx].id, sizeof(s_row_meta[idx].id), line->id);
    s_seen++;
}

static void drain_batch(void)
{
    bool got = false;
    for (;;) {
        uint32_t n = kast_relay_chat_read(s_batch, KAST_BATCH_MAX);
        if (n == 0) {
            break;
        }
        got = true;
        for (uint32_t i = 0; i < n; i++) {
            chat_append(&s_batch[i]);
        }
    }
    if (got && lv_tick_get() > s_hint_until) {
        lv_label_set_text_fmt(s_counter, "%u messages", (unsigned)s_seen);
    }
}

static void chat_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    drain_batch();
    /* relay bench ack for a moderation action — confirm on the footer */
    char action[16];
    if (kast_relay_chat_mod_ack(action, sizeof(action)) &&
        lv_tick_get() > s_hint_until) {
        lv_label_set_text_fmt(s_counter, "%s confirmed by relay", action);
        s_hint_until = lv_tick_get() + 3000;
    }
}

/* ------------------------------------------------------------------ */
/* Composer (send path)                                                */
/* ------------------------------------------------------------------ */

static void footer_hint_fail(void)
{
    /* silent failures read as "the check key is broken" — say it on glass */
    lv_label_set_text(s_counter, "send failed — relay link down");
    s_hint_until = lv_tick_get() + 3000;
}

static void composer_disarm(void)
{
    s_armed = false;
    if (s_macro_edit >= 0) {
        lv_obj_set_style_border_color(s_macro_btn[s_macro_edit], KAST_COLOR_GREEN_DIM, 0);
        lv_obj_set_style_border_width(s_macro_btn[s_macro_edit], 1, 0);
        s_macro_edit = -1;
    }
    lv_obj_set_hidden(s_macro_save_btn, true);
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_set_hidden(s_kb, true);
    lv_obj_set_hidden(s_send_btn, true);
    lv_obj_set_hidden(s_close_btn, true);
    /* composer back above the footer, macro row returns, list regains height */
    lv_obj_align(s_strip, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_hidden(s_macro_row, false);
    lv_obj_set_height(s_list, KAST_LIST_H_DISARMED);
}

static void composer_arm(void)
{
    s_armed = true;
    /* the keyboard overlays the bottom 200 px of the content area — park
     * the composer directly above it or the user types blind under it */
    lv_obj_align(s_strip, LV_ALIGN_BOTTOM_MID, 0, -(KAST_KB_H + 44));
    lv_obj_set_height(s_list, KAST_LIST_H_ARMED);
    lv_obj_set_hidden(s_macro_row, true);
    lv_obj_set_hidden(s_kb, false);
    if (s_macro_edit >= 0) {
        lv_obj_set_hidden(s_send_btn, true);
        lv_obj_set_hidden(s_macro_save_btn, false);
    } else {
        lv_obj_set_hidden(s_send_btn, false);
    }
    lv_obj_set_hidden(s_close_btn, false);
    lv_keyboard_set_textarea(s_kb, s_ta);
}

static void composer_try_send(void)
{
    const char *text = lv_textarea_get_text(s_ta);
    if (text[0] == '\0') {
        composer_disarm();
        return;
    }
    esp_err_t err = kast_relay_chat_send(text);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "chat.send: %s", text);
        /* instant ack on the footer — with real Kick there is no chat
         * bounce to confirm the send until chat read lands */
        lv_label_set_text(s_counter, "sent — relay has it");
        s_hint_until = lv_tick_get() + 3000;
        lv_textarea_set_text(s_ta, "");
        composer_disarm();
    } else {
        ESP_LOGW(TAG, "chat.send failed (%s) — relay link down?", esp_err_to_name(err));
        footer_hint_fail();
    }
}

static void composer_ta_focus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_armed) {
        composer_arm();
    }
}

static void composer_ta_defocus_cb(lv_event_t *e)
{
    (void)e;
    if (s_armed) {
        composer_disarm();
    }
}

static void composer_send_cb(lv_event_t *e)
{
    (void)e;
    lv_event_stop_bubbling(e);   /* K-5: strip buttons sit over the strip */
    composer_try_send();
}

static void composer_close_cb(lv_event_t *e)
{
    (void)e;
    lv_event_stop_bubbling(e);
    lv_textarea_set_text(s_ta, "");
    composer_disarm();
}

static void composer_kb_ready_cb(lv_event_t *e)
{
    (void)e;
    composer_try_send();
}

/* ------------------------------------------------------------------ */
/* Quick-reply macro row (slice 3)                                     */
/* ------------------------------------------------------------------ */

static void macro_refresh_chip(uint8_t slot)
{
    const char *text = s_macro_text[slot];
    if (text[0] == '\0') {
        lv_label_set_text_fmt(s_macro_label[slot], "M%u", (unsigned)slot + 1);
        lv_obj_set_style_text_color(s_macro_label[slot], KAST_COLOR_TEXT_DIM, 0);
    } else {
        /* Montserrat has no U+2026 glyph — ASCII "..." only */
        if (strlen(text) > 14) {
            lv_label_set_text_fmt(s_macro_label[slot], "%.14s...", text);
        } else {
            lv_label_set_text(s_macro_label[slot], text);
        }
        lv_obj_set_style_text_color(s_macro_label[slot], KAST_COLOR_TEXT, 0);
    }
}

static void macro_send(uint8_t slot)
{
    esp_err_t err = kast_relay_chat_send(s_macro_text[slot]);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "macro %u sent: %s", (unsigned)slot + 1, s_macro_text[slot]);
        lv_label_set_text_fmt(s_counter, "M%u sent", (unsigned)slot + 1);
        s_hint_until = lv_tick_get() + 3000;
    } else {
        ESP_LOGW(TAG, "macro %u send failed (%s) — relay link down?",
                 (unsigned)slot + 1, esp_err_to_name(err));
        footer_hint_fail();
    }
}

static void macro_begin_edit(uint8_t slot)
{
    if (s_macro_edit >= 0) {
        lv_obj_set_style_border_color(s_macro_btn[s_macro_edit], KAST_COLOR_GREEN_DIM, 0);
        lv_obj_set_style_border_width(s_macro_btn[s_macro_edit], 1, 0);
    }
    s_macro_edit = (int)slot;
    lv_obj_set_style_border_color(s_macro_btn[slot], KAST_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(s_macro_btn[slot], 3, 0);
    lv_textarea_set_text(s_ta, s_macro_text[slot]);
    if (!s_armed) {
        composer_arm();
    }
    /* composer_arm reveals SEND — in edit mode SET M# owns that slot */
    lv_obj_set_hidden(s_send_btn, true);
    lv_obj_set_hidden(s_macro_save_btn, false);
    lv_label_set_text_fmt(s_macro_save_label, "SET M%u", (unsigned)slot + 1);
}

static void macro_chip_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        macro_begin_edit(slot);
        return;
    }
    if (s_macro_edit == (int)slot) {
        return;   /* the chip under edit is not a send target */
    }
    if (s_macro_text[slot][0] == '\0') {
        macro_begin_edit(slot);
    } else {
        macro_send(slot);
    }
}

static void macro_save_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (s_macro_edit < 0) {
        return;
    }
    uint8_t slot = (uint8_t)s_macro_edit;
    strlcpy(s_macro_text[slot], lv_textarea_get_text(s_ta), KAST_MACRO_TEXT_LEN);
    esp_err_t err = kast_config_save_macro(slot, s_macro_text[slot]);
    ESP_LOGI(TAG, "macro %u saved: \"%s\" (%s)", (unsigned)slot + 1,
             s_macro_text[slot], esp_err_to_name(err));
    macro_refresh_chip(slot);
    lv_textarea_set_text(s_ta, "");
    composer_disarm();
}

static void build_macro_row(lv_obj_t *parent)
{
    s_macro_row = lv_obj_create(parent);
    lv_obj_set_size(s_macro_row, KAST_H_RES, 36);
    lv_obj_align(s_macro_row, LV_ALIGN_BOTTOM_MID, 0, -84);
    lv_obj_set_style_bg_opa(s_macro_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_macro_row, 0, 0);
    lv_obj_set_style_pad_all(s_macro_row, 0, 0);
    lv_obj_set_scrollable(s_macro_row, false);

    for (uint8_t i = 0; i < KAST_MACRO_COUNT; i++) {
        s_macro_btn[i] = lv_button_create(s_macro_row);
        lv_obj_set_size(s_macro_btn[i], 110, 32);
        lv_obj_align(s_macro_btn[i], LV_ALIGN_LEFT_MID, 8 + (int)i * 118, 0);
        lv_obj_set_style_radius(s_macro_btn[i], 8, 0);
        lv_obj_set_style_bg_color(s_macro_btn[i], KAST_COLOR_CARD, 0);
        lv_obj_set_style_border_color(s_macro_btn[i], KAST_COLOR_GREEN_DIM, 0);
        lv_obj_set_style_border_width(s_macro_btn[i], 1, 0);
        lv_obj_set_style_bg_color(s_macro_btn[i], KAST_COLOR_GREEN_DIM, LV_STATE_PRESSED);
        lv_obj_add_event_cb(s_macro_btn[i], macro_chip_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_add_event_cb(s_macro_btn[i], macro_chip_cb, LV_EVENT_LONG_PRESSED,
                            (void *)(uintptr_t)i);

        s_macro_label[i] = lv_label_create(s_macro_btn[i]);
        lv_obj_set_width(s_macro_label[i], 94);
        lv_label_set_long_mode(s_macro_label[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_macro_label[i], &lv_font_montserrat_12, 0);
        lv_obj_center(s_macro_label[i]);
        macro_refresh_chip(i);
    }
}

/* ------------------------------------------------------------------ */
/* Moderation card (slice 4)                                           */
/* ------------------------------------------------------------------ */

static void mod_close(void)
{
    lv_obj_set_hidden(s_mod_overlay, true);
}

static void mod_close_cb(lv_event_t *e)
{
    (void)e;
    mod_close();
}

static void mod_action_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);   /* keep the click from reaching the overlay */
    const char *action = (const char *)lv_event_get_user_data(e);
    esp_err_t err = kast_relay_chat_mod(action, s_mod_id, s_mod_user);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "chat.mod %s id=%s user=%s", action, s_mod_id, s_mod_user);
        lv_label_set_text_fmt(s_counter, "%s sent...", action);
        s_hint_until = lv_tick_get() + 3000;
    } else {
        ESP_LOGW(TAG, "chat.mod %s failed (%s)", action, esp_err_to_name(err));
        footer_hint_fail();
    }
    mod_close();
}

static void build_mod_popup(lv_obj_t *parent)
{
    /* dim full-page overlay; clicking outside the card closes it */
    s_mod_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_mod_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_mod_overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_mod_overlay, KAST_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_mod_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_mod_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_mod_overlay, 0, 0);
    lv_obj_add_event_cb(s_mod_overlay, mod_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_mod_overlay);
    lv_obj_set_size(card, 440, 170);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(card, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_scrollable(card, false);

    s_mod_title = lv_label_create(card);
    lv_label_set_text(s_mod_title, "MODERATE");
    lv_obj_set_style_text_font(s_mod_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_mod_title, KAST_COLOR_GREEN, 0);
    lv_obj_align(s_mod_title, LV_ALIGN_TOP_LEFT, 4, 0);

    s_mod_preview = lv_label_create(card);
    lv_obj_set_width(s_mod_preview, 408);
    lv_label_set_long_mode(s_mod_preview, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_mod_preview, "");
    lv_obj_set_style_text_font(s_mod_preview, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mod_preview, KAST_COLOR_TEXT, 0);
    lv_obj_align(s_mod_preview, LV_ALIGN_TOP_LEFT, 4, 24);

    static const char *actions[3] = { "delete", "timeout", "ban" };
    static const char *words[3]   = { "DELETE", "TIMEOUT", "BAN" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_set_size(btn, 128, 44);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 4 + i * 140, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, KAST_COLOR_GREEN_DIM, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, mod_action_cb, LV_EVENT_CLICKED, (void *)actions[i]);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, words[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, KAST_COLOR_TEXT, 0);
        lv_obj_set_style_text_color(label, KAST_COLOR_BG, LV_STATE_PRESSED);
        lv_obj_center(label);
    }

    lv_obj_set_hidden(s_mod_overlay, true);
}

static void row_longpress_cb(lv_event_t *e)
{
    const row_meta_t *meta = (const row_meta_t *)lv_event_get_user_data(e);
    if (s_mod_overlay != NULL && !lv_obj_is_hidden(s_mod_overlay)) {
        return;
    }
    if (meta->id[0] == '\0') {
        /* pre-protocol frame: no platform id to act on */
        lv_label_set_text(s_counter, "no id on this line — can't moderate");
        s_hint_until = lv_tick_get() + 3000;
        return;
    }
    snprintf(s_mod_id, sizeof(s_mod_id), "%s", meta->id);
    snprintf(s_mod_user, sizeof(s_mod_user), "%s", meta->user);
    lv_label_set_text_fmt(s_mod_title, "MODERATE @%s", meta->user);
    lv_label_set_text_fmt(s_mod_preview, "%s: %s", meta->user, meta->text);
    lv_obj_set_hidden(s_mod_overlay, false);
}

static void build_composer(lv_obj_t *parent)
{
    /* strip above the footer — tap the field to arm */
    s_strip = lv_obj_create(parent);
    lv_obj_set_size(s_strip, KAST_H_RES, 44);
    lv_obj_align(s_strip, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_style_bg_opa(s_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_strip, 0, 0);
    lv_obj_set_style_pad_all(s_strip, 0, 0);
    lv_obj_set_scrollable(s_strip, false);

    s_ta = lv_textarea_create(s_strip);
    lv_obj_set_size(s_ta, 296, 38);
    lv_obj_align(s_ta, LV_ALIGN_LEFT_MID, 8, 0);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, KAST_INPUT_MAX);
    lv_textarea_set_placeholder_text(s_ta, "tap to reply...");
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_ta, KAST_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_ta, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(s_ta, KAST_COLOR_TEXT_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(s_ta, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(s_ta, 2, 0);
    lv_obj_set_style_radius(s_ta, 8, 0);
    lv_obj_set_style_border_color(s_ta, KAST_COLOR_GREEN, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_ta, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(s_ta, KAST_COLOR_GREEN, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(s_ta, KAST_COLOR_BG, LV_PART_CURSOR);
    lv_obj_set_style_border_width(s_ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_pad_all(s_ta, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_ta, composer_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta, composer_ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    s_send_btn = lv_button_create(s_strip);
    lv_obj_set_size(s_send_btn, 78, 38);
    lv_obj_align(s_send_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(s_send_btn, 8, 0);
    lv_obj_set_style_bg_color(s_send_btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(s_send_btn, 0, 0);
    lv_obj_set_style_bg_color(s_send_btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_send_btn, composer_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_send_btn, true);

    lv_obj_t *send_label = lv_label_create(s_send_btn);
    lv_label_set_text(send_label, "SEND");
    lv_obj_set_style_text_font(send_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(send_label, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(send_label, KAST_COLOR_BG, LV_STATE_PRESSED);
    lv_obj_center(send_label);

    /* edit-mode stand-in for SEND: same geometry, saves the slot to NVS */
    s_macro_save_btn = lv_button_create(s_strip);
    lv_obj_set_size(s_macro_save_btn, 78, 38);
    lv_obj_align(s_macro_save_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(s_macro_save_btn, 8, 0);
    lv_obj_set_style_bg_color(s_macro_save_btn, KAST_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(s_macro_save_btn, 0, 0);
    lv_obj_set_style_bg_color(s_macro_save_btn, KAST_COLOR_GREEN_DIM, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_macro_save_btn, macro_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_macro_save_btn, true);

    s_macro_save_label = lv_label_create(s_macro_save_btn);
    lv_label_set_text(s_macro_save_label, "SET M#");
    lv_obj_set_style_text_font(s_macro_save_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_macro_save_label, KAST_COLOR_BG, 0);
    lv_obj_center(s_macro_save_label);

    s_close_btn = lv_button_create(s_strip);
    lv_obj_set_size(s_close_btn, 38, 38);
    lv_obj_align(s_close_btn, LV_ALIGN_RIGHT_MID, -92, 0);
    lv_obj_set_style_radius(s_close_btn, 8, 0);
    lv_obj_set_style_bg_color(s_close_btn, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(s_close_btn, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(s_close_btn, 2, 0);
    lv_obj_set_style_bg_color(s_close_btn, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_close_btn, composer_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_close_btn, true);

    lv_obj_t *close_label = lv_label_create(s_close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(close_label, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(close_label, KAST_COLOR_BG, LV_STATE_PRESSED);
    lv_obj_center(close_label);

    /* Stock QWERTY keyboard, panel-sized. Hidden until the composer arms;
     * OK fires READY (send), tapping a chat row defocuses the field and
     * disarms. Overlays the list bottom — messages keep flowing behind. */
    s_kb = lv_keyboard_create(parent);
    lv_obj_set_size(s_kb, KAST_H_RES, 200);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_event_cb(s_kb, composer_kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_set_hidden(s_kb, true);
}

void kast_chat_load(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, KAST_COLOR_BG, 0);
    s_used = 0;
    s_seen = 0;

    /* message column — vertical scroll, newest pinned to the bottom.
     * 356 tall: composer strip (44) + footer (36) take the rest. */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, KAST_H_RES, KAST_LIST_H_DISARMED);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 5, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < KAST_CHAT_ROWS; i++) {
        /* a row is a transparent single-line flex container: text label
         * plus two fixed emote img slots (slice 5). Height MUST be set
         * explicitly: this LVGL build's plain-obj class default is
         * LV_DPI_DEF (130 px), not LV_SIZE_CONTENT, so an unset height
         * pins every row to 130 px and only ~2 fit on screen (bench
         * 2026-10-27). The theme's card style also pads rows/columns, so
         * zero those for exact label+emote width math. The label wraps
         * text on its own; its width is shrunk per visible emote in
         * chat_append. */
        s_rows[i] = lv_obj_create(s_list);
        lv_obj_set_size(s_rows[i], KAST_H_RES - 12, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(s_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_rows[i], 0, 0);
        lv_obj_set_style_pad_all(s_rows[i], 0, 0);
        lv_obj_set_style_pad_row(s_rows[i], 0, 0);
        lv_obj_set_style_pad_column(s_rows[i], 4, 0);
        lv_obj_set_flex_flow(s_rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_rows[i], LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollable(s_rows[i], false);

        s_row_label[i] = lv_label_create(s_rows[i]);
        lv_obj_set_width(s_row_label[i], KAST_H_RES - 12);
        lv_label_set_recolor(s_row_label[i], true);
        lv_obj_set_style_text_font(s_row_label[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s_row_label[i], KAST_COLOR_TEXT, 0);

        for (int k = 0; k < 2; k++) {
            s_row_img[i][k] = lv_image_create(s_rows[i]);
            lv_obj_set_size(s_row_img[i][k], KAST_EMOTE_W, KAST_EMOTE_H);
            lv_obj_set_style_pad_right(s_row_img[i][k], 4, 0);
            lv_obj_set_hidden(s_row_img[i][k], true);
        }

        lv_obj_add_event_cb(s_rows[i], row_longpress_cb, LV_EVENT_LONG_PRESSED,
                            &s_row_meta[i]);
    }

    /* placeholder until the first frame lands (flex row 0; hidden flag
     * drops it from the layout once messages arrive) */
    s_empty = lv_label_create(s_list);
    lv_label_set_text(s_empty, "waiting for chat frames from the relay...");
    lv_obj_set_style_text_font(s_empty, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_empty, KAST_COLOR_TEXT_DIM, 0);

    /* footer strip: page word left, live counter right */
    lv_obj_t *foot = lv_obj_create(parent);
    lv_obj_set_size(foot, KAST_H_RES, 36);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_radius(foot, 0, 0);
    lv_obj_set_scrollable(foot, false);

    lv_obj_t *word = lv_label_create(foot);
    lv_label_set_text(word, "LIVE CHAT");
    lv_obj_set_style_text_font(word, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(word, KAST_COLOR_GREEN, 0);
    lv_obj_align(word, LV_ALIGN_LEFT_MID, 12, 0);

    s_counter = lv_label_create(foot);
    lv_label_set_text(s_counter, "0 messages");
    lv_obj_set_style_text_font(s_counter, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_counter, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_counter, LV_ALIGN_RIGHT_MID, -12, 0);

    build_composer(parent);
    build_macro_row(parent);
    build_mod_popup(parent);
    kast_emote_init();   /* idempotent; preloads the SD emote cache */

    /* macro slots from NVS; never-saved reads are fine, slot stays empty */
    for (int i = 0; i < KAST_MACRO_COUNT; i++) {
        s_macro_text[i][0] = '\0';
        if (kast_config_load_macro((uint8_t)i, s_macro_text[i],
                                   KAST_MACRO_TEXT_LEN) != ESP_OK) {
            s_macro_text[i][0] = '\0';
        }
        macro_refresh_chip((uint8_t)i);
    }

    /* anything the ring already holds renders now; the poll catches the rest */
    drain_batch();
    s_poll = lv_timer_create(chat_poll_cb, KAST_POLL_MS, NULL);
}

void kast_chat_unload(void)
{
    lv_timer_delete(s_poll);
    s_poll = NULL;
    s_list = NULL;
    s_empty = NULL;
    s_counter = NULL;
    s_strip = NULL;
    s_ta = NULL;
    s_kb = NULL;
    s_send_btn = NULL;
    s_close_btn = NULL;
    s_armed = false;
    s_hint_until = 0;
    s_used = 0;
    s_macro_row = NULL;
    s_macro_save_btn = NULL;
    s_macro_save_label = NULL;
    s_macro_edit = -1;
    for (int i = 0; i < KAST_MACRO_COUNT; i++) {
        s_macro_btn[i] = NULL;
        s_macro_label[i] = NULL;
        s_macro_text[i][0] = '\0';
    }
    s_mod_overlay = NULL;
    s_mod_title = NULL;
    s_mod_preview = NULL;
    s_mod_id[0] = '\0';
    s_mod_user[0] = '\0';
    for (int i = 0; i < KAST_CHAT_ROWS; i++) {
        s_rows[i] = NULL;
        s_row_label[i] = NULL;
        s_row_img[i][0] = NULL;
        s_row_img[i][1] = NULL;
        s_row_meta[i].id[0] = '\0';
        s_row_meta[i].user[0] = '\0';
        s_row_meta[i].text[0] = '\0';
    }
}
