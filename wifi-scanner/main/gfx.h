/*
    Drawing on the ST7789 (Waveshare ESP32-C6-LCD-1.47).

    The screen is drawn one horizontal band at a time into a small DMA buffer:
    gfx_band_begin(h, bg), draw with band-relative y coordinates, then
    gfx_band_flush(y) sends it to screen row y. Drawing is clipped to the band.
    Only one task (the UI task) may draw.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "color_scale.h"
#include "icons.h"

#define LCD_W           CONFIG_SCANNER_DISPLAY_WIDTH
#define LCD_H           CONFIG_SCANNER_DISPLAY_HEIGHT
#define GFX_BAND_MAX_H  44
#define GFX_FONT_W      8
#define GFX_FONT_H      16
#define MARGIN          3

/* RGB565 colors */
#define C_BLACK     0x0000
#define C_WHITE     0xFFFF
#define C_GRAY      0xB5B6      /* secondary text (~180/255) */
#define C_DIM       0x4208
#define C_BAR_BG    0x2124
#define C_LINE      0x31A6
#define C_DIM_DOT   0x8C71      /* dimmer text, and dots for "seen recently but not now" (~140/255) */
#define C_ACCENT    0xFFE0      /* yellow: selection */
#define C_SEL_BG    0x2965      /* dark blue-gray behind a selected row */
#define C_GREEN     0x07E0
#define C_CYAN      0x07FF
#define C_RED       0xF800
#define C_ORANGE    0xFC60
#define C_YELLOW    0xFFE0
#define C_BLUE      0x5D5F      /* light blue: network broadcasts a location */

esp_err_t gfx_init(void);

void gfx_band_begin(int h, uint16_t bg);
void gfx_band_flush(int y);

/* Fill screen rows [y0, y1) with a color, band by band. */
void gfx_clear_rows(int y0, int y1, uint16_t bg);

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_dot(int x, int y, uint16_t color);     /* 5x5 round dot */
void gfx_ring(int cx, int cy, int r, uint16_t color);   /* 1 px anti-aliased circle */
void gfx_icon(int x, int y, const icon_t *icon, uint16_t color);

/* 8x16 text. At most max_chars characters; longer text is cut and ends in '~'. */
void gfx_text(int x, int y, const char *s, int max_chars, uint16_t fg);
void gfx_text_right(int right_x, int y, const char *s, uint16_t fg);
/* Text scaled up by an integer factor (2 = 16x32 characters). */
void gfx_text_scaled(int x, int y, const char *s, int scale, uint16_t fg);

uint16_t gfx_rgb565(rgb_f_t c);
