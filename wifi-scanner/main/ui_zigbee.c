/*
    Zigbee / 802.15.4 mode: energy per channel (11-26) as a bar chart, and the
    networks that answered beacon requests. Wi-Fi is stopped while this mode is open.
*/
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "gfx.h"
#include "ui.h"
#include "wifi_scan.h"
#include "zb_scan.h"

#define CHART_H     72
#define LABEL_H     18
#define ED_MIN      -100
#define ED_MAX      -30
#define CH_PITCH    10
#define CH_X0       ((LCD_W - ZB_NUM_CH * CH_PITCH) / 2 + 1)
#define ROW_H       34
#define NUM_ROWS    ((UI_BODY_H - 4 - CHART_H - LABEL_H) / ROW_H)
#define RSSI_MIN    -100
#define RSSI_MAX    -40
#define BAR_W       (LCD_W - 2 * MARGIN - 4 * GFX_FONT_W - 4)
#define BAR_H       11
#define BAR_Y       (GFX_FONT_H + 2)

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    zb_net_t dummy;
    snprintf(title, tlen, "Zigbee 802.15.4");
    snprintf(right, rlen, "%d PAN", zb_scan_networks(&dummy, 0));
}

static void draw_chart(int y, const int8_t *energy, const bool *has_net)
{
    /* Bars are drawn across bands; gfx clips each band. */
    for (int by = 0; by < CHART_H; by += GFX_BAND_MAX_H) {
        int h = CHART_H - by < GFX_BAND_MAX_H ? CHART_H - by : GFX_BAND_MAX_H;
        gfx_band_begin(h, C_BLACK);
        for (int c = 0; c < ZB_NUM_CH; c++) {
            float t = (energy[c] - ED_MIN) / (float)(ED_MAX - ED_MIN);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            int bar_h = 2 + (int)(t * (CHART_H - 2));
            int x = CH_X0 + c * CH_PITCH;
            gfx_fill_rect(x, -by, CH_PITCH - 2, CHART_H, C_BAR_BG);
            gfx_fill_rect(x, CHART_H - bar_h - by, CH_PITCH - 2, bar_h, gfx_rgb565(color_scale_at(t)));
            if (has_net[c]) {
                gfx_fill_rect(x, -by, CH_PITCH - 2, 2, C_WHITE);    /* a network answered here */
            }
        }
        gfx_band_flush(y + by);
    }
    /* Channel numbers under bars 11, 15, 20, 25. */
    gfx_band_begin(LABEL_H, C_BLACK);
    static const int labels[] = { 11, 15, 20, 25 };
    for (int i = 0; i < 4; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", labels[i]);
        int x = CH_X0 + (labels[i] - ZB_FIRST_CH) * CH_PITCH + (CH_PITCH - 2) / 2 - GFX_FONT_W;
        gfx_text(x < 0 ? 0 : x, 1, buf, 2, C_GRAY);
    }
    gfx_band_flush(y + CHART_H);
}

static void draw_net(int y, const zb_net_t *n)
{
    gfx_band_begin(ROW_H, C_BLACK);
    char buf[32];
    const char *kind = n->proto == ZB_PROTO_ZIGBEE ? (n->stack_profile == 2 ? "ZB PRO" : "Zigbee")
                     : n->proto == ZB_PROTO_THREAD ? "Thread" : "15.4";
    if (n->proto == ZB_PROTO_THREAD && n->name[0]) {
        snprintf(buf, sizeof(buf), "%s %s", kind, n->name);
    } else {
        snprintf(buf, sizeof(buf), "%s %04X", kind, n->pan_id);
    }
    gfx_text(MARGIN, 1, buf, 12, C_WHITE);
    snprintf(buf, sizeof(buf), "ch%d", n->channel);
    gfx_text_right(LCD_W - MARGIN, 1, buf, C_GRAY);
    if (n->permit_join) {
        gfx_text_right(LCD_W - MARGIN - 5 * GFX_FONT_W, 1, "J", C_GREEN);   /* permit join open */
    }

    float t = (n->rssi - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    gfx_fill_rect(MARGIN, BAR_Y, BAR_W, BAR_H, C_BAR_BG);
    gfx_fill_rect(MARGIN, BAR_Y, (int)(t * BAR_W + 0.5f), BAR_H, gfx_rgb565(color_scale_at(t)));
    for (int i = 0; i < n->responders && i < 8; i++) {
        gfx_dot(MARGIN + 3 + i * 7, BAR_Y + 3, C_WHITE);     /* one dot per answering device */
    }
    snprintf(buf, sizeof(buf), "%d", n->rssi);
    gfx_text_right(LCD_W - MARGIN, BAR_Y - 3, buf, C_WHITE);
    gfx_band_flush(y);
}

static void draw_body(void)
{
    int8_t energy[ZB_NUM_CH];
    uint16_t frames[ZB_NUM_CH];
    zb_net_t nets[NUM_ROWS];
    bool has_net[ZB_NUM_CH] = { 0 };
    zb_scan_energy(energy, frames);
    int total = zb_scan_networks(nets, NUM_ROWS);
    int n = total < NUM_ROWS ? total : NUM_ROWS;
    for (int i = 0; i < n; i++) {
        int c = nets[i].channel - ZB_FIRST_CH;
        if (c >= 0 && c < ZB_NUM_CH) {
            has_net[c] = true;
        }
    }

    int y = UI_BODY_Y + 4;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    draw_chart(y, energy, has_net);
    y += CHART_H + LABEL_H;
    if (n == 0) {
        gfx_band_begin(2 * GFX_FONT_H + 6, C_BLACK);
        gfx_text(MARGIN, 0, zb_scan_sweeps() ? "No networks answered" : "Scanning ch 11-26...", 21, C_GRAY);
        gfx_text(MARGIN, GFX_FONT_H + 4, "Wi-Fi paused", 21, C_DIM_DOT);
        gfx_band_flush(y);
        y += 2 * GFX_FONT_H + 6;
    }
    for (int i = 0; i < n; i++, y += ROW_H) {
        draw_net(y, &nets[i]);
    }
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

static void enter(void)
{
    wifi_scan_pause(false);     /* Wi-Fi stays started but idle; the coexistence arbiter shares the radio */
    zb_scan_start(true);
}

static void leave(void)
{
    zb_scan_stop();
    wifi_scan_resume();
}

const ui_screen_t ui_screen_zigbee = {
    .name = "Zigbee",
    .icon = &icon_zigbee_24,
    .in_menu = true,
    .refresh_ms = 1000,
    .enter = enter,
    .leave = leave,
    .header = header,
    .draw_body = draw_body,
};
