/*
    Phone GPS link over Bluetooth LE, both ways.

    The scanner advertises as "WiFiScan-XXXX" with the Nordic UART Service:
    - TX (notify): the scanner's own position (GPS module, USB or demo) as NMEA
      sentences, like a Bluetooth GNSS receiver, for phone apps that read one.
    - RX (write): a phone can send its position as NMEA instead (lower priority
      than a module or USB).
    Both need a paired, encrypted link: the screen shows a 6-digit passkey to
    type on the phone. Bonds are kept across restarts.

    The link is switched on and off from the GPS screen (saved in NVS, off by
    default): while Bluetooth is advertising or connected, Wi-Fi scans take
    about twice as long (radio coexistence).
*/
#pragma once

#include <stdbool.h>

typedef enum {
    BLE_GPS_OFF,            /* switched off (or Bluetooth not running) */
    BLE_GPS_ADVERTISING,    /* waiting for a phone */
    BLE_GPS_CONNECTED,      /* connected, not yet paired / encrypted */
    BLE_GPS_PAIRED,         /* encrypted link: NMEA accepted */
} ble_gps_state_t;

void ble_gps_init(void);
void ble_gps_set_enabled(bool on);
bool ble_gps_enabled(void);
ble_gps_state_t ble_gps_state(void);
const char *ble_gps_name(void);     /* advertised name */
