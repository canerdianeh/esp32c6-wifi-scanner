/*
    The SPI bus shared by the display and the microSD card.
*/
#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

#define BOARD_SPI_HOST SPI2_HOST

/* Initialize the bus once; later calls return ESP_OK. */
esp_err_t board_spi_init(void);
