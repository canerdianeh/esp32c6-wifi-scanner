/*
    The SPI bus shared by the display and the microSD card.
*/
#include <stdbool.h>
#include "sdkconfig.h"
#include "board_spi.h"

/* Largest single transfer: one display band (172 x 44 pixels, 16-bit), or an SD block run. */
#define MAX_TRANSFER_BYTES (172 * 44 * 2)

esp_err_t board_spi_init(void)
{
    static bool s_done;
    if (s_done) {
        return ESP_OK;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_SCANNER_SPI_PIN_MOSI,
#if CONFIG_SCANNER_SD_LOG
        .miso_io_num = CONFIG_SCANNER_SPI_PIN_MISO,
#else
        .miso_io_num = -1,
#endif
        .sclk_io_num = CONFIG_SCANNER_SPI_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_TRANSFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    s_done = (err == ESP_OK);
    return err;
}
