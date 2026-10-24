/*
 * touch.c — KAST GT911 bring-up.
 *
 * Reference: vendor touch.h (docs\schematics\...\1_1_86switch_onoff):
 *   SDA=19, SCL=45, INT=-1, RST=-1, addr 0x5D.
 * No INT/RST means the driver must skip the reset sequence and probe the
 * configured address directly.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"

#include "pins.h"
#include "touch.h"

static const char *TAG = "touch";

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_gt911_raw; /* bypass path, vendor-identical */

/* Raw GT911 transactions — byte-for-byte what the vendor demo does over
 * Arduino Wire: [reg_hi, reg_lo] then read/write. No esp_lcd_panel_io. */
static esp_err_t gt911_raw_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_gt911_raw, addr, 2, buf, len, 100);
}

/* Chip-reported resolution, logged at boot for the bench record. */
static uint16_t s_chip_xres = 0, s_chip_yres = 0;

esp_err_t kast_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KAST_TOUCH_SDA,
        .scl_io_num = KAST_TOUCH_SCL,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG,
                        "I2C master bus init failed");

    const esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = 480,
        .y_max = 480,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            /* Empirical calibration 2026-10-24 (owner corner taps, on-screen
             * readout): raw GT911 coordinates are ALREADY in LVGL space on
             * this glass — top-left tap reads ~24,74 and bottom-right ~456,
             * 406. The vendor library's double inversion (TOUCH_MAP 480->0)
             * does NOT apply at rotation 0. Any mirroring makes presses land
             * diagonally opposite. */
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg,
                                                 &tp_io),
                        TAG, "touch panel IO create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, out_touch),
                        TAG, "GT911 driver init failed");

    ESP_LOGI(TAG, "GT911 touch init OK (addr 0x%02X, no INT/RST)",
             KAST_TOUCH_ADDR);

    /* What resolution does the chip's stored config declare? */
    uint8_t res[4] = {0};
    if (esp_lcd_panel_io_rx_param(tp_io, 0x8048, res, 4) == ESP_OK) {
        s_chip_xres = (uint16_t)(res[0] | (res[1] << 8));
        s_chip_yres = (uint16_t)(res[2] | (res[3] << 8));
        ESP_LOGI(TAG, "chip config: x_res=%d y_res=%d", s_chip_xres, s_chip_yres);
    } else {
        ESP_LOGW(TAG, "chip config resolution read failed");
    }

    /* Raw bypass device, same bus — vendor-identical transactions */
    const i2c_device_config_t raw_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KAST_TOUCH_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &raw_cfg,
                                                  &s_gt911_raw),
                        TAG, "raw bypass device add failed");
    uint8_t id[3] = {0};
    if (gt911_raw_read(0x8140, id, 3) == ESP_OK) {
        ESP_LOGI(TAG, "raw probe OK: %c%c%c", id[0], id[1], id[2]);
    } else {
        ESP_LOGW(TAG, "raw probe failed");
    }

    /* Boot-time register dump: 0x8140..0x815F — the whole touch-report
     * region, so a post-boot capture shows chip state even before taps. */
    uint8_t regdump[32] = {0};
    if (gt911_raw_read(0x8140, regdump, sizeof(regdump)) == ESP_OK) {
        ESP_LOGI(TAG, "reg 8140: %02x %02x %02x %02x %02x %02x %02x %02x",
                 regdump[0], regdump[1], regdump[2], regdump[3],
                 regdump[4], regdump[5], regdump[6], regdump[7]);
        ESP_LOGI(TAG, "reg 8148: %02x %02x %02x %02x %02x %02x %02x %02x",
                 regdump[8], regdump[9], regdump[10], regdump[11],
                 regdump[12], regdump[13], regdump[14], regdump[15]);
        ESP_LOGI(TAG, "reg 8150: %02x %02x %02x %02x %02x %02x %02x %02x",
                 regdump[16], regdump[17], regdump[18], regdump[19],
                 regdump[20], regdump[21], regdump[22], regdump[23]);
        ESP_LOGI(TAG, "reg 8158: %02x %02x %02x %02x %02x %02x %02x %02x",
                 regdump[24], regdump[25], regdump[26], regdump[27],
                 regdump[28], regdump[29], regdump[30], regdump[31]);
    }
    return ESP_OK;
}
