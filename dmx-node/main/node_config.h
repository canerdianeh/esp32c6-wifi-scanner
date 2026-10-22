/*
    Runtime settings: what the node receives and what each output does.
    Kept in NVS, read and changed as JSON by the web page and Bluetooth.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SOURCE_SACN,        /* E1.31 over Wi-Fi */
    SOURCE_ARTNET,      /* Art-Net over Wi-Fi */
    SOURCE_DMX,         /* RS485 into port A */
} source_t;

typedef enum {
    LOSS_HOLD,          /* keep the last look when data stops */
    LOSS_BLACKOUT,      /* go to zero after LOSS_TIMEOUT_MS */
} loss_t;

typedef enum {
    PORT_OFF,
    PORT_IN,            /* port A only, and only when source is SOURCE_DMX */
    PORT_OUT,
} port_mode_t;

typedef enum {
    PIX_OFF,
    PIX_WS2812,         /* WS2812 / SK6812, one data wire, encoded over SPI */
    PIX_APA102,         /* APA102 / SK9822, data and clock */
} pix_type_t;

typedef enum {
    PIX_ORDER_RGB,
    PIX_ORDER_GRB,
    PIX_ORDER_BGR,
    PIX_ORDER_RGBW,
    PIX_ORDER_GRBW,
} pix_order_t;

typedef enum {
    PWM_OFF,
    PWM_LED,
    PWM_SERVO,
} pwm_mode_t;

#define LOSS_TIMEOUT_MS 2500    /* E1.31 network data loss time */
#define PWM_MAX_CHANNELS 8

typedef struct {
    port_mode_t mode;
    uint16_t universe;
} port_cfg_t;

typedef struct {
    char name[32];
    char wifi_ssid[33];
    char wifi_pass[65];
    source_t source;
    loss_t loss;
    uint8_t fps;                /* DMX output refresh rate, 1-44 */
    port_cfg_t port_a;
    port_cfg_t port_b;
    struct {
        pix_type_t type;
        pix_order_t order;
        uint16_t universe;      /* first universe; long strips continue into the next ones */
        uint16_t address;       /* 1-512 */
        uint16_t count;
    } pix;
    struct {
        pwm_mode_t mode;
        bool fine;              /* 16-bit: two DMX channels per output (coarse, fine) */
        uint16_t universe;
        uint16_t address;       /* 1-512 */
        uint8_t count;
        uint16_t servo_min_us;  /* pulse at DMX 0 */
        uint16_t servo_max_us;  /* pulse at DMX 255 / 65535 */
    } pwm;
} node_config_t;

/* The settings in use since boot. Changes take effect after a restart. */
extern node_config_t g_config;

esp_err_t node_config_load(void);
esp_err_t node_config_save(const node_config_t *cfg);

/* JSON for the web page and Bluetooth. The Wi-Fi password is never sent out;
 * "wifi_pass_set" says whether there is one. Free the result. */
char *node_config_to_json(const node_config_t *cfg);

/* Applies the fields present in json to *cfg (missing fields are unchanged),
 * then fixes combinations that can't work. Returns ESP_ERR_INVALID_ARG if
 * json doesn't parse. */
esp_err_t node_config_patch_json(node_config_t *cfg, const char *json, size_t len);

/* Saves a patch and restarts the node shortly after, so the caller can reply. */
esp_err_t node_config_apply_json(const char *json, size_t len);

/* Number of DMX channels one pixel uses for this order (3 or 4). */
int pix_channels(pix_order_t order);
