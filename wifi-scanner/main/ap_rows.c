/*
    The list of networks shown on screen; see ap_rows.h.
*/
#include <string.h>
#include "esp_timer.h"
#include "sdkconfig.h"
#include "ap_rows.h"

#if CONFIG_SCANNER_DISPLAY
#define LIST_MIN_RSSI       CONFIG_SCANNER_DISPLAY_RSSI_MIN
#else
#define LIST_MIN_RSSI       -82
#endif

/* How long a network (row) or BSSID (dot) stays listed after it was last seen. */
#define KEEP_S  CONFIG_SCANNER_LIST_KEEP_S

/* Mark a BSSID as seen in this scan, adding it if it's new. Existing BSSIDs keep their slot. */
static void note_bssid(ap_row_t *row, const uint8_t mac[6], uint32_t now)
{
    for (int i = 0; i < row->num_bssids; i++) {
        if (memcmp(row->bssids[i].mac, mac, 6) == 0) {
            row->bssids[i].active = true;
            row->bssids[i].last_seen_s = now;
            return;
        }
    }
    int slot = row->num_bssids;
    if (slot >= AP_ROW_BSSIDS) {
        /* Full: take over the slot of the BSSID that has been gone longest, if any is gone. */
        slot = -1;
        for (int i = 0; i < row->num_bssids; i++) {
            if (!row->bssids[i].active &&
                (slot < 0 || row->bssids[i].last_seen_s < row->bssids[slot].last_seen_s)) {
                slot = i;
            }
        }
        if (slot < 0) {
            return;
        }
    } else {
        row->num_bssids++;
    }
    memcpy(row->bssids[slot].mac, mac, 6);
    row->bssids[slot].active = true;
    row->bssids[slot].last_seen_s = now;
}

/* Drop BSSIDs gone longer than KEEP_S, keeping the order of the rest. */
static void expire_bssids(ap_row_t *row, uint32_t now)
{
    int kept = 0;
    for (int i = 0; i < row->num_bssids; i++) {
        if (row->bssids[i].active || now - row->bssids[i].last_seen_s <= KEEP_S) {
            row->bssids[kept++] = row->bssids[i];
        }
    }
    row->num_bssids = kept;
}

/* Hidden networks are never listed: rows are kept by SSID, and they have none. */
bool ap_rows_accepts(const wifi_ap_record_t *ap)
{
    return ap->ssid[0] != 0 && ap->rssi >= LIST_MIN_RSSI;
}

const ap_row_t *ap_rows_find(const ap_rows_t *list, const char *ssid)
{
    for (int i = 0; i < list->n; i++) {
        if (strcmp(list->rows[i].ssid, ssid) == 0) {
            return &list->rows[i];
        }
    }
    return NULL;
}

void ap_rows_reset(ap_rows_t *list)
{
    list->n = 0;
    list->listed_aps = 0;
}

void ap_rows_update(ap_rows_t *list, const wifi_ap_record_t *recs, int count)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

    for (int i = 0; i < list->n; i++) {
        ap_row_t *row = &list->rows[i];
        row->count = 0;
        row->multi_channel = false;
        row->ftm = false;
        for (int j = 0; j < row->num_bssids; j++) {
            row->bssids[j].active = false;
        }
    }
    list->listed_aps = 0;

    for (int i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &recs[i];
        if (!ap_rows_accepts(ap)) {
            continue;
        }
        list->listed_aps++;

        ap_row_t *row = (ap_row_t *)ap_rows_find(list, (const char *)ap->ssid);
        if (!row) {
            if (list->n >= AP_ROWS_MAX) {
                continue;   /* no free row; a long press rebuilds the list */
            }
            row = &list->rows[list->n++];
            memset(row, 0, sizeof(*row));
            strlcpy(row->ssid, (const char *)ap->ssid, sizeof(row->ssid));
        }
        if (row->count == 0) {
            row->primary = ap->primary;     /* records are sorted, so this is the strongest */
        } else if (ap->primary != row->primary) {
            row->multi_channel = true;
        }
        if (row->count < AP_ROW_BARS) {
            row->rssi[row->count] = ap->rssi;
        }
        row->count++;
        row->ftm |= ap->ftm_responder;
        row->last_seen_s = now;
        note_bssid(row, ap->bssid, now);
    }

    /* Drop rows gone longer than KEEP_S, keeping the order of the rest. */
    int kept = 0;
    for (int i = 0; i < list->n; i++) {
        ap_row_t *row = &list->rows[i];
        if (row->count == 0 && now - row->last_seen_s > KEEP_S) {
            continue;
        }
        expire_bssids(row, now);
        if (kept != i) {
            list->rows[kept] = *row;
        }
        kept++;
    }
    list->n = kept;
}
