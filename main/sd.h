/*
 * sd.h — KAST SD card interface (SDSPI + FATFS).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the SD card at /sdcard over SDSPI and log capacity + free space.
 * Soft-fail by contract: a missing or unreadable card must never block
 * boot — the caller logs the result and the UI stays up.
 */
esp_err_t kast_sd_init(void);

#ifdef __cplusplus
}
#endif
