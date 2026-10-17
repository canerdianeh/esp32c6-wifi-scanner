/*
    Minimal WS2812 / WS2814 / SK6812 driver built on the RMT peripheral.
    Every pixel on a channel shows the same color.
*/
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    LED_ORDER_GRB,   /* WS2812, WS2812B, SK6812 RGB */
    LED_ORDER_RGB,
    LED_ORDER_WRGB,  /* WS2814 per datasheet */
    LED_ORDER_RGBW,
    LED_ORDER_GRBW,  /* SK6812 RGBW */
} led_order_t;

typedef struct {
    uint8_t r, g, b, w;
} led_color_t;

typedef struct led_pixels_t *led_pixels_handle_t;

esp_err_t led_pixels_new(int gpio, uint16_t count, led_order_t order, led_pixels_handle_t *ret_strip);

/* Set every pixel to `color` and send it. Waits for the previous frame to finish first. */
esp_err_t led_pixels_fill(led_pixels_handle_t strip, led_color_t color);
