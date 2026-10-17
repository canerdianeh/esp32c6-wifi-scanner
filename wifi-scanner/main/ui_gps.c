/*
    GPS mode: position in large digits, a sky plot of the satellites beside the
    fix summary, then altitude, speed, heading and UTC time.

    Sky plot: the outer ring is the horizon, the inner ring 45 degrees up, the
    center straight overhead, north at the top. Satellites in the fix are
    colored by signal strength; the rest are gray.

    Short press: phone GPS over Bluetooth on / off.
*/
#include <math.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "gfx.h"
#include "ui.h"
#include "gps.h"
#if CONFIG_SCANNER_PHONE_GPS
#include "ble_gps.h"
#endif

#define LINE_H      19
#define SKY_H       92                      /* sky plot block height */
#define SKY_R       42                      /* horizon radius */
#define SKY_CX      (MARGIN + SKY_R + 1)
#define SKY_CY      (SKY_R + 3)
#define COL_X       (MARGIN + 2 * SKY_R + 10)   /* text column beside the plot */
#define COL_CHARS   ((LCD_W - MARGIN - COL_X) / GFX_FONT_W)
#define COL_LINE_H  18

static const char *compass(float deg)
{
    static const char *const pts[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int i = (int)((deg + 22.5f) / 45.0f) & 7;
    return pts[i];
}

/* Label (small, gray) above a value (2x size). */
static int draw_big(int y, const char *label, const char *value, uint16_t color)
{
    gfx_band_begin(GFX_FONT_H, C_BLACK);
    gfx_text(MARGIN, 0, label, 21, C_GRAY);
    gfx_band_flush(y);
    y += GFX_FONT_H;
    gfx_band_begin(2 * GFX_FONT_H + 4, C_BLACK);
    gfx_text_scaled(MARGIN, 0, value, 2, color);
    gfx_band_flush(y);
    return y + 2 * GFX_FONT_H + 4;
}

/* "Label    value" line. */
static int draw_line(int y, const char *label, const char *value, uint16_t color)
{
    gfx_band_begin(LINE_H, C_BLACK);
    gfx_text(MARGIN, 1, label, 8, C_GRAY);
    gfx_text(MARGIN + 8 * GFX_FONT_W, 1, value, 13, color);
    gfx_band_flush(y);
    return y + LINE_H;
}

static uint16_t snr_color(int snr)
{
    return snr >= 40 ? C_GREEN : snr >= 32 ? C_YELLOW : snr >= 25 ? C_ORANGE : C_RED;
}

/* Sky plot and the summary column, drawn band by band (`off` = band top within the block). */
static void draw_sky_band(int off, const gps_sat_t *sats, int nsats, char col[][16], const uint16_t *col_color)
{
    int cy = SKY_CY - off;
    gfx_fill_rect(SKY_CX - SKY_R, cy, 2 * SKY_R + 1, 1, C_BAR_BG);     /* E-W */
    gfx_fill_rect(SKY_CX, cy - SKY_R, 1, 2 * SKY_R + 1, C_BAR_BG);     /* N-S */
    gfx_ring(SKY_CX, cy, SKY_R / 2, C_LINE);
    gfx_ring(SKY_CX, cy, SKY_R, C_DIM_DOT);
    gfx_text(SKY_CX - GFX_FONT_W / 2 + 1, cy - SKY_R + 2, "N", 1, C_DIM_DOT);

    /* Unused satellites first, so the ones in the fix draw on top. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nsats; i++) {
            const gps_sat_t *s = &sats[i];
            if (s->used != pass || s->elev < 0 || s->az < 0) {
                continue;
            }
            float r = SKY_R * (90 - (s->elev > 90 ? 90 : s->elev)) / 90.0f;
            float a = s->az * (float)M_PI / 180.0f;
            int x = SKY_CX + (int)lroundf(r * sinf(a));
            int y = cy - (int)lroundf(r * cosf(a));
            gfx_dot(x - 2, y - 2, s->used ? snr_color(s->snr) : (s->snr ? C_DIM_DOT : C_DIM));
        }
    }

    for (int i = 0; i < 5; i++) {
        if (col[i][0]) {
            gfx_text(COL_X, 2 + i * COL_LINE_H - off, col[i], COL_CHARS, col_color[i]);
        }
    }
}

static void draw_sky(int y, const gps_info_t *g)
{
    static gps_sat_t sats[GPS_MAX_SATS];
    int nsats = gps_get_sats(sats, GPS_MAX_SATS);

    char col[5][16] = { "" };
    uint16_t cc[5] = { C_GRAY, C_WHITE, C_WHITE, C_WHITE, C_WHITE };
    if (gps_source_label(g->source) && !g->sats_view) {
        snprintf(col[0], sizeof(col[0]), "Source");
        snprintf(col[1], sizeof(col[1]), "%s", gps_source_label(g->source));
        cc[1] = C_CYAN;
    } else {
        snprintf(col[0], sizeof(col[0]), "Sats");
        snprintf(col[1], sizeof(col[1]), "%d/%d", g->sats_used, g->sats_view);
    }
    if (g->valid) {
        snprintf(col[2], sizeof(col[2]), "%s ~%.0fm",
                 g->fix_type == 2 ? "2D" : (g->quality == 2 ? "DGPS" : "3D"), gps_accuracy_m(g));
        cc[2] = gps_fix_color(g);
    } else {
        snprintf(col[2], sizeof(col[2]), "no fix");
        cc[2] = C_GRAY;
    }
    snprintf(col[3], sizeof(col[3]), "HDOP%4.1f", g->hdop);
    snprintf(col[4], sizeof(col[4]), "%.1f Hz", g->rate_hz);

    for (int off = 0; off < SKY_H; off += GFX_BAND_MAX_H) {
        int h = SKY_H - off < GFX_BAND_MAX_H ? SKY_H - off : GFX_BAND_MAX_H;
        gfx_band_begin(h, C_BLACK);
        draw_sky_band(off, sats, nsats, col, cc);
        gfx_band_flush(y + off);
    }
}

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    gps_info_t g;
    snprintf(title, tlen, "GPS");
#if CONFIG_SCANNER_PHONE_GPS
    if (ble_gps_enabled()) {
        snprintf(title, tlen, "GPS phone on");
        *tcolor = C_CYAN;
    }
#endif
    if (!gps_get(&g)) {
        snprintf(right, rlen, "no data");
    } else if (!g.valid) {
        snprintf(right, rlen, "no fix");
    } else {
        const char *src = gps_source_label(g.source);
        snprintf(right, rlen, "%s%s%s fix", src ? src : "", src ? " " : "", g.fix_type == 2 ? "2D" : "3D");
    }
}

/* No GPS data: how to get some. */
static void draw_no_data(void)
{
    char buf[40];
    int y = UI_BODY_Y + 8;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    gfx_band_begin(GFX_FONT_H + 8, C_BLACK);
    gfx_text(MARGIN, 0, "No GPS data", 21, C_WHITE);
    gfx_band_flush(y);
    y += GFX_FONT_H + 8;
    gfx_band_begin(2 * GFX_FONT_H + 6, C_BLACK);
    gfx_text(MARGIN, 0, "Module:", 21, C_GRAY);
    snprintf(buf, sizeof(buf), "GPS TX -> GPIO%d", CONFIG_SCANNER_GPS_PIN_RX);
    gfx_text(MARGIN, GFX_FONT_H + 2, buf, 21, C_DIM_DOT);
    gfx_band_flush(y);
    y += 2 * GFX_FONT_H + 6;
#if CONFIG_SCANNER_USB_NMEA
    gfx_band_begin(2 * GFX_FONT_H + 6, C_BLACK);
    gfx_text(MARGIN, 0, "Computer over USB:", 21, C_GRAY);
    gfx_text(MARGIN, GFX_FONT_H + 2, "NMEA on the USB port", 21, C_DIM_DOT);
    gfx_band_flush(y);
    y += 2 * GFX_FONT_H + 6;
#endif
#if CONFIG_SCANNER_PHONE_GPS
    gfx_band_begin(3 * GFX_FONT_H + 6, C_BLACK);
    gfx_text(MARGIN, 0, "Phone over Bluetooth:", 21, C_GRAY);
    gfx_text(MARGIN, 2 * GFX_FONT_H + 4, ble_gps_enabled() ? "press: turn off" : "press: turn on", 21,
             C_DIM_DOT);
    switch (ble_gps_state()) {
    case BLE_GPS_CONNECTED:
        snprintf(buf, sizeof(buf), "connected, pairing");
        break;
    case BLE_GPS_PAIRED:
        snprintf(buf, sizeof(buf), "paired, no location");
        break;
    case BLE_GPS_ADVERTISING:
        snprintf(buf, sizeof(buf), "%s", ble_gps_name());
        break;
    default:
        snprintf(buf, sizeof(buf), "off");
        break;
    }
    gfx_text(MARGIN, GFX_FONT_H + 2, buf, 21, ble_gps_enabled() ? C_CYAN : C_GRAY);
    gfx_band_flush(y);
    y += 3 * GFX_FONT_H + 6;
#endif
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

static void draw_body(void)
{
    gps_info_t g;
    if (!gps_get(&g)) {
        draw_no_data();
        return;
    }

    char buf[40];
    uint16_t vc = g.valid ? C_WHITE : C_DIM_DOT;
    int y = UI_BODY_Y + 4;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);

    if (g.valid) {
        snprintf(buf, sizeof(buf), "%.6f", g.lat);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_big(y, "Latitude", buf, vc);
    if (g.valid) {
        snprintf(buf, sizeof(buf), "%.6f", g.lon);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_big(y, "Longitude", buf, vc);

    draw_sky(y, &g);
    y += SKY_H;

    if (g.valid) {
        snprintf(buf, sizeof(buf), "%.1f m  %.0f ft", g.alt_m, g.alt_m * 3.28084f);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_line(y, "Alt", buf, vc);
    if (g.valid) {
        snprintf(buf, sizeof(buf), "%.1f km/h %.1f mph", g.speed_kmh, g.speed_kmh * 0.621371f);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_line(y, "Speed", buf, vc);
    if (g.valid && g.course_valid) {
        snprintf(buf, sizeof(buf), "%.0f deg %s", g.course_deg, compass(g.course_deg));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_line(y, "Heading", buf, vc);
    if (g.time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d UTC", g.hour, g.minute, g.second);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = draw_line(y, "Time", buf, C_WHITE);
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

#if CONFIG_SCANNER_PHONE_GPS
static void on_short(void)
{
    ble_gps_set_enabled(!ble_gps_enabled());
}
#endif

const ui_screen_t ui_screen_gps = {
    .name = "GPS",
    .icon = &icon_globe_24,
    .in_menu = true,
    .refresh_ms = 500,
    .header = header,
    .draw_body = draw_body,
#if CONFIG_SCANNER_PHONE_GPS
    .on_short = on_short,
#endif
};
