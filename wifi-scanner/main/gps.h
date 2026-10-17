/*
    GPS position from NMEA 0183 (GGA, RMC, GSA, GSV, VTG from any talker), from:
    - a GPS module on a UART (detected automatically, including the baud rate),
    - a computer over USB (NMEA lines on the USB serial port, sdlog.c),
    - a paired phone over Bluetooth LE (ble_gps.c), or
    - a simulated receiver in demo builds.
    Priority in that order: a module wins over USB, USB over a phone, and all of
    them over the simulation.
*/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In priority order (lower wins). */
typedef enum { GPS_SRC_NONE, GPS_SRC_MODULE, GPS_SRC_USB, GPS_SRC_PHONE, GPS_SRC_SIM, GPS_SRC_COUNT } gps_source_t;

/* Satellite systems (index into talker IDs GP, GL, GA, GB/BD, GQ, GI). */
enum { GPS_SYS_GPS, GPS_SYS_GLONASS, GPS_SYS_GALILEO, GPS_SYS_BEIDOU, GPS_SYS_QZSS, GPS_SYS_NAVIC,
       GPS_SYS_OTHER, GPS_SYS_COUNT };

typedef struct {
    uint8_t system;         /* GPS_SYS_* */
    uint16_t prn;
    int8_t elev;            /* degrees above the horizon; -1 = unknown */
    int16_t az;             /* degrees from north; -1 = unknown */
    uint8_t snr;            /* dB-Hz; 0 = not tracked */
    bool used;              /* in the position fix (GSA) */
} gps_sat_t;

#define GPS_MAX_SATS    48

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
    gps_source_t source;    /* where the latest data came from */
} gps_info_t;

void gps_init(void);

/* Copy the latest data. Returns info->present. */
bool gps_get(gps_info_t *info);

/* Satellites in view (from GSV), up to `max`. Returns how many were copied. */
int gps_get_sats(gps_sat_t *out, int max);

/* Feed NMEA text (any chunking; lines end in CR/LF). Safe from any task. */
void gps_feed(gps_source_t src, const char *data, size_t len);

/* Called with each valid sentence that is used ("$...*CS", no line ending), from the feeding task. */
void gps_set_listener(void (*cb)(const char *sentence, gps_source_t src));

/* Short label for sources worth naming on screen ("USB", "phone"), or NULL. */
const char *gps_source_label(gps_source_t src);

/* Rough horizontal accuracy from HDOP, in meters. */
float gps_accuracy_m(const gps_info_t *info);

/* RGB565 color for the fix: gray none, red 2D/poor, orange, yellow, green good, cyan differential. */
uint16_t gps_fix_color(const gps_info_t *info);
