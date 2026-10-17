/*
    Wi-Fi mode.

    List: networks, 7 per page, with overlaid RSSI bars and a dot per BSSID.
          Short press = next page. Double press = select (starting with the
          first network on the page).
    Details: one network; the LED follows it. Short press = next network.
          Double press = back to the list (the LED stays on that network).
*/
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "gfx.h"
#include "ui.h"
#include "distance.h"
#include "oui_db.h"
#include "wifi_loc.h"
#if CONFIG_SCANNER_FTM
#include "ftm.h"
#endif
#include "wifi_scan.h"

#if CONFIG_SCANNER_DISPLAY_RSSI_MAX <= CONFIG_SCANNER_DISPLAY_RSSI_MIN
#error "Display: the full-bar dBm must be greater than the empty-bar dBm"
#endif

#define ROW_H       34
#define DOTS_H      18
#define PER_PAGE    ((UI_BODY_H - DOTS_H) / ROW_H)
#define DBM_CHARS   4                                   /* room for "-100" */
#define BAR_X       MARGIN
#define BAR_W       (LCD_W - 2 * MARGIN - DBM_CHARS * GFX_FONT_W - 4)
#define BAR_H       11
#define BAR_Y       (GFX_FONT_H + 2)
#define SSID_CHARS  ((LCD_W - 2 * MARGIN - 5 * GFX_FONT_W) / GFX_FONT_W)  /* leave room for " ch11" */
#define DOT_PITCH   7       /* 5 px dot + 2 px gap */
#define DOT_GAP     4       /* space between the name and the first dot */
#define DOT_Y       7       /* centered on the lowercase letters */
#define LINE_H      18

static bool s_detail;
static int s_page;
static char s_sel[33];

static float bar_fraction(int rssi)
{
    float t = (rssi - CONFIG_SCANNER_DISPLAY_RSSI_MIN) /
              (float)(CONFIG_SCANNER_DISPLAY_RSSI_MAX - CONFIG_SCANNER_DISPLAY_RSSI_MIN);
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

static int num_pages(int n)
{
    int p = (n + PER_PAGE - 1) / PER_PAGE;
    return p < 1 ? 1 : p;
}

static int row_index(const ap_rows_t *list, const char *ssid)
{
    for (int i = 0; i < list->n; i++) {
        if (strcmp(list->rows[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void draw_page_dots(int y, int pages, int page)
{
    gfx_band_begin(DOTS_H, C_BLACK);
    if (pages > 1) {
        int pitch = 10;
        int x0 = (LCD_W - (pages - 1) * pitch - 5) / 2;
        for (int p = 0; p < pages; p++) {
            gfx_dot(x0 + p * pitch, (DOTS_H - 5) / 2, p == page ? C_WHITE : C_DIM_DOT);
        }
    }
    gfx_band_flush(y);
}

/* Location broadcast by any of the row's BSSIDs (strongest / first-seen first). */
static bool row_location(const ap_row_t *r, loc_info_t *out)
{
    for (int i = 0; i < r->num_bssids; i++) {
        if (wifi_loc_get(r->bssids[i].mac, out) && loc_has_location(out)) {
            return true;
        }
    }
    return false;
}

/* ---- list ---- */

static void draw_row(int y, const ap_row_t *r, bool selected)
{
    gfx_band_begin(ROW_H, selected ? C_SEL_BG : C_BLACK);
    bool missing = r->count == 0;
    loc_info_t loc;
    uint16_t name_color = missing ? C_GRAY
                        : selected ? C_ACCENT
                        : row_location(r, &loc) ? C_BLUE
                        : r->ftm ? C_GREEN : C_WHITE;
    char buf[16];

    /* Name, shortened if needed so a dot per BSSID always fits after it:
     * white = in the latest scan, gray = seen recently but not now. */
    int dots_w = r->num_bssids ? DOT_GAP + r->num_bssids * DOT_PITCH : 0;
    int max_chars = (SSID_CHARS * GFX_FONT_W - dots_w) / GFX_FONT_W;
    int name_len = (int)strlen(r->ssid);
    int name_chars = name_len < max_chars ? name_len : max_chars;
    gfx_text(MARGIN, 1, r->ssid, name_chars, name_color);
    int dot_x = MARGIN + name_chars * GFX_FONT_W + DOT_GAP;
    for (int i = 0; i < r->num_bssids; i++) {
        gfx_dot(dot_x + i * DOT_PITCH, DOT_Y, r->bssids[i].active ? C_WHITE : C_DIM_DOT);
    }

    snprintf(buf, sizeof(buf), "ch%d%s", r->primary, r->multi_channel ? "+" : "");
    gfx_text_right(LCD_W - MARGIN, 1, buf, C_GRAY);

    /* Overlay: longest bar first, each weaker BSSID's bar on top in its own color,
     * with a dark tick marking where each weaker bar ends. */
    int bars = r->count < AP_ROW_BARS ? r->count : AP_ROW_BARS;
    gfx_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, C_BAR_BG);
    for (int i = 0; i < bars; i++) {
        float t = bar_fraction(r->rssi[i]);
        int filled = (int)(t * BAR_W + 0.5f);
        gfx_fill_rect(BAR_X, BAR_Y, filled, BAR_H, gfx_rgb565(color_scale_at(t)));
        if (i > 0 && filled > 0) {
            gfx_fill_rect(BAR_X + filled - 1, BAR_Y, 1, BAR_H, C_BLACK);
        }
    }

    if (missing) {
        gfx_text_right(LCD_W - MARGIN, BAR_Y - 3, "--", C_GRAY);
    } else {
        snprintf(buf, sizeof(buf), "%d", r->rssi[0]);
        gfx_text_right(LCD_W - MARGIN, BAR_Y - 3, buf, C_WHITE);
    }
    gfx_band_flush(y);
}

static void draw_list(const ap_rows_t *list, const char *led_ssid)
{
    int pages = num_pages(list->n);
    if (s_page >= pages) {
        s_page = pages - 1;
    }
    int first = s_page * PER_PAGE;
    int n = list->n - first < PER_PAGE ? list->n - first : PER_PAGE;
    for (int i = 0; i < n; i++) {
        const ap_row_t *r = &list->rows[first + i];
        draw_row(UI_BODY_Y + i * ROW_H, r, led_ssid[0] && strcmp(r->ssid, led_ssid) == 0);
    }
    int dots_y = UI_BODY_Y + UI_BODY_H - DOTS_H;
    gfx_clear_rows(UI_BODY_Y + n * ROW_H, dots_y, C_BLACK);
    draw_page_dots(dots_y, pages, s_page);
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

static const wifi_ap_record_t *find_record(const wifi_ap_record_t *recs, int n, const uint8_t *mac)
{
    for (int i = 0; i < n; i++) {
        if (memcmp(recs[i].bssid, mac, 6) == 0) {
            return &recs[i];
        }
    }
    return NULL;
}

/* Distance text for a BSSID and its color: FTM when there's a recent measurement
 * (green = high accuracy, yellow = moderate), otherwise the RSSI estimate (white).
 * *uncertainty is set for FTM, -1 for RSSI. */
static uint16_t range_for(const wifi_ap_record_t *rec, char *dist, size_t len, float *uncertainty)
{
#if CONFIG_SCANNER_FTM
    ftm_result_t f;
    if (ftm_get(rec->bssid, &f)) {
        if (f.dist_m < 10.0f) {
            snprintf(dist, len, "%.1f m", f.dist_m);
        } else {
            snprintf(dist, len, "%.0f m", f.dist_m);
        }
        *uncertainty = f.uncertainty_m;
        return f.uncertainty_m < FTM_HIGH_ACCURACY_M ? C_GREEN : C_YELLOW;
    }
#endif
    distance_format(dist, len, distance_m(rec->rssi, WIFI_RSSI_1M, WIFI_PATH_N));
    *uncertainty = -1;
    return C_WHITE;
}

/* Ask for ANQP location queries of the shown BSSIDs, and FTM ranging of those that advertise FTM responder. */
static void set_ftm_targets(const ap_row_t *r, const wifi_ap_record_t *recs, int nrecs)
{
    uint8_t macs[3][6], chans[3];
    int nq = 0;
    for (int i = 0; i < r->num_bssids && nq < 3; i++) {
        const wifi_ap_record_t *rec = r->bssids[i].active ? find_record(recs, nrecs, r->bssids[i].mac) : NULL;
        if (rec) {
            memcpy(macs[nq], rec->bssid, 6);
            chans[nq++] = rec->primary;
        }
    }
    wifi_loc_set_query_targets(macs, chans, nq);
#if CONFIG_SCANNER_FTM
    ftm_target_t t[FTM_MAX_TARGETS];
    int n = 0;
    for (int i = 0; i < r->num_bssids && n < FTM_MAX_TARGETS; i++) {
        const wifi_ap_record_t *rec = r->bssids[i].active ? find_record(recs, nrecs, r->bssids[i].mac) : NULL;
        if (rec && rec->ftm_responder) {
            memcpy(t[n].mac, rec->bssid, 6);
            t[n].channel = rec->primary;
            n++;
        }
    }
    ftm_set_targets(t, n);
#endif
}

static void clear_ftm_targets(void)
{
    wifi_loc_set_query_targets(NULL, NULL, 0);
#if CONFIG_SCANNER_FTM
    ftm_set_targets(NULL, 0);
#endif
}

static void draw_detail(const ap_rows_t *list)
{
    int idx = row_index(list, s_sel);
    if (idx < 0) {
        clear_ftm_targets();
        ui_body_message("No longer listed", "press: next network");
        return;
    }
    const ap_row_t *r = &list->rows[idx];
    const wifi_ap_record_t *recs;
    int nrecs = wifi_ui_records(&recs);
    set_ftm_targets(r, recs, nrecs);
    /* The strongest BSSID in the latest scan carries the network's details. */
    const wifi_ap_record_t *best = NULL;
    for (int i = 0; i < nrecs; i++) {
        if (strcmp((const char *)recs[i].ssid, r->ssid) == 0) {
            best = &recs[i];
            break;
        }
    }

    char buf[48], dist[16];
    int y = UI_BODY_Y + 2;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);

    /* Name, and position in the list. */
    gfx_band_begin(LINE_H + 2, C_BLACK);
    snprintf(buf, sizeof(buf), "%d/%d", idx + 1, list->n);
    gfx_text_right(LCD_W - MARGIN, 1, buf, C_GRAY);
    gfx_text(MARGIN, 1, r->ssid, (LCD_W - 2 * MARGIN) / GFX_FONT_W - (int)strlen(buf) - 1, C_ACCENT);
    gfx_band_flush(y);
    y += LINE_H + 2;

    /* Distance (big) and signal. */
    gfx_band_begin(2 * GFX_FONT_H + 4, C_BLACK);
    if (best) {
        float unc;
        uint16_t c = range_for(best, dist, sizeof(dist), &unc);
        gfx_text_scaled(MARGIN, 0, dist, 2, c);
        snprintf(buf, sizeof(buf), "%d dBm", best->rssi);
        gfx_text_right(LCD_W - MARGIN, 0, buf, gfx_rgb565(color_scale_at(bar_fraction(best->rssi))));
        /* How the distance was found. */
        if (unc >= 0) {
            snprintf(buf, sizeof(buf), "FTM +-%.1fm", unc);
            gfx_text_right(LCD_W - MARGIN, GFX_FONT_H + 2, buf, c);
        } else {
            gfx_text_right(LCD_W - MARGIN, GFX_FONT_H + 2, best->ftm_responder ? "ranging..." : "RSSI est.",
                           C_DIM_DOT);
        }
    } else {
        gfx_text_scaled(MARGIN, 0, "--", 2, C_GRAY);
        gfx_text_right(LCD_W - MARGIN, GFX_FONT_H / 2, "not seen", C_GRAY);
    }
    gfx_band_flush(y);
    y += 2 * GFX_FONT_H + 4;

    /* Location from the beacon (Aruba LCI / RFC 3825 position, RFC 4776 civic address). */
    loc_info_t loc;
    if (row_location(r, &loc)) {
        if (loc.has_position) {
            gfx_band_begin(LINE_H, C_BLACK);
            snprintf(buf, sizeof(buf), "%.6f,%.6f", loc.lat, loc.lon);
            gfx_text(MARGIN, 1, buf, 21, C_BLUE);
            gfx_band_flush(y);
            y += LINE_H;
            char extra[40] = "";
            int n = 0;
            if (loc.has_alt) {
                n += snprintf(extra + n, sizeof(extra) - n, "%.0f m ", loc.alt_m);
            }
            if (loc.accuracy_m > 0) {
                n += snprintf(extra + n, sizeof(extra) - n, "+-%.0fm ", loc.accuracy_m);
            }
            if (loc.has_source) {
                n += snprintf(extra + n, sizeof(extra) - n, "%s ", loc.source == LOC_SOURCE_GPS ? "GPS" : "fixed");
            }
            if (loc.via_anqp) {
                snprintf(extra + n, sizeof(extra) - n, "ANQP");
            }
            y = detail_line(y, "Position", extra, C_BLUE);
        }
        if (loc.country[0]) {
            /* Civic address, wrapped over up to two lines. */
            char civic[64];
            loc_civic_format(&loc, civic, sizeof(civic));
            int len = (int)strlen(civic);
            for (int off = 0; off < len && off < 42; off += 21) {
                gfx_band_begin(LINE_H, C_BLACK);
                gfx_text(MARGIN, 1, civic + off, 21, C_BLUE);
                gfx_band_flush(y);
                y += LINE_H;
            }
        }
    }

    if (best) {
        if (best->second != WIFI_SECOND_CHAN_NONE) {
            snprintf(buf, sizeof(buf), "%d (40 MHz)", best->primary);
        } else {
            snprintf(buf, sizeof(buf), "%d", best->primary);
        }
        y = detail_line(y, "Channel", buf, C_WHITE);
        y = detail_line(y, "Security", wifi_auth_name(best->authmode), C_WHITE);
        snprintf(buf, sizeof(buf), "%s%s%s%s%s", best->phy_11b ? "b" : "", best->phy_11g ? "g" : "",
                 best->phy_11n ? "n" : "", best->phy_11ax ? "ax" : "", best->wps ? " WPS" : "");
        y = detail_line(y, "Standard", buf, C_WHITE);
        /* Vendor from the strongest BSSID's MAC prefix; "(v)" = virtual (locally administered) BSSID. */
        bool virt;
        const char *vendor = oui_vendor_mac(best->bssid, &virt);
        snprintf(buf, sizeof(buf), "%s%s", vendor ? vendor : "unknown", virt ? " (v)" : "");
        y = detail_line(y, "Vendor", buf, vendor ? C_WHITE : C_DIM_DOT);
        /* 802.11mc fine timing measurement, as advertised in the beacon. */
        const char *ftm = best->ftm_responder && best->ftm_initiator ? "resp+init"
                        : best->ftm_responder ? "responder" : best->ftm_initiator ? "initiator" : "no";
        y = detail_line(y, "FTM", ftm, best->ftm_responder ? C_GREEN : (best->ftm_initiator ? C_WHITE : C_DIM_DOT));
        if (best->country.cc[0] >= 'A') {
            snprintf(buf, sizeof(buf), "%c%c", best->country.cc[0], best->country.cc[1]);
            y = detail_line(y, "Country", buf, C_WHITE);
        }
    }

    /* BSSIDs, in first-seen order (matching the dots in the list). */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    for (int i = 0; i < r->num_bssids && y + 2 * GFX_FONT_H <= UI_BODY_Y + UI_BODY_H; i++) {
        const ap_bssid_t *b = &r->bssids[i];
        const wifi_ap_record_t *rec = b->active ? find_record(recs, nrecs, b->mac) : NULL;
        gfx_band_begin(2 * GFX_FONT_H, C_BLACK);
        gfx_dot(MARGIN, 6, rec ? C_WHITE : C_DIM_DOT);
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b->mac[0], b->mac[1], b->mac[2], b->mac[3],
                 b->mac[4], b->mac[5]);
        gfx_text(MARGIN + 9, 0, buf, 20, C_GRAY);
        if (rec) {
            float unc;
            uint16_t c = range_for(rec, dist, sizeof(dist), &unc);
            snprintf(buf, sizeof(buf), "ch%d  %d dBm  ", rec->primary, rec->rssi);
            gfx_text(MARGIN + 9, GFX_FONT_H, buf, 20, C_WHITE);
            gfx_text(MARGIN + 9 + (int)strlen(buf) * GFX_FONT_W, GFX_FONT_H, dist, 20 - (int)strlen(buf), c);
        } else {
            unsigned ago = now_s - b->last_seen_s;
            snprintf(buf, sizeof(buf), "seen %u%s ago", ago < 60 ? ago : ago / 60, ago < 60 ? "s" : "m");
            gfx_text(MARGIN + 9, GFX_FONT_H, buf, 20, C_DIM_DOT);
        }
        gfx_band_flush(y);
        y += 2 * GFX_FONT_H;
    }
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

/* ---- screen ---- */

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    wifi_ui_lock();
    const char *sel = wifi_ui_selected();
    if (sel[0]) {
        snprintf(title, tlen, "LED:%s", sel);
        *tcolor = C_ACCENT;
    } else {
#if CONFIG_SCANNER_MODE_TARGET_SSID
        snprintf(title, tlen, "%s", CONFIG_SCANNER_TARGET_SSID);
#else
        snprintf(title, tlen, "LED:strongest");
#endif
    }
    snprintf(right, rlen, "%d AP", wifi_ui_rows()->listed_aps);
    wifi_ui_unlock();
}

static void draw_body(void)
{
    wifi_ui_lock();
    const ap_rows_t *list = wifi_ui_rows();
    if (list->n == 0) {
        bool scanned = wifi_ui_scan_count() > 0;
        wifi_ui_unlock();
        ui_body_message(scanned ? "No networks found" : "Scanning...", NULL);
        return;
    }
    if (s_detail) {
        draw_detail(list);
    } else {
        draw_list(list, wifi_ui_selected());
    }
    wifi_ui_unlock();
}

static void on_short(void)
{
    char next[33] = "";
    wifi_ui_lock();
    const ap_rows_t *list = wifi_ui_rows();
    if (!s_detail) {
        s_page = (s_page + 1) % num_pages(list->n);
    } else if (list->n) {
        int idx = row_index(list, s_sel);
        strlcpy(next, list->rows[(idx + 1) % list->n].ssid, sizeof(next));
    }
    wifi_ui_unlock();
    if (next[0]) {
        strlcpy(s_sel, next, sizeof(s_sel));
        wifi_ui_select(s_sel);
    }
}

static void on_double(void)
{
    wifi_ui_lock();
    const ap_rows_t *list = wifi_ui_rows();
    if (!s_detail) {
        int first = s_page * PER_PAGE;
        if (first < list->n) {
            strlcpy(s_sel, list->rows[first].ssid, sizeof(s_sel));
            s_detail = true;
        }
    } else {
        int idx = row_index(list, s_sel);
        s_page = idx >= 0 ? idx / PER_PAGE : 0;     /* back to the page with the selection */
        s_detail = false;
    }
    wifi_ui_unlock();
    if (s_detail) {
        wifi_ui_select(s_sel);
    } else {
        clear_ftm_targets();
    }
}

#if CONFIG_SCANNER_DEMO
bool ui_wifi_demo_open(int nth)
{
    bool found = false;
    loc_info_t loc;
    wifi_ui_lock();
    const ap_rows_t *list = wifi_ui_rows();
    for (int i = 0; i < list->n && !found; i++) {
        const ap_row_t *r = &list->rows[i];
        if (r->count && (r->ftm || row_location(r, &loc)) && nth-- == 0) {
            strlcpy(s_sel, r->ssid, sizeof(s_sel));
            s_detail = true;
            found = true;
        }
    }
    wifi_ui_unlock();
    if (found) {
        wifi_ui_select(s_sel);
    }
    return found;
}
#endif

static void enter(void)
{
    s_detail = false;
    s_page = 0;
    clear_ftm_targets();
    wifi_scan_resume();
    wifi_ui_reset();
}

static void leave(void)
{
    clear_ftm_targets();    /* stop ranging when another mode opens */
}

const ui_screen_t ui_screen_wifi = {
    .name = "Wi-Fi scan",
    .icon = &icon_wifi_24,
    .in_menu = true,
    .wants_double = true,
    .enter = enter,
    .leave = leave,
    .header = header,
    .draw_body = draw_body,
    .on_short = on_short,
    .on_double = on_double,
};
