/*
    Screen modes, the mode menu and the BOOT button.

    Button: long press opens the mode menu (from any mode). In the menu, short
    press moves to the next mode and long press opens it. Each mode handles
    short presses itself (and double presses, if it asks for them).

    Layout: 24 px header, body, 20 px footer (GPS globe and SD card status).
*/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "icons.h"

#define UI_HEADER_H     24
#define UI_FOOTER_H     20
#define UI_BODY_Y       UI_HEADER_H
#define UI_BODY_H       (LCD_H - UI_HEADER_H - UI_FOOTER_H)

typedef struct {
    const char *name;                   /* menu label */
    const icon_t *icon;                 /* 24 px menu icon */
    bool in_menu;
    bool wants_double;                  /* deliver double presses (short presses then wait ~0.35 s) */
    int refresh_ms;                     /* redraw at least this often */
    void (*enter)(void);
    void (*leave)(void);
    /* Header text: title on the left (in *title_color), info on the right. */
    void (*header)(char *title, size_t title_len, uint16_t *title_color, char *right, size_t right_len);
    void (*draw_body)(void);            /* draw screen rows [UI_BODY_Y, UI_BODY_Y + UI_BODY_H) */
    void (*on_short)(void);
    void (*on_double)(void);
} ui_screen_t;

void ui_init(void);

/* Ask the UI task to redraw soon. Safe from any task. */
void ui_request_redraw(void);

/* Simulate a button press ('S' short, 'L' long, 'D' double), e.g. from a serial command. */
void ui_inject_button(char kind);

/* Show a Bluetooth pairing passkey over the current screen (0..999999), or hide it (-1). Safe from any task. */
void ui_show_passkey(int32_t passkey);

/* Switch to a screen (e.g. the file screen opening the share screen). UI task only. */
void ui_show(const ui_screen_t *screen);

/* Draw one body row band showing a message, centered vertically in the body. UI task helper. */
void ui_body_message(const char *line1, const char *line2);

/* Demo: open the details of the nth listed network with FTM or a location. UI task only. */
bool ui_wifi_demo_open(int nth);

extern const ui_screen_t ui_screen_wifi;
extern const ui_screen_t ui_screen_gps;
extern const ui_screen_t ui_screen_ble;
extern const ui_screen_t ui_screen_band;
extern const ui_screen_t ui_screen_zigbee;
extern const ui_screen_t ui_screen_files;
extern const ui_screen_t ui_screen_share;
