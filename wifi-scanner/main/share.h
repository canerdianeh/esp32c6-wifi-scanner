/*
    Share log files over Wi-Fi: the scanner becomes an access point with a web
    page listing the microSD files for download. (AirDrop is Apple-only and
    Bluetooth file transfer needs Bluetooth Classic, which the C6 lacks.)
    Wi-Fi scanning must be paused first.
*/
#pragma once

#include <stdbool.h>

typedef struct {
    char ssid[24];
    char password[12];
    char url[32];
    int clients;            /* devices connected to the access point */
    int downloads;          /* files sent so far */
} share_status_t;

/* Start the access point and web server. `highlight` is shown first on the page (may be NULL). */
bool share_start(const char *highlight);
void share_stop(void);
void share_get_status(share_status_t *st);
