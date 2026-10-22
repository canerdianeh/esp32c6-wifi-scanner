/*
    DMX512 on a UART and an RS485 transceiver (MAX485, SN75176, ...):
    250 kbit/s, 8N2, a break and mark-after-break before every frame.
*/
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int uart;
    int tx_gpio;
    int rx_gpio;        /* -1 for output-only ports */
    int dir_gpio;       /* DE and /RE, -1 if the transceiver is hard-wired */
} dmx_port_pins_t;

/* Sends universe slot `slot` continuously at fps frames per second. */
esp_err_t dmx_port_start_output(const dmx_port_pins_t *pins, int slot, int fps);

/* Receives frames into universe slot `slot`. */
esp_err_t dmx_port_start_input(const dmx_port_pins_t *pins, int slot);
