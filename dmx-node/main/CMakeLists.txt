set(srcs "app_main.c" "node_config.c" "universe.c" "net.c" "dmx_port.c"
         "sacn.c" "artnet.c" "pixels.c" "pwm_out.c" "web_config.c")
if(CONFIG_NODE_BLE_SETUP)
    list(APPEND srcs "ble_config.c")
endif()

idf_component_register(SRCS ${srcs}
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer lwip
                                  esp_driver_uart esp_driver_gpio esp_driver_spi esp_driver_ledc
                                  esp_http_server esp_app_format bt
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "web/index.html")
