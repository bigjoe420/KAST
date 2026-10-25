idf_component_register(SRCS "main.c" "display.c" "touch.c" "ui.c" "net.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram esp_lcd esp_mm esp_wifi esp_event esp_netif nvs_flash
                                esp_driver_ledc esp_driver_gpio esp_driver_i2c driver
                                espressif__esp_lcd_st7701 espressif__esp_lcd_panel_io_additions
                                espressif__esp_lcd_touch_gt911
                                espressif__esp_lvgl_port lvgl__lvgl)
