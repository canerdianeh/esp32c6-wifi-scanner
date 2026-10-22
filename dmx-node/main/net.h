/*
    Wi-Fi: joins the configured network, or starts a setup access point
    when there is none or it can't be joined.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t net_start(void);

/* Blocks until an interface has an address. Returns a counter that changes
 * every time the address changes, so receivers know to rebuild sockets. */
uint32_t net_wait_up(void);
uint32_t net_generation(void);

/* The station address if connected, else the access point address. */
esp_netif_ip_info_t net_ip_info(void);
bool net_is_station(void);

/* Turns Wi-Fi power saving off. Only allowed once Bluetooth is off. */
void net_set_low_latency(void);
