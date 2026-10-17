/*
    GPS module on a UART (NMEA 0183: GGA, RMC, GSA, GSV, VTG from any talker).
    Detected automatically, including the baud rate.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present;           /* NMEA received in the last few seconds */
    bool valid;             /* position fix */
    uint8_t fix_type;       /* 0/1 none, 2 = 2D, 3 = 3D */
    uint8_t quality;        /* GGA fix quality: 1 GPS, 2 DGPS/SBAS, 4/5 RTK, 6 estimated */
    double lat, lon;        /* degrees, + = N / E */
    float alt_m;
    float speed_kmh;
    float course_deg;
    bool course_valid;
    uint8_t sats_used;
    uint8_t sats_view;
    float hdop;
    float rate_hz;          /* position updates per second */
    bool time_valid;
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    uint32_t baud;
} gps_info_t;

void gps_init(void);

/* Copy the latest data. Returns info->present. */
bool gps_get(gps_info_t *info);

/* Rough horizontal accuracy from HDOP, in meters. */
float gps_accuracy_m(const gps_info_t *info);

/* RGB565 color for the fix: gray none, red 2D/poor, orange, yellow, green good, cyan differential. */
uint16_t gps_fix_color(const gps_info_t *info);
