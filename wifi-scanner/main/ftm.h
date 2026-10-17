/*
    Wi-Fi FTM (802.11mc fine timing measurement) ranging to access points that
    advertise FTM responder. Connectionless: no need to join the network.

    The UI names the BSSIDs it wants ranged; the scan task runs one session per
    target between Wi-Fi scans (so they never compete for the radio).
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FTM_MAX_TARGETS 3

typedef struct {
    uint8_t mac[6];
    uint8_t channel;
} ftm_target_t;

typedef struct {
    float dist_m;           /* median of recent sessions */
    float uncertainty_m;    /* spread across frames and sessions (at least 1 m) */
    uint8_t sessions;       /* sessions in the median */
} ftm_result_t;

/* Below this uncertainty the result counts as high accuracy (green). */
#define FTM_HIGH_ACCURACY_M 3.0f

void ftm_init(void);

/* Set the BSSIDs to range (n = 0 stops). Safe from any task. */
void ftm_set_targets(const ftm_target_t *targets, int n);

/* Run one session per target. Called by the scan task between scans. */
void ftm_run_pending(void);

/* A recent (last ~8 s) successful result for this BSSID. */
bool ftm_get(const uint8_t mac[6], ftm_result_t *out);
