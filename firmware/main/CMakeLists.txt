file(GLOB SRCS "*.c")
idf_component_register(SRCS ${SRCS} INCLUDE_DIRS "."
                       REQUIRES esp_driver_usb_serial_jtag bt esp_wifi esp_http_server esp_netif esp_event mbedtls nvs_flash fatfs
                                esp_driver_i2c esp_driver_gpio esp_timer esp_hw_support espressif__esp_lcd_touch
                                lua esp_pm esp_lcd espressif__esp_lvgl_port json mbedtls)
if(SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WATCH_SELFTEST=1)
endif()
if(SNAP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WATCH_SNAP=1)
endif()
