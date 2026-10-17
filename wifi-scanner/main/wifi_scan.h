/*
    Interface from the UI to the Wi-Fi scanner in scan.c.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"
#include "ap_rows.h"

/* Hold the lock while reading the rows or the selection. */
void wifi_ui_lock(void);
void wifi_ui_unlock(void);
const ap_rows_t *wifi_ui_rows(void);
const char *wifi_ui_selected(void);     /* SSID the LED is locked to; "" = automatic */
uint32_t wifi_ui_scan_count(void);

/* Latest scan records, strongest first (hold the lock while using them). Returns the count. */
int wifi_ui_records(const wifi_ap_record_t **recs);

/* Lock the LED to the next listed SSID (after the last, back to automatic). */
void wifi_ui_select_next(void);
/* Lock the LED to an SSID ("" = automatic). Don't hold the lock when calling. */
void wifi_ui_select(const char *ssid);

/* Short name for a Wi-Fi security mode, e.g. "WPA2/WPA3". */
const char *wifi_auth_name(wifi_auth_mode_t mode);

/* Drive the LED directly while Wi-Fi scanning is paused (e.g. BLE mode following a device). */
void led_show_rssi(float rssi);
void led_show_off(void);
/* Back to automatic, and re-sort the list. */
void wifi_ui_reset(void);

/* Pause scanning (the LED goes dark) so another mode can use the radio; waits for any scan
 * in progress to end. With stop_wifi, Wi-Fi is also stopped (needed for 802.15.4). */
void wifi_scan_pause(bool stop_wifi);
void wifi_scan_resume(void);
