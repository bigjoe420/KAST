/*
 * main.c — KAST entry point.
 * Boot proof (build stamp + chip/PSRAM stats), then subsystem bring-up:
 * display -> touch -> LVGL UI. The build stamp makes the stale-build check
 * trivial: the boot log must show today's date.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#include "display.h"
#include "touch.h"
#include "ui.h"
#include "net.h"
#include "sd.h"
#include "relay.h"
#include "build_stamp.h"

static const char *TAG = "kast";

/* Power sequencing: a cold boot from battery browns out if the backlight
 * slams to 100% while the panel, PSRAM, and radio all surge at once (the
 * rail collapses, brownout detector resets, loop forever). Ramp the
 * backlight up slowly, then start Wi-Fi/SD only after the surge has
 * passed. Harmless on USB; mandatory on battery. */
static void kast_power_up_task(void *arg)
{
    (void)arg;
    for (int p = 10; p <= 100; p += 3) {
        kast_backlight_set((uint8_t)p);
        vTaskDelay(pdMS_TO_TICKS(45));
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    const esp_err_t net_err = kast_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init skipped: %s", esp_err_to_name(net_err));
    }

    const esp_err_t relay_err = kast_relay_init();
    if (relay_err != ESP_OK) {
        ESP_LOGW(TAG, "relay link manager skipped: %s", esp_err_to_name(relay_err));
    }

    const esp_err_t sd_err = kast_sd_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount skipped: %s", esp_err_to_name(sd_err));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* NVS first: config reads (Wi-Fi creds, relay endpoint) happen as
     * soon as net/UI come up. Cheap and flashless — safe pre-ramp. */
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "=== KAST boot proof ===");
    /* Freshness stamp: build_stamp.c recompiles on every build (CMake
     * touch step), so this line always reflects the current binary —
     * the app-descriptor time only refreshes when esp_app_desc.c itself
     * happens to recompile, which silently staled the check. */
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "BUILD: %s (v%s, idf %s)", kast_build_stamp(),
             app->version, app->idf_ver);
    ESP_LOGI(TAG, "chip: ESP32-S3 rev %d, %d cores @ %d MHz",
             chip.revision, chip.cores, CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "features: WiFi %s, BT %s, embedded-psram %s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_BT) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no");
    ESP_LOGI(TAG, "PSRAM: %d bytes", (int)esp_psram_get_size());

    /* Display: raw RGB panel + vendor ST7701 init stream (display.c).
     * No disp_on_off call: this panel has no DISP GPIO (the RGB engine
     * starts at panel_init; display-on is the vendor 0x29 command). */
    ESP_ERROR_CHECK(kast_backlight_init());
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(kast_display_init(&panel));

    /* Touch: GT911 (LVGL reads it through the esp_lcd_touch handle). */
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(kast_touch_init(&touch));

    /* UI: LVGL bounce-buffer mode on the RGB panel, then Home screen.
     * Backlight starts dim; kast_power_up_task ramps it and brings the
     * radio/SD up after the cold-start surge has passed. */
    ESP_ERROR_CHECK(kast_ui_init(panel, touch));
    kast_backlight_set(10);
    xTaskCreate(kast_power_up_task, "kast_pwrup", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive");
    }
}
