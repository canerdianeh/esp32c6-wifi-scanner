/*
    Drawing on the ST7789 (Waveshare ESP32-C6-LCD-1.47); see gfx.h.
*/
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "font8x16.h"
#include "board_spi.h"
#include "gfx.h"

static const char *TAG = "gfx";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_band;    /* LCD_W x GFX_BAND_MAX_H, byte-swapped RGB565 (the panel expects big-endian) */
static int s_band_h;

static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

uint16_t gfx_rgb565(rgb_f_t c)
{
    int r = (int)c.r, g = (int)c.g, b = (int)c.b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Blend fg over bg with alpha 0..15 (RGB565, native byte order). */
static uint16_t blend(uint16_t fg, uint16_t bg, int a)
{
    if (a >= 15) {
        return fg;
    }
    if (a <= 0) {
        return bg;
    }
    int r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (15 - a)) / 15;
    int g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (15 - a)) / 15;
    int b = ((fg & 0x1F) * a + (bg & 0x1F) * (15 - a)) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void blend_px(int x, int y, uint16_t fg, int a)
{
    if (a && x >= 0 && x < LCD_W && y >= 0 && y < s_band_h) {
        uint16_t *p = &s_band[y * LCD_W + x];
        *p = swap16(blend(fg, swap16(*p), a));
    }
}

void gfx_band_begin(int h, uint16_t bg)
{
    /* Wait until the previous band has finished going out over SPI before reusing the buffer. */
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
    s_band_h = h < GFX_BAND_MAX_H ? h : GFX_BAND_MAX_H;
    uint16_t v = swap16(bg);
    for (int i = 0; i < LCD_W * s_band_h; i++) {
        s_band[i] = v;
    }
}

void gfx_band_flush(int y)
{
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + s_band_h, s_band) != ESP_OK) {
        /* No transfer was queued, so the done callback won't release the buffer. */
        xSemaphoreGive(s_flush_done);
    }
}

void gfx_clear_rows(int y0, int y1, uint16_t bg)
{
    for (int y = y0; y < y1; y += GFX_BAND_MAX_H) {
        int h = y1 - y < GFX_BAND_MAX_H ? y1 - y : GFX_BAND_MAX_H;
        gfx_band_begin(h, bg);
        gfx_band_flush(y);
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w, y1 = y + h > s_band_h ? s_band_h : y + h;
    uint16_t v = swap16(color);
    for (int j = y0; j < y1; j++) {
        for (int i = x0; i < x1; i++) {
            s_band[j * LCD_W + i] = v;
        }
    }
}

void gfx_dot(int x, int y, uint16_t color)
{
    static const uint8_t rows[5] = { 0x0E, 0x1F, 0x1F, 0x1F, 0x0E };
    for (int j = 0; j < 5; j++) {
        for (int i = 0; i < 5; i++) {
            if ((rows[j] >> (4 - i)) & 1) {
                blend_px(x + i, y + j, color, 15);
            }
        }
    }
}

void gfx_ring(int cx, int cy, int r, uint16_t color)
{
    int y0 = cy - r - 1 < 0 ? 0 : cy - r - 1, y1 = cy + r + 1 >= s_band_h ? s_band_h - 1 : cy + r + 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy))) - r;
            d = d < 0 ? -d : d;
            if (d < 1.0f) {
                blend_px(x, y, color, (int)(15 * (1.0f - d)));
            }
        }
    }
}

void gfx_icon(int x, int y, const icon_t *icon, uint16_t color)
{
    int stride = (icon->w + 1) / 2;
    for (int j = 0; j < icon->h; j++) {
        for (int i = 0; i < icon->w; i++) {
            uint8_t byte = icon->alpha[j * stride + i / 2];
            blend_px(x + i, y + j, color, (i & 1) ? (byte & 0x0F) : (byte >> 4));
        }
    }
}

static void draw_char(int x, int y, char ch, int scale, uint16_t fg)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) {
        ch = '?';
    }
    const uint8_t *glyph = font_glyphs[ch - FONT_FIRST];
    for (int gy = 0; gy < FONT_H; gy++) {
        for (int gx = 0; gx < FONT_W; gx++) {
            uint8_t byte = glyph[(gy * FONT_W + gx) / 2];
            int a = (gx & 1) ? (byte & 0x0F) : (byte >> 4);
            if (!a) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    blend_px(x + gx * scale + sx, y + gy * scale + sy, fg, a);
                }
            }
        }
    }
}

void gfx_text(int x, int y, const char *s, int max_chars, uint16_t fg)
{
    int len = (int)strlen(s);
    for (int i = 0; i < len && i < max_chars; i++) {
        char ch = (len > max_chars && i == max_chars - 1) ? '~' : s[i];
        draw_char(x + i * FONT_W, y, ch, 1, fg);
    }
}

void gfx_text_right(int right_x, int y, const char *s, uint16_t fg)
{
    gfx_text(right_x - (int)strlen(s) * FONT_W, y, s, LCD_W / FONT_W, fg);
}

void gfx_text_scaled(int x, int y, const char *s, int scale, uint16_t fg)
{
    for (int i = 0; s[i]; i++) {
        draw_char(x + i * FONT_W * scale, y, s[i], scale, fg);
    }
}

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ledc_channel_config_t chan = {
        .gpio_num = CONFIG_SCANNER_DISPLAY_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    return ledc_channel_config(&chan);
}

static void backlight_set(int percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t gfx_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    xSemaphoreGive(s_flush_done);

    s_band = heap_caps_malloc(LCD_W * GFX_BAND_MAX_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_band, ESP_ERR_NO_MEM, TAG, "band buffer");

    ESP_RETURN_ON_ERROR(board_spi_init(), TAG, "SPI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_SCANNER_DISPLAY_PIN_CS,
        .dc_gpio_num = CONFIG_SCANNER_DISPLAY_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_SCANNER_DISPLAY_SPI_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_flush_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &io),
                        TAG, "panel IO");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_SCANNER_DISPLAY_PIN_RST,
#if CONFIG_SCANNER_DISPLAY_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "ST7789 panel");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if CONFIG_SCANNER_DISPLAY_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_set_gap(s_panel, CONFIG_SCANNER_DISPLAY_X_GAP, CONFIG_SCANNER_DISPLAY_Y_GAP);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Blank screen with a startup note, then backlight on. */
    gfx_clear_rows(0, LCD_H, C_BLACK);
    gfx_band_begin(GFX_FONT_H, C_BLACK);
    gfx_text(MARGIN, 0, "Starting...", 20, C_GRAY);
    gfx_band_flush(40);
    backlight_set(CONFIG_SCANNER_DISPLAY_BRIGHTNESS);
    ESP_LOGI(TAG, "%dx%d ST7789 ready", LCD_W, LCD_H);
    return ESP_OK;
}
