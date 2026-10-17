/*
    The list of networks shown on screen (and cycled through with the button).

    Each SSID keeps its row once it has one: the list is sorted strongest first
    only when it is built from empty (at startup, or after ap_rows_reset()).
    New SSIDs fill free rows at the end. A row whose SSID stops showing up is
    kept (marked missing) for CONFIG_SCANNER_LIST_KEEP_S, then removed; rows
    below move up. Each row's BSSIDs are kept the same way, in first-seen order.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"

#define AP_ROWS_MAX     32  /* networks kept in the list (shown 7 per page) */
#define AP_ROW_BARS     8   /* bars overlaid per row; more BSSIDs are still counted */
#define AP_ROW_BSSIDS   10  /* BSSIDs remembered per row (one dot each) */

typedef struct {
    uint8_t mac[6];
    bool active;            /* in the latest scan */
    uint32_t last_seen_s;   /* seconds since boot */
} ap_bssid_t;

typedef struct {
    char ssid[33];
    int count;              /* BSSIDs with this SSID in the latest scan; 0 = missing */
    uint32_t last_seen_s;   /* seconds since boot */
    bool multi_channel;     /* BSSIDs are on more than one channel */
    bool ftm;               /* a BSSID in the latest scan advertises FTM responder */
    uint8_t primary;        /* channel of the strongest BSSID (last seen) */
    int8_t rssi[AP_ROW_BARS];   /* strongest first; rssi[0] is the strongest */
    ap_bssid_t bssids[AP_ROW_BSSIDS];   /* in first-seen order */
    int num_bssids;
} ap_row_t;

typedef struct {
    ap_row_t rows[AP_ROWS_MAX];
    int n;
    int listed_aps;         /* access points in the latest scan that pass the filter */
} ap_rows_t;

/* Whether an access point can be listed (hidden and too-weak ones are not). */
bool ap_rows_accepts(const wifi_ap_record_t *ap);

/* Fold in a new scan. `recs` must be sorted strongest first. */
void ap_rows_update(ap_rows_t *list, const wifi_ap_record_t *recs, int count);

/* Empty the list, so the next update builds a freshly sorted one. */
void ap_rows_reset(ap_rows_t *list);

/* Row for an SSID, or NULL. */
const ap_row_t *ap_rows_find(const ap_rows_t *list, const char *ssid);
