idf_component_register(SRCS "main.c" "display.c" "touch.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram esp_lcd esp_mm esp_driver_ledc esp_driver_gpio esp_driver_i2c driver
                                espressif__esp_lcd_st7701 espressif__esp_lcd_panel_io_additions espressif__esp_lcd_touch_gt911)
