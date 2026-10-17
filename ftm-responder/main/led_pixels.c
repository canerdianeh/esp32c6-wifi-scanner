/*
    Minimal WS2812 / WS2814 / SK6812 driver built on the RMT peripheral.
*/
#include <stdlib.h>
#include "driver/rmt_tx.h"
#include "soc/soc_caps.h"
#include "esp_check.h"
#include "led_pixels.h"

static const char *TAG = "led_pixels";

/* 10 MHz RMT clock: 1 tick = 100 ns.
 * These timings sit inside the WS2812, WS2812B, SK6812 and WS2814 windows:
 *   0 bit: 300 ns high, 900 ns low
 *   1 bit: 700 ns high, 600 ns low (WS2814 needs >= 580 ns low)
 * The latch/reset gap (>= 280 us for WS2814) comes from the frame interval,
 * since frames are sent at most every few milliseconds. */
#define LED_RMT_RESOLUTION_HZ 10000000
#define T0H_TICKS 3
#define T0L_TICKS 9
#define T1H_TICKS 7
#define T1L_TICKS 6

struct led_pixels_t {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t encoder;
    led_order_t order;
    uint16_t count;
    uint8_t bytes_per_pixel;
    uint8_t *buf;
};

esp_err_t led_pixels_new(int gpio, uint16_t count, led_order_t order, led_pixels_handle_t *ret_strip)
{
    ESP_RETURN_ON_FALSE(count > 0 && ret_strip, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    struct led_pixels_t *strip = calloc(1, sizeof(*strip));
    ESP_RETURN_ON_FALSE(strip, ESP_ERR_NO_MEM, TAG, "no memory for strip");
    strip->order = order;
    strip->count = count;
    strip->bytes_per_pixel = (order == LED_ORDER_GRB || order == LED_ORDER_RGB) ? 3 : 4;
    strip->buf = calloc(count, strip->bytes_per_pixel);
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(strip->buf, ESP_ERR_NO_MEM, fail, TAG, "no memory for pixel buffer");

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 2,
    };
    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &strip->chan), fail, TAG, "create RMT channel on GPIO%d", gpio);

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H_TICKS, .level1 = 0, .duration1 = T0L_TICKS },
        .bit1 = { .level0 = 1, .duration0 = T1H_TICKS, .level1 = 0, .duration1 = T1L_TICKS },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &strip->encoder), fail, TAG, "create encoder");
    ESP_GOTO_ON_ERROR(rmt_enable(strip->chan), fail, TAG, "enable RMT channel");

    *ret_strip = strip;
    return ESP_OK;

fail:
    if (strip->encoder) {
        rmt_del_encoder(strip->encoder);
    }
    if (strip->chan) {
        rmt_del_channel(strip->chan);
    }
    free(strip->buf);
    free(strip);
    return ret;
}

esp_err_t led_pixels_fill(led_pixels_handle_t strip, led_color_t c)
{
    /* The buffer must not change while a previous frame is still going out. */
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(strip->chan, 100), TAG, "previous frame timed out");

    uint8_t px[4];
    switch (strip->order) {
    case LED_ORDER_GRB:  px[0] = c.g; px[1] = c.r; px[2] = c.b; break;
    case LED_ORDER_RGB:  px[0] = c.r; px[1] = c.g; px[2] = c.b; break;
    case LED_ORDER_WRGB: px[0] = c.w; px[1] = c.r; px[2] = c.g; px[3] = c.b; break;
    case LED_ORDER_RGBW: px[0] = c.r; px[1] = c.g; px[2] = c.b; px[3] = c.w; break;
    case LED_ORDER_GRBW: px[0] = c.g; px[1] = c.r; px[2] = c.b; px[3] = c.w; break;
    }
    for (uint16_t i = 0; i < strip->count; i++) {
        for (uint8_t j = 0; j < strip->bytes_per_pixel; j++) {
            strip->buf[i * strip->bytes_per_pixel + j] = px[j];
        }
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    return rmt_transmit(strip->chan, strip->encoder, strip->buf,
                        (size_t)strip->count * strip->bytes_per_pixel, &tx_cfg);
}
