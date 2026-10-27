/*
 * settings_screen.c — KAST connection settings page.
 *
 * Edits the two things a user needs to point KAST at their network and
 * PC: Wi-Fi credentials and the relay endpoint (Phase 2 WebSocket
 * client reads these). Everything saves to NVS (config.c) — creds never
 * live in committed code (D-4). Wi-Fi changes re-apply live through
 * kast_net_apply_creds(); the relay endpoint takes effect on reboot
 * until the WS client learns hot-reload.
 *
 * Entry happens through the stock LVGL keyboard, sized for the 480-wide
 * panel. The Phase 3 chat keyboard will be a custom layout; this one
 * only needs to be functional.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"
#include "config.h"
#include "net.h"
#include "relay.h"

/* Bench creds header: local + gitignored (D-4). Builds without it (CI,
 * fresh clones) get empty strings — NVS-entered creds are the only path. */
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define KAST_WIFI_SSID ""
#define KAST_WIFI_PASSWORD ""
#endif

static const char *TAG = "settings";

#define KAST_FIELD_W   300
#define KAST_FIELD_H   36

static lv_obj_t *s_ssid_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_host_ta;
static lv_obj_t *s_port_ta;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_keyboard;

/* Host/port entry layout: digits with the PERIOD on its own dedicated
 * key (row 2, right side) — IP entry is the only job of this board, and
 * hunting the dot behind a mode switch was the #1 settings-page gripe.
 * Backspace sits top-right; the "abc" key is the keyboard's built-in
 * mode-change back to the text board (hostnames, if ever needed). */
static const char *const s_kb_host_map[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", ".", "\n",
    "7", "8", "9", "0", "\n",
    "abc", "\n", NULL,
};

/* Host/port fields always land on the IP board — on focus AND on every
 * tap (a re-tap of an already-focused field fires no FOCUSED event, and
 * a mode key pressed in between used to leave the letters board up). */
static void ta_host_board_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_USER_1);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_keyboard, ta);
    const lv_keyboard_mode_t mode =
        (ta == s_host_ta || ta == s_port_ta) ? LV_KEYBOARD_MODE_USER_1
                                             : LV_KEYBOARD_MODE_TEXT_LOWER;
    lv_keyboard_set_mode(s_keyboard, mode);
}

static void ta_defocus_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_textarea(s_keyboard, NULL);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder,
                            int y, int max_len)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, KAST_FIELD_W, KAST_FIELD_H);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 16, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(ta, KAST_COLOR_CARD, 0);
    lv_obj_set_style_text_color(ta, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(ta, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    /* Focus feedback: bright keyline — there is no visible caret until
     * the user knows which field is live */
    lv_obj_set_style_border_color(ta, KAST_COLOR_GREEN, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 3, LV_STATE_FOCUSED);
    /* Solid block caret (LV_PART_CURSOR): green background, dark letter */
    lv_obj_set_style_bg_color(ta, KAST_COLOR_GREEN, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(ta, KAST_COLOR_BG, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_pad_all(ta, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    return ta;
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(s_ssid_ta);
    const char *pass = lv_textarea_get_text(s_pass_ta);
    const char *host = lv_textarea_get_text(s_host_ta);
    const char *port_text = lv_textarea_get_text(s_port_ta);

    if (ssid[0] == '\0') {
        lv_label_set_text(s_msg_label, "SSID cannot be empty");
        return;
    }
    if (strlen(pass) > 0 && strlen(pass) < 8) {
        lv_label_set_text(s_msg_label, "Password must be 8+ chars (or empty)");
        return;
    }

    esp_err_t err = kast_config_save_wifi(ssid, pass);
    if (err == ESP_OK) {
        err = kast_config_save_relay(host, (uint16_t)atoi(port_text));
    }
    if (err != ESP_OK) {
        lv_label_set_text(s_msg_label, "Save failed — NVS error");
        return;
    }
    ESP_LOGI(TAG, "settings saved (ssid '%s', relay %s:%s)",
             ssid, host[0] ? host : "(unset)", port_text);
    lv_label_set_text(s_msg_label, "Saved — applying");
    kast_net_apply_creds(ssid, pass);
    kast_relay_apply();
}

void kast_settings_load(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "CONNECTION");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, KAST_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Populate from NVS, bench header as fallback — same policy as net.c */
    char ssid[33] = {0};
    char pass[65] = {0};
    char host[64] = {0};
    uint16_t port = 0;
    if (kast_config_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", KAST_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", KAST_WIFI_PASSWORD);
    }
    if (kast_config_load_relay(host, sizeof(host), &port) != ESP_OK) {
        host[0] = '\0';
        port = 0;
    }

    s_ssid_ta = make_field(parent, "Wi-Fi SSID", 56, 32);
    lv_textarea_set_text(s_ssid_ta, ssid);

    s_pass_ta = make_field(parent, "Wi-Fi password", 98, 64);
    lv_textarea_set_text(s_pass_ta, pass);
    lv_textarea_set_password_mode(s_pass_ta, true);

    s_host_ta = make_field(parent, "Relay host (PC IP)", 140, 63);
    lv_textarea_set_text(s_host_ta, host);
    lv_obj_add_event_cb(s_host_ta, ta_host_board_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_host_ta, ta_host_board_cb, LV_EVENT_CLICKED, NULL);

    s_port_ta = make_field(parent, "Relay port", 182, 5);
    lv_obj_add_event_cb(s_port_ta, ta_host_board_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_port_ta, ta_host_board_cb, LV_EVENT_CLICKED, NULL);
    if (port > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)port);
        lv_textarea_set_text(s_port_ta, buf);
    }

    lv_obj_t *save = lv_button_create(parent);
    lv_obj_set_size(save, 140, KAST_FIELD_H);
    lv_obj_align(save, LV_ALIGN_TOP_LEFT, 330, 182);
    lv_obj_set_style_radius(save, 10, 0);
    lv_obj_set_style_bg_color(save, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_color(save, KAST_COLOR_GREEN_DIM, 0);
    lv_obj_set_style_border_width(save, 2, 0);
    lv_obj_set_style_bg_color(save, KAST_COLOR_GREEN, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "SAVE");
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(save_label, KAST_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(save_label, KAST_COLOR_BG, LV_STATE_PRESSED);
    lv_obj_center(save_label);

    s_msg_label = lv_label_create(parent);
    lv_label_set_text(s_msg_label, "Wi-Fi and relay settings — apply on SAVE");
    lv_obj_set_style_text_font(s_msg_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_msg_label, KAST_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 226);

    /* Stock keyboard, panel-sized. Attached to whichever field is focused.
     * USER_1 carries the IP layout (digits + dot, defined above). */
    s_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(s_keyboard, KAST_H_RES, 200);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_keyboard, KAST_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_USER_1, s_kb_host_map, NULL);
}

void kast_settings_unload(void)
{
    /* Fields, message, keyboard — all children of the content container,
     * cleaned by the router. Nothing here outlives the screen. */
    s_keyboard = NULL;
    s_ssid_ta = NULL;
    s_pass_ta = NULL;
    s_host_ta = NULL;
    s_port_ta = NULL;
    s_msg_label = NULL;
}
