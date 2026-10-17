/*
    Maps an RSSI reading onto the color scale chosen in menuconfig.
*/
#include <math.h>
#include "sdkconfig.h"
#include "color_scale.h"

#if CONFIG_SCANNER_RSSI_MAX <= CONFIG_SCANNER_RSSI_MIN
#error "Color scale: the strongest-signal dBm must be greater than the weakest-signal dBm"
#endif

/* Gradient stops, evenly spaced from weakest (first) to strongest (last). */
static const uint32_t s_stops[] = {
#if CONFIG_SCANNER_SCALE_RED_GREEN
    0xFF0000, 0xFFFF00, 0x00FF00,
#elif CONFIG_SCANNER_SCALE_THERMAL
    0x0000FF, 0x00FFFF, 0x00FF00, 0xFFFF00, 0xFF0000,
#elif CONFIG_SCANNER_SCALE_SINGLE
    0x000000, CONFIG_SCANNER_SCALE_SINGLE_COLOR,
#elif CONFIG_SCANNER_SCALE_CUSTOM
    CONFIG_SCANNER_SCALE_CUSTOM_WEAK, CONFIG_SCANNER_SCALE_CUSTOM_MID, CONFIG_SCANNER_SCALE_CUSTOM_STRONG,
#endif
};
#define NUM_STOPS (sizeof(s_stops) / sizeof(s_stops[0]))

static uint8_t s_gamma[256];

void color_scale_init(void)
{
    for (int i = 0; i < 256; i++) {
#if CONFIG_SCANNER_GAMMA
        s_gamma[i] = (uint8_t)lroundf(powf(i / 255.0f, 2.2f) * 255.0f);
#else
        s_gamma[i] = (uint8_t)i;
#endif
    }
}

rgb_f_t color_from_hex(uint32_t hex)
{
    return (rgb_f_t) {
        .r = (hex >> 16) & 0xFF,
        .g = (hex >> 8) & 0xFF,
        .b = hex & 0xFF,
    };
}

rgb_f_t color_scale_for_rssi(float rssi)
{
    float t = (rssi - CONFIG_SCANNER_RSSI_MIN) / (float)(CONFIG_SCANNER_RSSI_MAX - CONFIG_SCANNER_RSSI_MIN);
    return color_scale_at(t);
}

rgb_f_t color_scale_at(float t)
{
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    float pos = t * (NUM_STOPS - 1);
    int i = (int)pos;
    if (i >= (int)NUM_STOPS - 1) {
        return color_from_hex(s_stops[NUM_STOPS - 1]);
    }
    float f = pos - i;
    rgb_f_t a = color_from_hex(s_stops[i]);
    rgb_f_t b = color_from_hex(s_stops[i + 1]);
    return (rgb_f_t) {
        .r = a.r + (b.r - a.r) * f,
        .g = a.g + (b.g - a.g) * f,
        .b = a.b + (b.b - a.b) * f,
    };
}

static uint8_t channel_out(float v)
{
    int i = (int)lroundf(fminf(fmaxf(v, 0.0f), 255.0f));
    return (uint8_t)((s_gamma[i] * CONFIG_SCANNER_LED_BRIGHTNESS + 127) / 255);
}

led_color_t color_to_led(rgb_f_t c)
{
    led_color_t out = {
        .r = channel_out(c.r),
        .g = channel_out(c.g),
        .b = channel_out(c.b),
        .w = 0,
    };
#if CONFIG_SCANNER_LED_USE_WHITE
    uint8_t w = out.r < out.g ? out.r : out.g;
    w = w < out.b ? w : out.b;
    out.r -= w;
    out.g -= w;
    out.b -= w;
    out.w = w;
#endif
    return out;
}
