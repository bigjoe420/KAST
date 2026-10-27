/*
 * emote.c — KAST emote cache (see emote.h).
 *
 * Fixed 16-slot pool; each slot owns a PSRAM-backed lv_img_dsc_t sized for
 * one 24x24 RGB565 tile. States: EMPTY -> WANTED (request dispatched) ->
 * HAVE. The SD mirror (/sdcard/emotes/<code>.565) is best-effort on both
 * directions: boot preload tolerates a missing card, store tolerates a
 * failed write (RAM cache still serves the session).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "emote.h"
#include "relay.h"

static const char *TAG = "emote";
static const char *KAST_EMOTE_DIR = "/sdcard/emotes";

typedef enum {
    EMOTE_EMPTY = 0,
    EMOTE_WANTED,
    EMOTE_HAVE
} emote_state_t;

typedef struct {
    char            code[KAST_EMOTE_CODE_LEN];
    lv_image_dsc_t  dsc;
    uint8_t        *data;   /* KAST_EMOTE_RAW_MAX, PSRAM */
    emote_state_t   state;
} emote_slot_t;

static emote_slot_t    s_pool[KAST_EMOTE_POOL];
static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static bool            s_inited;

/* Relay-normalized codes are [A-Za-z0-9_]; enforce it so a malformed
 * frame can never escape the cache directory. */
static bool code_ok(const char *code)
{
    size_t n = strlen(code);
    if (n == 0 || n >= KAST_EMOTE_CODE_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)code[i]) && code[i] != '_') {
            return false;
        }
    }
    return true;
}

static emote_slot_t *slot_find(const char *code)
{
    for (int i = 0; i < KAST_EMOTE_POOL; i++) {
        if (s_pool[i].state != EMOTE_EMPTY &&
            strcmp(s_pool[i].code, code) == 0) {
            return &s_pool[i];
        }
    }
    return NULL;
}

static emote_slot_t *slot_take(void)
{
    for (int i = 0; i < KAST_EMOTE_POOL; i++) {
        if (s_pool[i].state == EMOTE_EMPTY) {
            return &s_pool[i];
        }
    }
    return NULL;
}

const lv_image_dsc_t *kast_emote_get(const char *code)
{
    if (!s_inited || code == NULL) {
        return NULL;
    }
    const lv_image_dsc_t *dsc = NULL;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    emote_slot_t *slot = slot_find(code);
    if (slot != NULL && slot->state == EMOTE_HAVE) {
        dsc = &slot->dsc;
    }
    xSemaphoreGive(s_mtx);
    return dsc;
}

void kast_emote_request(const char *code)
{
    if (!s_inited || code == NULL || !code_ok(code)) {
        return;
    }
    bool ask = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    emote_slot_t *slot = slot_find(code);
    if (slot == NULL) {
        slot = slot_take();
        if (slot != NULL) {
            snprintf(slot->code, sizeof(slot->code), "%s", code);
            slot->state = EMOTE_WANTED;
            ask = true;
        }
    } else if (slot->state == EMOTE_EMPTY) {
        slot->state = EMOTE_WANTED;
        ask = true;
    }
    xSemaphoreGive(s_mtx);

    if (ask) {
        ESP_LOGI(TAG, "requesting emote %s", code);
        if (kast_relay_emote_request(code) != ESP_OK) {
            /* link down — leave WANTED; the next line carrying this code
             * retries the ask (request() fires again on WANTED? no:
             * WANTED suppresses). Fall back: reset to EMPTY so the next
             * render re-asks. */
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            slot->state = EMOTE_EMPTY;
            xSemaphoreGive(s_mtx);
        }
    }
}

bool kast_emote_store(const char *code, const uint8_t *rgb565, size_t len)
{
    if (!s_inited || code == NULL || rgb565 == NULL ||
        len != KAST_EMOTE_RAW_MAX || !code_ok(code)) {
        return false;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    emote_slot_t *slot = slot_find(code);
    if (slot == NULL) {
        slot = slot_take();
        if (slot == NULL) {
            xSemaphoreGive(s_mtx);
            ESP_LOGW(TAG, "pool full — emote %s dropped", code);
            return false;
        }
        snprintf(slot->code, sizeof(slot->code), "%s", code);
    }
    if (slot->data == NULL) {
        slot->data = heap_caps_malloc(KAST_EMOTE_RAW_MAX, MALLOC_CAP_SPIRAM);
        if (slot->data == NULL) {
            slot->state = EMOTE_EMPTY;
            xSemaphoreGive(s_mtx);
            ESP_LOGE(TAG, "PSRAM alloc failed for emote %s", code);
            return false;
        }
    }
    memcpy(slot->data, rgb565, len);
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->dsc.header.w = KAST_EMOTE_W;
    slot->dsc.header.h = KAST_EMOTE_H;
    slot->dsc.data_size = KAST_EMOTE_RAW_MAX;
    slot->dsc.data = slot->data;
    slot->state = EMOTE_HAVE;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "emote %s stored (%u bytes)", code, (unsigned)len);

    /* SD mirror outside the lock — fatfs serializes, and a failed card
     * must not hold the pool hostage */
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.565", KAST_EMOTE_DIR, code);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(rgb565, 1, len, f);
        fclose(f);
    }
    return true;
}

static void preload_one(const char *fname)
{
    /* fname like "<code>.565" */
    const char *dot = strstr(fname, ".565");
    if (dot == NULL || dot == fname || dot[5] != '\0' ||
        (size_t)(dot - fname) >= KAST_EMOTE_CODE_LEN) {
        return;
    }
    char code[KAST_EMOTE_CODE_LEN];
    size_t n = (size_t)(dot - fname);
    memcpy(code, fname, n);
    code[n] = '\0';
    if (!code_ok(code)) {
        return;
    }

    char path[48];
    snprintf(path, sizeof(path), "%s/%s", KAST_EMOTE_DIR, fname);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    uint8_t raw[KAST_EMOTE_RAW_MAX];
    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (got == sizeof(raw)) {
        kast_emote_store(code, raw, got);
        ESP_LOGI(TAG, "preloaded emote %s from SD", code);
    }
}

esp_err_t kast_emote_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (s_mtx == NULL) {
        return ESP_FAIL;
    }

    /* best-effort: a missing card or full pool must not block the UI */
    mkdir(KAST_EMOTE_DIR, 0775);
    DIR *dir = opendir(KAST_EMOTE_DIR);
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            preload_one(ent->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGW(TAG, "emote dir unavailable (%s) — SD-only cache off",
                 KAST_EMOTE_DIR);
    }

    s_inited = true;
    return ESP_OK;
}
