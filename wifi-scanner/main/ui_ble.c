/*
    Bluetooth LE mode. Wi-Fi scanning pauses while it's open.

    List: devices heard in the last 30 s, strongest first, 7 per page.
          Short press = next page. Double press = select (starting with the
          first device on the page).
    Details: one device; the LED follows its signal (handy for finding it).
          Short press = next device. Double press = back to the list.
*/
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "gfx.h"
#include "ui.h"
#include "ble_scan.h"
#include "distance.h"
#include "oui_db.h"
#include "wifi_scan.h"

#define ROW_H       34
#define DOTS_H      18
#define LINE_H      18
#define PER_PAGE    ((UI_BODY_H - DOTS_H) / ROW_H)
#define MAX_DEVS    64
#define MAX_AGE_MS  30000
#define RSSI_MIN    -95
#define RSSI_MAX    -40
#define BAR_X       MARGIN
#define BAR_W       (LCD_W - 2 * MARGIN - 4 * GFX_FONT_W - 4)
#define BAR_H       11
#define BAR_Y       (GFX_FONT_H + 2)

static ble_dev_t s_devs[MAX_DEVS];
static int s_page;
static bool s_detail;
static uint8_t s_sel[6];

static float bar_fraction(float rssi)
{
    float t = (rssi - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

static float device_distance(const ble_dev_t *d)
{
    float at_1m = d->tx_power != BLE_TX_POWER_NONE ? d->tx_power - BLE_LOSS_1M : BLE_RSSI_1M;
    return distance_m(d->rssi_avg, at_1m, BLE_PATH_N);
}

/* Name if advertised, else maker + end of address, else the address. */
static void device_label(const ble_dev_t *d, char *name, size_t len)
{
    const char *maker = ble_company_name(d->company);
    if (d->name[0]) {
        snprintf(name, len, "%s", d->name);
    } else if (maker) {
        snprintf(name, len, "%s %02X%02X%02X", maker, d->addr[3], d->addr[4], d->addr[5]);
    } else {
        snprintf(name, len, "%02X:%02X:%02X:%02X:%02X", d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
    }
}

static int num_pages(int total)
{
    int p = (total + PER_PAGE - 1) / PER_PAGE;
    return p < 1 ? 1 : p;
}

static int get_devices(void)
{
    int total = ble_scan_get(s_devs, MAX_DEVS, MAX_AGE_MS);
    return total < MAX_DEVS ? total : MAX_DEVS;
}

static int index_of(const uint8_t *addr, int total)
{
    for (int i = 0; i < total; i++) {
        if (memcmp(s_devs[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    ble_dev_t dummy;
    snprintf(title, tlen, s_detail ? "BLE device" : "BLE devices");
    snprintf(right, rlen, "%d dev", ble_scan_get(&dummy, 0, MAX_AGE_MS));
}

/* ---- list ---- */

static void draw_row(int y, const ble_dev_t *d)
{
    gfx_band_begin(ROW_H, C_BLACK);
    char name[32], buf[16];
    const char *maker = ble_company_name(d->company);
    device_label(d, name, sizeof(name));
    int right_chars = 0;
    if (d->name[0] && maker) {
        right_chars = (int)strlen(maker) + 1;
        gfx_text_right(LCD_W - MARGIN, 1, maker, C_GRAY);
    } else if (d->random_addr) {
        right_chars = 4;
        gfx_text_right(LCD_W - MARGIN, 1, "rnd", C_DIM_DOT);     /* random (often rotating) address */
    }
    gfx_text(MARGIN, 1, name, (LCD_W - 2 * MARGIN) / GFX_FONT_W - right_chars, d->name[0] ? C_WHITE : C_GRAY);

    float t = bar_fraction(d->rssi);
    gfx_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, C_BAR_BG);
    gfx_fill_rect(BAR_X, BAR_Y, (int)(t * BAR_W + 0.5f), BAR_H, gfx_rgb565(color_scale_at(t)));
    snprintf(buf, sizeof(buf), "%d", d->rssi);
    gfx_text_right(LCD_W - MARGIN, BAR_Y - 3, buf, C_WHITE);
    gfx_band_flush(y);
}

static void draw_list(int total)
{
    int pages = num_pages(total);
    if (s_page >= pages) {
        s_page = pages - 1;     /* devices aged out: stay on the last page */
    }
    int first = s_page * PER_PAGE;
    int n = total - first < PER_PAGE ? total - first : PER_PAGE;
    for (int i = 0; i < n; i++) {
        draw_row(UI_BODY_Y + i * ROW_H, &s_devs[first + i]);
    }
    int dots_y = UI_BODY_Y + UI_BODY_H - DOTS_H;
    gfx_clear_rows(UI_BODY_Y + n * ROW_H, dots_y, C_BLACK);

    gfx_band_begin(DOTS_H, C_BLACK);
    if (pages > 1) {
        int pitch = 10;
        int x0 = (LCD_W - (pages - 1) * pitch - 5) / 2;
        for (int p = 0; p < pages; p++) {
            gfx_dot(x0 + p * pitch, (DOTS_H - 5) / 2, p == s_page ? C_WHITE : C_DIM_DOT);
        }
    }
    gfx_band_flush(dots_y);
}

/* ---- details ---- */

static int detail_line(int y, const char *label, const char *value, uint16_t color)
{
    gfx_band_begin(LINE_H, C_BLACK);
    gfx_text(MARGIN, 1, label, 9, C_GRAY);
    gfx_text(MARGIN + 9 * GFX_FONT_W, 1, value, 12, color);
    gfx_band_flush(y);
    return y + LINE_H;
}

static void draw_detail(int total)
{
    ble_dev_t d;
    if (!ble_scan_find(s_sel, &d)) {
        led_show_off();
        ui_body_message("Device gone", "press: next device");
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t ago = now - d.last_seen_ms;
    bool fresh = ago < 10000;
    if (fresh) {
        led_show_rssi(d.rssi_avg);
    } else {
        led_show_off();
    }

    char buf[48], dist[16];
    int y = UI_BODY_Y + 2;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);

    /* Name, and position in the list. */
    int idx = index_of(s_sel, total);
    gfx_band_begin(LINE_H + 2, C_BLACK);
    if (idx >= 0) {
        snprintf(buf, sizeof(buf), "%d/%d", idx + 1, total);
    } else {
        buf[0] = 0;
    }
    gfx_text_right(LCD_W - MARGIN, 1, buf, C_GRAY);
    char name[32];
    device_label(&d, name, sizeof(name));
    gfx_text(MARGIN, 1, name, (LCD_W - 2 * MARGIN) / GFX_FONT_W - (int)strlen(buf) - 1, C_ACCENT);
    gfx_band_flush(y);
    y += LINE_H + 2;

    /* Distance (big) and signal. */
    gfx_band_begin(2 * GFX_FONT_H + 4, C_BLACK);
    distance_format(dist, sizeof(dist), device_distance(&d));
    gfx_text_scaled(MARGIN, 0, fresh ? dist : "--", 2, fresh ? C_WHITE : C_GRAY);
    snprintf(buf, sizeof(buf), "%.0f dBm", d.rssi_avg);
    gfx_text_right(LCD_W - MARGIN, GFX_FONT_H / 2, buf, gfx_rgb565(color_scale_at(bar_fraction(d.rssi_avg))));
    gfx_band_flush(y);
    y += 2 * GFX_FONT_H + 4;

    /* Signal bar (smoothed). */
    gfx_band_begin(16, C_BLACK);
    float t = bar_fraction(d.rssi_avg);
    gfx_fill_rect(MARGIN, 2, LCD_W - 2 * MARGIN, 10, C_BAR_BG);
    gfx_fill_rect(MARGIN, 2, (int)(t * (LCD_W - 2 * MARGIN)), 10, gfx_rgb565(color_scale_at(t)));
    gfx_band_flush(y);
    y += 16;

    const char *maker = ble_company_name(d.company);
    if (maker) {
        y = detail_line(y, "Maker", maker, C_WHITE);
    } else if (d.company != 0xFFFF) {
        snprintf(buf, sizeof(buf), "ID 0x%04X", d.company);
        y = detail_line(y, "Maker", buf, C_WHITE);
    }
    gfx_band_begin(LINE_H, C_BLACK);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", d.addr[0], d.addr[1], d.addr[2], d.addr[3],
             d.addr[4], d.addr[5]);
    gfx_text(MARGIN, 1, buf, 21, C_WHITE);
    gfx_band_flush(y);
    y += LINE_H;
    y = detail_line(y, "Address", d.random_addr ? "random" : "public", C_WHITE);
    if (!d.random_addr) {
        /* Public addresses are IEEE-assigned, so the prefix names the vendor. */
        const char *vendor = oui_vendor(d.addr);
        y = detail_line(y, "Vendor", vendor ? vendor : "unknown", vendor ? C_WHITE : C_DIM_DOT);
    }
    if (d.tx_power != BLE_TX_POWER_NONE) {
        snprintf(buf, sizeof(buf), "%d dBm", d.tx_power);
        y = detail_line(y, "TX power", buf, C_WHITE);
    } else {
        y = detail_line(y, "TX power", "not sent", C_DIM_DOT);
    }
    y = detail_line(y, "Connect", d.connectable ? "yes" : "no", C_WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.adverts);
    y = detail_line(y, "Adverts", buf, C_WHITE);
    snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)(ago / 1000));
    y = detail_line(y, "Last seen", buf, fresh ? C_WHITE : C_ORANGE);
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

/* ---- screen ---- */

static void draw_body(void)
{
    int total = get_devices();
    if (s_detail) {
        draw_detail(total);
    } else if (total == 0) {
        ui_body_message("Scanning for BLE...", "Wi-Fi scan paused");
    } else {
        draw_list(total);
    }
}

static void on_short(void)
{
    int total = get_devices();
    if (!s_detail) {
        s_page = (s_page + 1) % num_pages(total);
    } else if (total) {
        int idx = index_of(s_sel, total);
        memcpy(s_sel, s_devs[(idx + 1) % total].addr, 6);
    }
}

static void on_double(void)
{
    int total = get_devices();
    if (!s_detail) {
        int first = s_page * PER_PAGE;
        if (first < total) {
            memcpy(s_sel, s_devs[first].addr, 6);
            s_detail = true;
        }
    } else {
        int idx = index_of(s_sel, total);
        s_page = idx >= 0 ? idx / PER_PAGE : 0;
        s_detail = false;
        led_show_off();
    }
}

static void enter(void)
{
    s_page = 0;
    s_detail = false;
    wifi_scan_pause(false);
    ble_scan_start();
}

static void leave(void)
{
    ble_scan_stop();
    wifi_scan_resume();
}

const ui_screen_t ui_screen_ble = {
    .name = "BLE devices",
    .icon = &icon_bluetooth_24,
    .in_menu = true,
    .wants_double = true,
    .refresh_ms = 500,
    .enter = enter,
    .leave = leave,
    .header = header,
    .draw_body = draw_body,
    .on_short = on_short,
    .on_double = on_double,
};
