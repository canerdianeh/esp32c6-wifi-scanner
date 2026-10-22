/*
    Bluetooth LE setup: a GATT service with the same JSON settings as the
    web page. UUIDs and usage are in README.md.

    Wi-Fi can't turn power saving off while Bluetooth runs, so Bluetooth
    shuts down after CONFIG_NODE_BLE_SETUP_MINUTES (once nobody is
    connected) unless no Wi-Fi network is configured.
*/
#pragma once

#include "esp_err.h"

esp_err_t ble_config_start(void);
