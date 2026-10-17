/*
    Files mode: the microSD log files. Short press selects the next file,
    double press shares the files over Wi-Fi (see share.c).
*/
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "gfx.h"
#include "ui.h"
#include "sdlog.h"
#include "share.h"
#include "wifi_scan.h"

#define ROW_H       24
#define HINT_H      40
#define NUM_ROWS    ((UI_BODY_H - 4 - HINT_H) / ROW_H)
#define MAX_FILES   64

static sdlog_file_t s_files[MAX_FILES];
static int s_num_files;
static int s_sel;
static char s_share_name[16];

static void refresh_list(void)
{
    char prev[16] = "";
    if (s_sel < s_num_files) {
        strlcpy(prev, s_files[s_sel].name, sizeof(prev));
    }
    s_num_files = sdlog_list_files(s_files, MAX_FILES);
    s_sel = 0;
    for (int i = 0; i < s_num_files; i++) {
        if (strcmp(s_files[i].name, prev) == 0) {
            s_sel = i;
        }
    }
}

static void header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    snprintf(title, tlen, "Files");
    snprintf(right, rlen, "%d", s_num_files);
}

static void draw_body(void)
{
    refresh_list();
    if (s_num_files == 0) {
        ui_body_message(sdlog_is_active() ? "No files" : "No microSD card", NULL);
        return;
    }
    int first = s_sel >= NUM_ROWS ? s_sel - NUM_ROWS + 1 : 0;     /* scroll to keep the selection visible */
    int y = UI_BODY_Y + 4;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    for (int row = 0; row < NUM_ROWS; row++, y += ROW_H) {
        int i = first + row;
        bool sel = i == s_sel;
        gfx_band_begin(ROW_H, sel ? C_SEL_BG : C_BLACK);
        if (i < s_num_files) {
            char size[16];
            long s = s_files[i].size;
            if (s < 10 * 1024) {
                snprintf(size, sizeof(size), "%ld B", s);
            } else if (s < 10 * 1024 * 1024) {
                snprintf(size, sizeof(size), "%ld KB", s / 1024);
            } else {
                snprintf(size, sizeof(size), "%ld MB", s / (1024 * 1024));
            }
            if (s_files[i].current) {
                gfx_dot(MARGIN, (ROW_H - 5) / 2, C_RED);        /* being recorded */
            }
            gfx_text(MARGIN + 8, (ROW_H - GFX_FONT_H) / 2, s_files[i].name, 12, sel ? C_ACCENT : C_WHITE);
            gfx_text_right(LCD_W - MARGIN, (ROW_H - GFX_FONT_H) / 2, size, C_GRAY);
        }
        gfx_band_flush(y);
    }
    int hint_y = UI_BODY_Y + UI_BODY_H - HINT_H;
    gfx_clear_rows(y, hint_y, C_BLACK);
    gfx_band_begin(HINT_H, C_BLACK);
    gfx_text(MARGIN, 2, "press: next file", 21, C_GRAY);
    gfx_text(MARGIN, GFX_FONT_H + 6, "2x:    share Wi-Fi", 21, C_GRAY);
    gfx_band_flush(hint_y);
}

static void on_short(void)
{
    if (s_num_files) {
        s_sel = (s_sel + 1) % s_num_files;
    }
}

static void on_double(void)
{
    if (s_num_files) {
        strlcpy(s_share_name, s_files[s_sel].name, sizeof(s_share_name));
        ui_show(&ui_screen_share);
    }
}

const ui_screen_t ui_screen_files = {
    .name = "Files",
    .icon = &icon_folder_24,
    .in_menu = true,
    .wants_double = true,
    .refresh_ms = 3000,
    .header = header,
    .draw_body = draw_body,
    .on_short = on_short,
    .on_double = on_double,
};

/* ---- Share screen: access point details until a press ---- */

static bool s_share_ok;

static void share_header(char *title, size_t tlen, uint16_t *tcolor, char *right, size_t rlen)
{
    snprintf(title, tlen, "Share over Wi-Fi");
    *tcolor = C_ACCENT;
}

static int share_line(int y, const char *label, const char *value, uint16_t color)
{
    gfx_band_begin(2 * GFX_FONT_H + 6, C_BLACK);
    gfx_text(MARGIN, 0, label, 21, C_GRAY);
    gfx_text(MARGIN, GFX_FONT_H + 2, value, 21, color);
    gfx_band_flush(y);
    return y + 2 * GFX_FONT_H + 6;
}

static void share_body(void)
{
    if (!s_share_ok) {
        ui_body_message("Couldn't start sharing", "press to go back");
        return;
    }
    share_status_t st;
    share_get_status(&st);
    char buf[40];
    int y = UI_BODY_Y + 6;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    y = share_line(y, "1. Join Wi-Fi", st.ssid, C_WHITE);
    y = share_line(y, "   Password", st.password, C_WHITE);
    y = share_line(y, "2. Open in a browser", st.url, C_CYAN);
    snprintf(buf, sizeof(buf), "%d connected, %d sent", st.clients, st.downloads);
    y = share_line(y, "Status", buf, st.clients ? C_GREEN : C_GRAY);
    y = share_line(y, "Highlighted file", s_share_name, C_ACCENT);
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H - GFX_FONT_H - 4, C_BLACK);
    gfx_band_begin(GFX_FONT_H + 4, C_BLACK);
    gfx_text(MARGIN, 0, "press: stop sharing", 21, C_GRAY);
    gfx_band_flush(UI_BODY_Y + UI_BODY_H - GFX_FONT_H - 4);
}

static void share_enter(void)
{
    wifi_scan_pause(false);
    s_share_ok = share_start(s_share_name);
}

static void share_leave(void)
{
    share_stop();
    wifi_scan_resume();
}

static void share_press(void)
{
    ui_show(&ui_screen_files);
}

const ui_screen_t ui_screen_share = {
    .name = "Share",
    .icon = &icon_folder_24,
    .in_menu = false,
    .refresh_ms = 1000,
    .enter = share_enter,
    .leave = share_leave,
    .header = share_header,
    .draw_body = share_body,
    .on_short = share_press,
};
