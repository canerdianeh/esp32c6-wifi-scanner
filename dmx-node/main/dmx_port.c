#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "node_config.h"
#include "universe.h"
#include "dmx_port.h"

static const char *TAG = "dmx_port";

#define DMX_BAUD 250000
/* The transmitter must send a break of at least 92 us and a mark after
 * break of at least 12 us; these leave margin for slow receivers. */
#define BREAK_US 176
#define MAB_US 16

/* DMX output timing is busy-waited, so keep it off the core running Wi-Fi. */
#define DMX_CORE (portNUM_PROCESSORS - 1)

typedef struct {
    dmx_port_pins_t pins;
    int slot;
    int fps;
    QueueHandle_t events;
} port_t;

static esp_err_t setup_uart(port_t *p, bool input)
{
    const uart_config_t cfg = {
        .baud_rate = DMX_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* The TX ring buffer holds a whole frame so uart_write_bytes doesn't block. */
    ESP_RETURN_ON_ERROR(uart_driver_install(p->pins.uart, 1024, input ? 0 : 1024,
                                            input ? 20 : 0, input ? &p->events : NULL, 0),
                        TAG, "install UART%d", p->pins.uart);
    ESP_RETURN_ON_ERROR(uart_param_config(p->pins.uart, &cfg), TAG, "configure UART%d", p->pins.uart);
    ESP_RETURN_ON_ERROR(uart_set_pin(p->pins.uart, p->pins.tx_gpio, input ? p->pins.rx_gpio : UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "set UART%d pins", p->pins.uart);

    if (p->pins.dir_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << p->pins.dir_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "direction GPIO%d", p->pins.dir_gpio);
        gpio_set_level(p->pins.dir_gpio, input ? 0 : 1);
    }
    return ESP_OK;
}

static void output_task(void *arg)
{
    port_t *p = arg;
    uint8_t frame[1 + DMX_SLOTS] = { 0 };   /* start code 0, then the slots */
    const TickType_t period = pdMS_TO_TICKS(1000 / p->fps);
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        int64_t age_us = universe_read(p->slot, frame + 1);
        if (g_config.loss == LOSS_BLACKOUT && (age_us < 0 || age_us > LOSS_TIMEOUT_MS * 1000LL)) {
            memset(frame + 1, 0, DMX_SLOTS);
        }

        /* The break is made by inverting the idle-high TX line, which
         * needs the previous frame fully sent. */
        uart_wait_tx_done(p->pins.uart, pdMS_TO_TICKS(50));
        uart_set_line_inverse(p->pins.uart, UART_SIGNAL_TXD_INV);
        esp_rom_delay_us(BREAK_US);
        uart_set_line_inverse(p->pins.uart, UART_SIGNAL_INV_DISABLE);
        esp_rom_delay_us(MAB_US);
        uart_write_bytes(p->pins.uart, frame, sizeof(frame));

        /* A full frame takes 22.7 ms, so at 44 fps this never waits. */
        xTaskDelayUntil(&wake, period ? period : 1);
    }
}

/* Frames are cut at each break. The UART reports a break as a frame error
 * plus a 0x00 byte, after the bytes that came before it.
 * TODO: event-based capture is fine for a steady console; a GPIO-edge or
 * UART-ISR based capture would also measure break/MAB timing and could
 * support RDM. */
static void input_task(void *arg)
{
    port_t *p = arg;
    uint8_t *frame = malloc(1 + DMX_SLOTS + 64);    /* room for the break byte and overruns */
    int len = 0;
    bool synced = false;    /* drop data until the first break */
    uart_event_t ev;

    for (;;) {
        if (!xQueueReceive(p->events, &ev, portMAX_DELAY)) {
            continue;
        }
        switch (ev.type) {
        case UART_DATA: {
            int room = 1 + DMX_SLOTS + 64 - len;
            int n = uart_read_bytes(p->pins.uart, frame + len, ev.size < room ? ev.size : room, 0);
            if (n > 0 && synced) {
                len += n;
            }
            if (len >= 1 + DMX_SLOTS + 64) {
                len = 0;    /* no break for too long: not DMX */
                synced = false;
            }
            break;
        }
        case UART_BREAK: {
            size_t buffered = 0;
            uart_get_buffered_data_len(p->pins.uart, &buffered);
            int room = 1 + DMX_SLOTS + 64 - len;
            if (buffered > 0) {
                int n = uart_read_bytes(p->pins.uart, frame + len, buffered < room ? buffered : room, 0);
                if (n > 0 && synced) {
                    len += n;
                }
            }
            if (len > 0 && frame[len - 1] == 0) {
                len--;      /* the break's own 0x00 */
            }
            if (synced && len > 1 && frame[0] == 0x00) {   /* start code 0: dimmer data */
                int slots = len - 1 > DMX_SLOTS ? DMX_SLOTS : len - 1;
                universe_write(p->slot, frame + 1, slots, UNIV_SRC_DMX, 100, 0);
            }
            len = 0;
            synced = true;
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART%d receive overflow", p->pins.uart);
            uart_flush_input(p->pins.uart);
            xQueueReset(p->events);
            len = 0;
            synced = false;
            break;
        default:
            break;  /* frame errors are expected at every break */
        }
    }
}

esp_err_t dmx_port_start_output(const dmx_port_pins_t *pins, int slot, int fps)
{
    port_t *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory");
    p->pins = *pins;
    p->slot = slot;
    p->fps = fps;
    ESP_RETURN_ON_ERROR(setup_uart(p, false), TAG, "output setup");
    ESP_LOGI(TAG, "UART%d: DMX out on GPIO%d", pins->uart, pins->tx_gpio);
    return xTaskCreatePinnedToCore(output_task, "dmx_out", 3072, p, 10, NULL, DMX_CORE) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t dmx_port_start_input(const dmx_port_pins_t *pins, int slot)
{
    port_t *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory");
    p->pins = *pins;
    p->slot = slot;
    ESP_RETURN_ON_ERROR(setup_uart(p, true), TAG, "input setup");
    ESP_LOGI(TAG, "UART%d: DMX in on GPIO%d", pins->uart, pins->rx_gpio);
    return xTaskCreatePinnedToCore(input_task, "dmx_in", 3072, p, 10, NULL, DMX_CORE) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
