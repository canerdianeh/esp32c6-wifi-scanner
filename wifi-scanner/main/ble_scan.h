/*
    Bluetooth LE scanner (NimBLE, active scanning). BLE only: the ESP32-C6 has
    no Bluetooth Classic, so classic-only devices don't appear.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t addr[6];        /* most significant byte first, as usually printed */
    bool random_addr;
    char name[24];
    uint16_t company;       /* from manufacturer data; 0xFFFF = none */
    int8_t rssi;            /* latest */
    float rssi_avg;         /* smoothed, for distance and the LED */
    int8_t tx_power;        /* advertised TX power (dBm); BLE_TX_POWER_NONE if not advertised */
    bool connectable;
    uint32_t adverts;       /* advertisements / scan responses received */
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
} ble_dev_t;

#define BLE_TX_POWER_NONE 127

/* Latest data for one device. Returns false if it isn't in the table. */
bool ble_scan_find(const uint8_t addr[6], ble_dev_t *out);

void ble_scan_start(void);
void ble_scan_stop(void);

/* Devices seen in the last `max_age_ms`, strongest first; copies up to `max`.
 * Returns how many such devices there are (may be more than `max`). */
int ble_scan_get(ble_dev_t *out, int max, uint32_t max_age_ms);

/* Short maker name for a Bluetooth SIG company ID, or NULL. */
const char *ble_company_name(uint16_t company);
