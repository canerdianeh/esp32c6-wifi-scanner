set(srcs "ftm_responder.c" "led_pixels.c")
if(CONFIG_RESPONDER_GPS)
    list(APPEND srcs "gps.c")
endif()

idf_component_register(SRCS ${srcs}
                    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_driver_rmt esp_driver_gpio
                                  esp_driver_uart
                    INCLUDE_DIRS ".")
