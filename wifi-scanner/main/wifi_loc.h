/*
    Location and FTM correction elements received in beacons / probe responses
    (format in loc_ie.h, sent by the ftm-responder project), kept per BSSID.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "loc_ie.h"

/* Register the receive callback (after esp_wifi_init). */
void wifi_loc_init(void);

/* ANQP (802.11u) location queries: the BSSIDs to ask (details page), and running the
 * queries (scan task, between scans). */
void wifi_loc_set_query_targets(const uint8_t (*macs)[6], const uint8_t *channels, int n);
void wifi_loc_run_queries(void);

/* The latest element from this BSSID, if heard in the last few minutes. */
bool wifi_loc_get(const uint8_t bssid[6], loc_info_t *out);
