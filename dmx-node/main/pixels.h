/*
    Addressable LED pixels on the SPI data (and clock) pins.
*/
#pragma once

#include "esp_err.h"

/* Subscribes the strip's universes and starts updating it. */
esp_err_t pixels_start(void);
