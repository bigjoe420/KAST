/*
 * emote.h — KAST emote cache (Phase 3, chat page).
 *
 * Emote images never decode on-device: the relay sends them as raw
 * RGB565 (24x24) base64 inside {"type":"emote","code","data"} frames —
 * the PC owns any PNG conversion (Kick CDN emotes arrive there; the
 * bench simulator synthesizes them). The device keeps a small fixed RAM
 * pool (K-6: nothing allocates per message) mirrored to
 * /sdcard/emotes/<code>.565 so favorites survive reboots.
 *
 * Lifecycle:
 *   kast_emote_init()        once, after SD mount; preloads the pool
 *   kast_emote_get(code)     LVGL task: const lv_img_dsc_t * or NULL
 *   kast_emote_request(code) LVGL task: marks WANTED + asks the relay
 *   kast_emote_store(...)    relay ws handler: lands a pushed emote
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KAST_EMOTE_W        24
#define KAST_EMOTE_H        24
#define KAST_EMOTE_RAW_MAX  (KAST_EMOTE_W * KAST_EMOTE_H * 2)   /* RGB565 */
#define KAST_EMOTE_B64_MAX  2064   /* raw*4/3 + slack + NUL */
#define KAST_EMOTE_POOL     16
#define KAST_EMOTE_CODE_LEN 16

esp_err_t kast_emote_init(void);

/* Pool lookup. NULL when the code is unknown or still WANTED. */
const lv_image_dsc_t *kast_emote_get(const char *code);

/* Ask the relay for an emote ({"type":"emote.req","code"}). One ask per
 * code per session — repeats are suppressed until a store lands. */
void kast_emote_request(const char *code);

/* Land a pushed emote: pool insert + best-effort SD mirror. rgb565 must
 * be exactly KAST_EMOTE_RAW_MAX bytes. Callable from the ws handler task.
 * Returns false on malformed code/size or a full pool.
 */
bool kast_emote_store(const char *code, const uint8_t *rgb565, size_t len);

#ifdef __cplusplus
}
#endif
