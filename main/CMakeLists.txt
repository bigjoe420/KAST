idf_component_register(SRCS "main.c" "build_stamp.c" "display.c" "touch.c" "ui.c" "home_screen.c"
                            "simple_screens.c" "settings_screen.c" "net.c" "sd.c" "config.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram esp_lcd esp_mm esp_wifi esp_event esp_netif nvs_flash
                                esp_driver_ledc esp_driver_gpio esp_driver_i2c driver esp_driver_sdspi sdmmc fatfs
                                esp_app_format
                                espressif__esp_lcd_st7701 espressif__esp_lcd_panel_io_additions
                                espressif__esp_lcd_touch_gt911
                                espressif__esp_lvgl_port lvgl__lvgl)

# Commandment VI: make the freshness stamp recompile on EVERY build.
# Without this, __DATE__/__TIME__ only refresh when the file itself changes
# (or when esp_app_desc.c happens to recompile) and the boot log can show
# a stale date on a fresh binary. Touching the source costs one file
# recompile plus a relink per build — cheap insurance.
add_custom_target(kast_build_stamp ALL
                  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_SOURCE_DIR}/build_stamp.c")
add_dependencies(${COMPONENT_LIB} kast_build_stamp)
