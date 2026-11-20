idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_rmt esp_driver_usb_serial_jtag esp_timer nvs_flash esp_wifi esp_netif esp_event lwip)
