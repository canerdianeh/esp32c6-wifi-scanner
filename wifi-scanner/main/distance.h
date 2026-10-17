/*
    Rough distance from signal strength (log-distance path loss):

        d = 10 ^ ((RSSI_at_1m - RSSI) / (10 * n))

    Walls, bodies and antenna orientation easily change RSSI by 10 dB, so treat
    the result as "near / a room away / far" rather than a measurement.
*/
#pragma once

#include <math.h>
#include <stdio.h>

/* Wi-Fi access point: ~20 dBm EIRP minus ~40 dB free-space loss at 1 m, 2.4 GHz, less some real-world loss. */
#define WIFI_RSSI_1M        -32.0f
#define WIFI_PATH_N         3.0f        /* indoors */

/* BLE device without advertised TX power: typical phone/beacon (0 dBm) at 1 m. */
#define BLE_RSSI_1M         -59.0f
/* With advertised TX power: RSSI at 1 m ~= TX power - 41 dB. */
#define BLE_LOSS_1M         41.0f
#define BLE_PATH_N          2.5f

static inline float distance_m(float rssi, float rssi_at_1m, float n)
{
    return powf(10.0f, (rssi_at_1m - rssi) / (10.0f * n));
}

static inline void distance_format(char *buf, size_t len, float m)
{
    if (m < 1.0f) {
        snprintf(buf, len, "<1 m");
    } else if (m < 10.0f) {
        snprintf(buf, len, "~%.0f m", m);
    } else if (m < 100.0f) {
        snprintf(buf, len, "~%.0f m", roundf(m / 5.0f) * 5.0f);     /* nearest 5 m */
    } else {
        snprintf(buf, len, ">100 m");
    }
}
