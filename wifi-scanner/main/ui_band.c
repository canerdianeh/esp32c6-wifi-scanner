/*
    2.4 GHz band mode: RF energy across 2400-2483.5 MHz, for seeing Bluetooth
    (and Wi-Fi, and everything else) channel usage.

    The C6's Bluetooth controller doesn't report per-channel activity, so this
    uses the 802.15.4 radio's energy detector at its 16 channels (2405-2480 MHz,
    5 MHz apart, ~2 MHz wide each), which spans the whole BLE band. It measures
    all RF energy, not only Bluetooth. Markers show the three BLE advertising
    channels (37/38/39) and Wi-Fi channels 1/6/11. Wi-Fi is stopped meanwhile.

    Top: average energy per point (bar) with a peak-hold tick.
    Bottom: waterfall, newest sweep at the top, ~12 sweeps per second.
*/
#include <stdio.h>
#include "sdkconfig.h"
#include "gfx.h"
#include "ui.h"
#include "wifi_scan.h"
#include "zb_scan.h"

#define MARK_H      12
#define CHART_H     80
#define LABEL_H     18
#define LEGEND_H    20
#define WF_Y0       (UI_BODY_Y + MARK_H + CHART_H + LABEL_H + LEGEND_H)
#define WF_ROWS     (UI_BODY_Y + UI_BODY_H - WF_Y0)
#define ED_MIN      -100
#define ED_MAX      -35
#define F_LO        2400
#define F_HI        2484
#define PLOT_X0     MARGIN
#define PLOT_W      (LCD_W - 2 * MARGIN)
#define MHZ_X(f)    (PLOT_X0 + ((f) - F_LO) * PLOT_W / (F_HI - F_LO))
#define POINT_W     8

_Static_assert(WF_ROWS <= ZB_HIST_ROWS, "waterfall taller than the history buffer");

static const int s_ble_adv[] = { 2402, 2426, 2480 };    /* BLE channels 37, 38, 39 */
static const int s_wifi[] = { 2412, 2437, 2462 };       /* Wi-Fi channels 1, 6, 11 */

static int8_t s_avg[ZB_NUM_CH], s_peak[ZB_NUM_CH];
static int8_t s_hist[WF_ROWS][ZB_NUM_CH];

static float level(int8_t dbm)
{
    float t = (dbm - ED_MIN) / (float)(ED_MAX - ED_MIN);
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    snprintf(title, tlen, "2.4 GHz band");
    int busiest = 0;
    for (int c = 1; c < ZB_NUM_CH; c++) {
        if (s_avg[c] > s_avg[busiest]) {
            busiest = c;
        }
    }
    snprintf(right, rlen, "max %d", ZB_CH_MHZ(busiest));
}

static void draw_markers(int y)
{
    gfx_band_begin(MARK_H, C_BLACK);
    for (int i = 0; i < 3; i++) {
        gfx_fill_rect(MHZ_X(s_wifi[i]) - 1, 2, 3, MARK_H - 2, C_ORANGE);
        gfx_fill_rect(MHZ_X(s_ble_adv[i]) - 1, 2, 3, MARK_H - 2, C_CYAN);
    }
    gfx_band_flush(y);
}

static void draw_chart(int y)
{
    for (int by = 0; by < CHART_H; by += GFX_BAND_MAX_H) {
        int h = CHART_H - by < GFX_BAND_MAX_H ? CHART_H - by : GFX_BAND_MAX_H;
        gfx_band_begin(h, C_BLACK);
        /* Faint guides under the markers. */
        for (int i = 0; i < 3; i++) {
            gfx_fill_rect(MHZ_X(s_wifi[i]), -by, 1, CHART_H, C_DIM);
            gfx_fill_rect(MHZ_X(s_ble_adv[i]), -by, 1, CHART_H, C_DIM);
        }
        for (int c = 0; c < ZB_NUM_CH; c++) {
            int x = MHZ_X(ZB_CH_MHZ(c)) - POINT_W / 2;
            float t = level(s_avg[c]);
            int bar_h = 2 + (int)(t * (CHART_H - 2));
            gfx_fill_rect(x, CHART_H - bar_h - by, POINT_W, bar_h, gfx_rgb565(color_scale_at(t)));
            int peak_y = CHART_H - 2 - (int)(level(s_peak[c]) * (CHART_H - 2));
            gfx_fill_rect(x, peak_y - by, POINT_W, 2, C_WHITE);
        }
        gfx_band_flush(y + by);
    }
}

static void draw_labels(int y)
{
    gfx_band_begin(LABEL_H, C_BLACK);
    gfx_text(PLOT_X0, 1, "2400", 4, C_GRAY);
    gfx_text(MHZ_X(2442) - 2 * GFX_FONT_W, 1, "2442", 4, C_GRAY);
    gfx_text_right(PLOT_X0 + PLOT_W, 1, "2484", C_GRAY);
    gfx_band_flush(y);

    gfx_band_begin(LEGEND_H, C_BLACK);
    gfx_fill_rect(PLOT_X0, 6, 3, 9, C_CYAN);
    gfx_text(PLOT_X0 + 6, 2, "BLE adv", 7, C_GRAY);
    gfx_fill_rect(PLOT_X0 + 76, 6, 3, 9, C_ORANGE);
    gfx_text(PLOT_X0 + 82, 2, "Wi-Fi 1/6/11", 10, C_GRAY);
    gfx_fill_rect(0, LEGEND_H - 1, LCD_W, 1, C_LINE);
    gfx_band_flush(y + LABEL_H);
}

static uint16_t heat(int8_t dbm)
{
    /* Dark at the noise floor, brightening along the color scale with energy. */
    float t = level(dbm);
    rgb_f_t c = color_scale_at(t);
    float k = t < 0.05f ? 0 : 0.2f + 0.8f * t;
    c.r *= k;
    c.g *= k;
    c.b *= k;
    return gfx_rgb565(c);
}

static void draw_waterfall(void)
{
    for (int by = 0; by < WF_ROWS; by += GFX_BAND_MAX_H) {
        int h = WF_ROWS - by < GFX_BAND_MAX_H ? WF_ROWS - by : GFX_BAND_MAX_H;
        gfx_band_begin(h, C_BLACK);
        for (int r = 0; r < h; r++) {
            const int8_t *row = s_hist[by + r];
            for (int c = 0; c < ZB_NUM_CH; c++) {
                gfx_fill_rect(MHZ_X(ZB_CH_MHZ(c)) - POINT_W / 2, r, POINT_W, 1, heat(row[c]));
            }
        }
        gfx_band_flush(WF_Y0 + by);
    }
}

static void draw_body(void)
{
    zb_band_get(s_avg, s_peak, s_hist, WF_ROWS);
    int y = UI_BODY_Y;
    draw_markers(y);
    draw_chart(y + MARK_H);
    draw_labels(y + MARK_H + CHART_H);
    draw_waterfall();
}

static void enter(void)
{
    wifi_scan_pause(false);     /* Wi-Fi stays started but idle; the coexistence arbiter shares the radio */
    zb_scan_start(false);
}

static void leave(void)
{
    zb_scan_stop();
    wifi_scan_resume();
}

const ui_screen_t ui_screen_band = {
    .name = "2.4GHz band",
    .icon = &icon_bluetooth_24,
    .in_menu = true,
    .refresh_ms = 250,
    .enter = enter,
    .leave = leave,
    .header = header,
    .draw_body = draw_body,
};
