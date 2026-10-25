/*
 * sd.c — KAST SD card bring-up (SDSPI host + FATFS).
 *
 * Pins (pins.h — the Law): CS=42, SCK=48, MOSI=47, MISO=41 @ 20 MHz.
 * SCK/MOSI are shared with the LCD's 3-wire SPI command channel, but that
 * channel is bit-banged GPIO via the panel-IO additions component — the
 * GPSPI2 peripheral itself is free, so the SD card gets the real SDSPI
 * host driver (sdspi_host.c). Never bit-bang an SD card: the protocol
 * timing and CRC handling belong in the driver.
 *
 * Policy: mount at /sdcard, print card geometry + free space to the boot
 * log, soft-fail so a missing card never blocks boot.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_check.h"

#include "sd.h"
#include "pins.h"

static const char *TAG = "sd";
#define KAST_SD_MOUNT_POINT "/sdcard"

static sdmmc_card_t *s_card;

esp_err_t kast_sd_init(void)
{
    /* GPSPI2 bus shared-pin config. */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = KAST_SD_MOSI,
        .miso_io_num = KAST_SD_MISO,
        .sclk_io_num = KAST_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SDSPI_DEFAULT_HOST, &bus_cfg, SDSPI_DEFAULT_DMA),
                        TAG, "SPI bus init failed");

    /* SDSPI host at 20 MHz. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDSPI_DEFAULT_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = KAST_SD_CS;
    slot_cfg.host_id = host.slot;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdspi_mount(KAST_SD_MOUNT_POINT, &host,
                                                &slot_cfg, &mount_cfg, &s_card),
                        TAG, "card mount failed");

    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(KAST_SD_MOUNT_POINT, &total, &free_bytes);
    ESP_LOGI(TAG, "card mounted at %s: %llu MB total, %llu MB free",
             KAST_SD_MOUNT_POINT,
             (unsigned long long)(total / (1024 * 1024)),
             (unsigned long long)(free_bytes / (1024 * 1024)));
    return ESP_OK;
}
