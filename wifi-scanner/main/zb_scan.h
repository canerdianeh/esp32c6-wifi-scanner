/*
    IEEE 802.15.4 (Zigbee / Thread) scanner: per-channel energy, and networks
    found by sending beacon requests on channels 11-26.
    Wi-Fi must be stopped while it runs (shared radio).
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZB_FIRST_CH     11
#define ZB_NUM_CH       16
#define ZB_CH_MHZ(i)    (2405 + 5 * (i))    /* center frequency of channel index i (0 = ch 11) */
#define ZB_HIST_ROWS    160                 /* band mode history (sweeps) */

typedef enum { ZB_PROTO_OTHER, ZB_PROTO_ZIGBEE, ZB_PROTO_THREAD } zb_proto_t;

typedef struct {
    uint16_t pan_id;
    uint8_t channel;
    zb_proto_t proto;
    uint8_t stack_profile;      /* Zigbee: 2 = Zigbee PRO */
    bool permit_join;           /* association permitted (beacon superframe spec) */
    bool router_capacity;       /* Zigbee: accepts router joins */
    uint8_t responders;         /* distinct devices that answered */
    int8_t rssi;                /* strongest answer in the last sweep */
    uint8_t ext_pan[8];         /* Zigbee extended PAN ID */
    char name[17];              /* Thread network name */
    uint32_t last_seen_ms;
} zb_net_t;

/* beacons = true: energy + beacon requests (Zigbee mode, ~3 s per sweep).
 * beacons = false: energy only, ~12 sweeps/s (2.4 GHz band mode). */
void zb_scan_start(bool beacons);
void zb_scan_stop(void);

/* Band mode: running average and peak-hold energy (dBm) per channel, and the
 * last `rows` sweeps (newest first) for a waterfall. */
void zb_band_get(int8_t avg[ZB_NUM_CH], int8_t peak[ZB_NUM_CH], int8_t hist[][ZB_NUM_CH], int rows);

/* Latest energy per channel (dBm), index 0 = channel 11. */
void zb_scan_energy(int8_t energy[ZB_NUM_CH], uint16_t frames[ZB_NUM_CH]);

/* Networks heard recently, strongest first. Returns how many (copies up to max). */
int zb_scan_networks(zb_net_t *out, int max);

/* Sweeps completed since start. */
uint32_t zb_scan_sweeps(void);
