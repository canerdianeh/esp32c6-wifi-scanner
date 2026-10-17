/*
    Maps an RSSI reading onto the color scale chosen in menuconfig, and turns
    that color into what is actually sent to the LEDs.
*/
#pragma once

#include <stdint.h>
#include "led_pixels.h"

typedef struct {
    float r, g, b;   /* 0..255, linear (before gamma and brightness) */
} rgb_f_t;

void color_scale_init(void);

/* Color for a signal level, using the configured scale and RSSI range. */
rgb_f_t color_scale_for_rssi(float rssi);

/* Color at position t (0 = weakest, 1 = strongest) along the configured scale. */
rgb_f_t color_scale_at(float t);

/* Color from a 0xRRGGBB value. */
rgb_f_t color_from_hex(uint32_t hex);

/* Apply gamma, brightness and (for RGBW pixels) white extraction. */
led_color_t color_to_led(rgb_f_t c);
