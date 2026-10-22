/*
    Addressable pixels over SPI (DMA), so long strips cost no CPU time:
    - WS2812 / SK6812: each data bit is sent as 4 SPI bits at 3.2 MHz,
      1 = 1110 (938 ns high), 0 = 1000 (313 ns high), 1.25 us per bit.
    - APA102 / SK9822: native SPI, start frame, 0xE0|brightness + 3 colors
      per pixel, end frame.

    Pixels fill the first universe from the start address, then continue at
    channel 1 of the following universes. A pixel never straddles two
    universes (170 RGB or 128 RGBW pixels per full universe).
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "node_config.h"
#include "universe.h"
#include "pixels.h"

static const char *TAG = "pixels";

#define PIXEL_HOST SPI2_HOST
#define WS2812_SPI_HZ 3200000
#define APA102_SPI_HZ 4000000
#define WS2812_RESET_BYTES 120      /* 300 us low latches WS2812B and SK6812 */
#define MAX_UNIVERSES 4

/* Two data bits per SPI byte. */
static const uint8_t WS_LUT[4] = { 0x88, 0x8E, 0xE8, 0xEE };

/* Where each color of the pixel order goes in the output: index into the
 * DMX channels (R, G, B, W) for each wire position. */
static const uint8_t WIRE_ORDER[][4] = {
    [PIX_ORDER_RGB] = { 0, 1, 2 },
    [PIX_ORDER_GRB] = { 1, 0, 2 },
    [PIX_ORDER_BGR] = { 2, 1, 0 },
    [PIX_ORDER_RGBW] = { 0, 1, 2, 3 },
    [PIX_ORDER_GRBW] = { 1, 0, 2, 3 },
};

static spi_device_handle_t s_spi;
static uint8_t *s_buf;
static size_t s_buf_len;
static int s_slots[MAX_UNIVERSES];
static int s_universes;
static int s_first_count;       /* pixels in the first universe */
static int s_per_universe;      /* pixels in each later universe */

static void encode(const uint8_t *rgbw, int index, int cpp)
{
    const uint8_t *order = WIRE_ORDER[g_config.pix.order];
    if (g_config.pix.type == PIX_WS2812) {
        uint8_t *p = s_buf + 1 + index * cpp * 4;   /* byte 0 keeps the line low first */
        for (int c = 0; c < cpp; c++) {
            uint8_t v = rgbw[order[c]];
            *p++ = WS_LUT[(v >> 6) & 3];
            *p++ = WS_LUT[(v >> 4) & 3];
            *p++ = WS_LUT[(v >> 2) & 3];
            *p++ = WS_LUT[v & 3];
        }
    } else {
        /* APA102 wires blue, green, red; the order setting maps DMX channels
         * to those three. */
        uint8_t *p = s_buf + 4 + index * 4;
        p[0] = 0xE0 | 31;
        p[1] = rgbw[order[2]];
        p[2] = rgbw[order[1]];
        p[3] = rgbw[order[0]];
    }
}

static void pixels_task(void *arg)
{
    static uint8_t dmx[DMX_SLOTS];
    const int cpp = pix_channels(g_config.pix.order);

    for (;;) {
        /* Wake on new data, or once a second to apply data loss. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        int pixel = 0;
        for (int u = 0; u < s_universes; u++) {
            int64_t age_us = universe_read(s_slots[u], dmx);
            if (g_config.loss == LOSS_BLACKOUT && (age_us < 0 || age_us > LOSS_TIMEOUT_MS * 1000LL)) {
                memset(dmx, 0, sizeof(dmx));
            }
            int start = u == 0 ? g_config.pix.address - 1 : 0;
            int n = u == 0 ? s_first_count : s_per_universe;
            for (int i = 0; i < n && pixel < g_config.pix.count; i++, pixel++) {
                encode(dmx + start + i * cpp, pixel, cpp);
            }
        }

        spi_transaction_t t = { .length = s_buf_len * 8, .tx_buffer = s_buf };
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPI: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t pixels_start(void)
{
    const int count = g_config.pix.count;
    const int cpp = pix_channels(g_config.pix.order);
    const bool ws = g_config.pix.type == PIX_WS2812;
    ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_INVALID_ARG, TAG, "no pixels");
    ESP_RETURN_ON_FALSE(ws || CONFIG_NODE_PIXEL_CLOCK_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "APA102 needs a clock GPIO");

    s_first_count = (DMX_SLOTS - (g_config.pix.address - 1)) / cpp;
    s_per_universe = DMX_SLOTS / cpp;
    s_universes = 1;
    for (int left = count - s_first_count; left > 0 && s_universes < MAX_UNIVERSES; left -= s_per_universe) {
        s_universes++;
    }
    for (int u = 0; u < s_universes; u++) {
        s_slots[u] = universe_subscribe(g_config.pix.universe + u);
        ESP_RETURN_ON_FALSE(s_slots[u] >= 0, ESP_ERR_NO_MEM, TAG, "too many universes");
    }

    s_buf_len = ws ? 1 + count * cpp * 4 + WS2812_RESET_BYTES
                   : 4 + count * 4 + (count + 15) / 16 + 4;
    s_buf = heap_caps_calloc(1, s_buf_len, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG, "no memory for %u byte SPI buffer", (unsigned)s_buf_len);
    if (!ws) {
        /* Start frame is already zero; the end frame supplies the extra
         * clock edges that push data to the last pixels. */
        memset(s_buf + 4 + count * 4, 0xFF, s_buf_len - 4 - count * 4);
    }

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_NODE_PIXEL_DATA_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = ws ? -1 : CONFIG_NODE_PIXEL_CLOCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_buf_len,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(PIXEL_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    spi_device_interface_config_t dev = {
        .clock_speed_hz = ws ? WS2812_SPI_HZ : APA102_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(PIXEL_HOST, &dev, &s_spi), TAG, "SPI device");

    TaskHandle_t task;
    ESP_RETURN_ON_FALSE(xTaskCreate(pixels_task, "pixels", 3072, NULL, 9, &task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    for (int u = 0; u < s_universes; u++) {
        universe_listen(s_slots[u], task);
    }
    ESP_LOGI(TAG, "%d %s pixels from universe %u address %u (%d universe%s)", count,
             ws ? "WS2812" : "APA102", g_config.pix.universe, g_config.pix.address,
             s_universes, s_universes > 1 ? "s" : "");
    return ESP_OK;
}
