/*
    PWM outputs on the LEDC peripheral: dimmers for LED drivers (MOSFETs),
    or servo pulses (50 Hz, min_us..max_us).
*/
#pragma once

#include "esp_err.h"

esp_err_t pwm_out_start(void);
