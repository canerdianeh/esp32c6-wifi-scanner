/* Monochrome icons (4-bit alpha), tinted when drawn. Data in icons.c. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t w, h;
    const uint8_t *alpha;   /* 4 bpp, high nibble = left pixel, rows padded to whole bytes */
} icon_t;

extern const icon_t icon_wifi_24;
extern const icon_t icon_globe_24;
extern const icon_t icon_bluetooth_24;
extern const icon_t icon_zigbee_24;
extern const icon_t icon_folder_24;
extern const icon_t icon_sdcard_24;
extern const icon_t icon_wifi_14;
extern const icon_t icon_globe_14;
extern const icon_t icon_bluetooth_14;
extern const icon_t icon_zigbee_14;
extern const icon_t icon_folder_14;
extern const icon_t icon_sdcard_14;
