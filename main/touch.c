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

static esp_err_t gt911_raw_write8(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return i2c_master_transmit(s_gt911_raw, buf, 3, 100);
}

/* Diagnostics shared with the heartbeat (definitions live here so both
 * kast_touch_init and the debug task can reach them). */
static uint8_t s_raw_status = 0; /* last raw 0x814E value (bypasses parser) */
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

/* Last few touch-downs, retained for the heartbeat replay — lets a serial
 * capture made any time after a tap sequence still report the coordinates. */
#define KAST_TAP_HISTORY 6
static uint16_t s_tap_x[KAST_TAP_HISTORY], s_tap_y[KAST_TAP_HISTORY];
static uint8_t s_tap_count = 0;
static uint32_t s_polls = 0;    /* read_data calls — proves the task is alive */
static uint32_t s_hits = 0;     /* polls that reported a touch */
static esp_err_t s_last_err = ESP_OK;

uint8_t kast_touch_last_taps(const uint16_t **x, const uint16_t **y)
{
    *x = s_tap_x;
    *y = s_tap_y;
    return s_tap_count;
}

void kast_touch_stats(uint32_t *polls, uint32_t *hits, int *last_err)
{
    *polls = s_polls;
    *hits = s_hits;
    *last_err = (int)s_last_err;
}

void kast_touch_raw_diag(uint8_t *status, uint16_t *xres, uint16_t *yres)
{
    *status = s_raw_status;
    *xres = s_chip_xres;
    *yres = s_chip_yres;
}

static void touch_debug_task(void *arg)
{
    (void)arg;
    bool was_touched = false;

    while (1) {
        s_polls++;
        /* Raw path: status @ 0x814E, point data @ 0x814F, clear @ 0x814E */
        uint8_t st = 0;
        s_last_err = gt911_raw_read(0x814E, &st, 1);
        s_raw_status = (s_last_err == ESP_OK) ? st : 0xEE;
        if (st != 0 && s_last_err == ESP_OK) {
            ESP_LOGI(TAG, "touch status 0x%02X", st);
        }
        bool touched = ((s_last_err == ESP_OK) && (st & 0x80) &&
                        (st & 0x0F) > 0 && (st & 0x0F) <= 5);
        if (touched) {
            s_hits++;
            uint8_t pt[8] = {0};
            if (gt911_raw_read(0x814F, pt, 8) == ESP_OK) {
                uint16_t x = (uint16_t)(pt[1] | (pt[2] << 8));
                uint16_t y = (uint16_t)(pt[3] | (pt[4] << 8));
                if (!was_touched && s_tap_count < KAST_TAP_HISTORY) {
                    s_tap_x[s_tap_count] = x;
                    s_tap_y[s_tap_count] = y;
                    s_tap_count++;
                    ESP_LOGI(TAG, "touch raw: x=%d y=%d", x, y);
                }
            }
        }
        was_touched = touched;
        gt911_raw_write8(0x814E, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t kast_touch_debug_start(esp_lcd_touch_handle_t touch)
{
    (void)touch; /* raw path uses s_gt911_raw directly */
    BaseType_t ok = xTaskCreate(touch_debug_task, "touch_dbg", 4096,
                                NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
