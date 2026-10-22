/*
    Runtime settings, stored in NVS as the same JSON the web page uses, so
    settings saved by an older firmware still load (missing fields keep
    their defaults).
*/
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "node_config.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "node"
#define NVS_KEY "config"

node_config_t g_config;

static const char *const SOURCE_NAMES[] = { "sacn", "artnet", "dmx" };
static const char *const LOSS_NAMES[] = { "hold", "blackout" };
static const char *const PORT_NAMES[] = { "off", "in", "out" };
static const char *const PIX_NAMES[] = { "off", "ws2812", "apa102" };
static const char *const ORDER_NAMES[] = { "rgb", "grb", "bgr", "rgbw", "grbw" };
static const char *const PWM_NAMES[] = { "off", "led", "servo" };

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

int pix_channels(pix_order_t order)
{
    return (order == PIX_ORDER_RGBW || order == PIX_ORDER_GRBW) ? 4 : 3;
}

static void set_defaults(node_config_t *cfg)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "%s-%02X%02X", CONFIG_NODE_NAME, mac[4], mac[5]);
    cfg->source = SOURCE_SACN;
    cfg->loss = LOSS_HOLD;
    cfg->fps = 40;
    cfg->port_a = (port_cfg_t){ .mode = PORT_OUT, .universe = 2 };
    cfg->port_b = (port_cfg_t){ .mode = PORT_OUT, .universe = 1 };
    cfg->pix.type = PIX_OFF;
    cfg->pix.order = PIX_ORDER_GRB;
    cfg->pix.universe = 3;
    cfg->pix.address = 1;
    cfg->pix.count = 50;
    cfg->pwm.mode = PWM_OFF;
    cfg->pwm.universe = 1;
    cfg->pwm.address = 1;
    cfg->pwm.count = 4;
    cfg->pwm.servo_min_us = 1000;
    cfg->pwm.servo_max_us = 2000;
}

/* Fixes combinations the hardware can't do, rather than refusing them, so a
 * partly wrong patch still leaves a working node. */
static void sanitize(node_config_t *cfg)
{
    if (cfg->source == SOURCE_DMX) {
        cfg->port_a.mode = PORT_IN;     /* the wired input is port A */
    } else if (cfg->port_a.mode == PORT_IN) {
        cfg->port_a.mode = PORT_OFF;
    }
    if (cfg->port_b.mode == PORT_IN) {
        cfg->port_b.mode = PORT_OUT;    /* port B has no receiver wired */
    }
    if (cfg->fps < 1 || cfg->fps > 44) {
        cfg->fps = 40;                  /* 44 Hz is the most a full 512-slot frame allows */
    }
    if (cfg->pix.count > CONFIG_NODE_PIXEL_MAX) {
        cfg->pix.count = CONFIG_NODE_PIXEL_MAX;
    }
    if (cfg->pix.address < 1 || cfg->pix.address > 512) {
        cfg->pix.address = 1;
    }
    if (cfg->pwm.count > PWM_MAX_CHANNELS) {
        cfg->pwm.count = PWM_MAX_CHANNELS;
    }
    int width = cfg->pwm.count * (cfg->pwm.fine ? 2 : 1);
    if (cfg->pwm.address < 1 || cfg->pwm.address + width - 1 > 512) {
        cfg->pwm.address = 1;
    }
    if (cfg->pwm.servo_min_us < 500 || cfg->pwm.servo_min_us > 2500) {
        cfg->pwm.servo_min_us = 1000;
    }
    if (cfg->pwm.servo_max_us < 500 || cfg->pwm.servo_max_us > 2500) {
        cfg->pwm.servo_max_us = 2000;
    }
}

static cJSON *add_port(cJSON *root, const char *key, const port_cfg_t *p)
{
    cJSON *o = cJSON_AddObjectToObject(root, key);
    cJSON_AddStringToObject(o, "mode", PORT_NAMES[p->mode]);
    cJSON_AddNumberToObject(o, "univ", p->universe);
    return o;
}

static char *to_json(const node_config_t *cfg, bool with_password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", cfg->name);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi_ssid);
    if (with_password) {
        cJSON_AddStringToObject(wifi, "pass", cfg->wifi_pass);
    } else {
        cJSON_AddBoolToObject(wifi, "pass_set", cfg->wifi_pass[0] != '\0');
    }
    cJSON_AddStringToObject(root, "source", SOURCE_NAMES[cfg->source]);
    cJSON_AddStringToObject(root, "loss", LOSS_NAMES[cfg->loss]);
    cJSON_AddNumberToObject(root, "fps", cfg->fps);
    add_port(root, "a", &cfg->port_a);
    add_port(root, "b", &cfg->port_b);

    cJSON *pix = cJSON_AddObjectToObject(root, "pix");
    cJSON_AddStringToObject(pix, "type", PIX_NAMES[cfg->pix.type]);
    cJSON_AddStringToObject(pix, "order", ORDER_NAMES[cfg->pix.order]);
    cJSON_AddNumberToObject(pix, "univ", cfg->pix.universe);
    cJSON_AddNumberToObject(pix, "addr", cfg->pix.address);
    cJSON_AddNumberToObject(pix, "count", cfg->pix.count);

    cJSON *pwm = cJSON_AddObjectToObject(root, "pwm");
    cJSON_AddStringToObject(pwm, "mode", PWM_NAMES[cfg->pwm.mode]);
    cJSON_AddBoolToObject(pwm, "fine", cfg->pwm.fine);
    cJSON_AddNumberToObject(pwm, "univ", cfg->pwm.universe);
    cJSON_AddNumberToObject(pwm, "addr", cfg->pwm.address);
    cJSON_AddNumberToObject(pwm, "count", cfg->pwm.count);
    cJSON_AddNumberToObject(pwm, "min_us", cfg->pwm.servo_min_us);
    cJSON_AddNumberToObject(pwm, "max_us", cfg->pwm.servo_max_us);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *node_config_to_json(const node_config_t *cfg)
{
    return to_json(cfg, false);
}

/* Small helpers that leave the field alone when the key is missing or the
 * wrong type. */
static void get_str(const cJSON *o, const char *key, char *dst, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v)) {
        strlcpy(dst, v->valuestring, size);
    }
}

static void get_u16(const cJSON *o, const char *key, uint16_t *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 65535) {
        *dst = (uint16_t)v->valuedouble;
    }
}

static void get_u8(const cJSON *o, const char *key, uint8_t *dst)
{
    uint16_t v = *dst;
    get_u16(o, key, &v);
    if (v <= 255) {
        *dst = (uint8_t)v;
    }
}

static void get_bool(const cJSON *o, const char *key, bool *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) {
        *dst = cJSON_IsTrue(v);
    }
}

/* Enums are stored in the struct as their own type; this works on any of
 * them through an int. */
static void get_enum(const cJSON *o, const char *key, const char *const *names, size_t n, int *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(v->valuestring, names[i]) == 0) {
            *dst = (int)i;
            return;
        }
    }
    ESP_LOGW(TAG, "unknown %s \"%s\" ignored", key, v->valuestring);
}

#define GET_ENUM(o, key, names, field) do { \
        int tmp_ = (int)(field); \
        get_enum((o), (key), (names), COUNT_OF(names), &tmp_); \
        (field) = tmp_; \
    } while (0)

static void get_port(const cJSON *root, const char *key, port_cfg_t *p)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsObject(o)) {
        GET_ENUM(o, "mode", PORT_NAMES, p->mode);
        get_u16(o, "univ", &p->universe);
    }
}

esp_err_t node_config_patch_json(node_config_t *cfg, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    get_str(root, "name", cfg->name, sizeof(cfg->name));
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        get_str(wifi, "ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
        get_str(wifi, "pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    }
    GET_ENUM(root, "source", SOURCE_NAMES, cfg->source);
    GET_ENUM(root, "loss", LOSS_NAMES, cfg->loss);
    get_u8(root, "fps", &cfg->fps);
    get_port(root, "a", &cfg->port_a);
    get_port(root, "b", &cfg->port_b);

    const cJSON *pix = cJSON_GetObjectItemCaseSensitive(root, "pix");
    if (cJSON_IsObject(pix)) {
        GET_ENUM(pix, "type", PIX_NAMES, cfg->pix.type);
        GET_ENUM(pix, "order", ORDER_NAMES, cfg->pix.order);
        get_u16(pix, "univ", &cfg->pix.universe);
        get_u16(pix, "addr", &cfg->pix.address);
        get_u16(pix, "count", &cfg->pix.count);
    }

    const cJSON *pwm = cJSON_GetObjectItemCaseSensitive(root, "pwm");
    if (cJSON_IsObject(pwm)) {
        GET_ENUM(pwm, "mode", PWM_NAMES, cfg->pwm.mode);
        get_bool(pwm, "fine", &cfg->pwm.fine);
        get_u16(pwm, "univ", &cfg->pwm.universe);
        get_u16(pwm, "addr", &cfg->pwm.address);
        get_u8(pwm, "count", &cfg->pwm.count);
        get_u16(pwm, "min_us", &cfg->pwm.servo_min_us);
        get_u16(pwm, "max_us", &cfg->pwm.servo_max_us);
    }

    cJSON_Delete(root);
    sanitize(cfg);
    return ESP_OK;
}

esp_err_t node_config_load(void)
{
    set_defaults(&g_config);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return ESP_OK;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY, NULL, &len);
    if (err == ESP_OK) {
        char *json = malloc(len);
        if (json && nvs_get_str(h, NVS_KEY, json, &len) == ESP_OK) {
            err = node_config_patch_json(&g_config, json, len);
        }
        free(json);
    }
    nvs_close(h);
    sanitize(&g_config);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t node_config_save(const node_config_t *cfg)
{
    char *json = to_json(cfg, true);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY, json);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(json);
    return err;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(750));     /* let the HTTP or GATT reply go out */
    esp_restart();
}

esp_err_t node_config_apply_json(const char *json, size_t len)
{
    node_config_t next = g_config;
    esp_err_t err = node_config_patch_json(&next, json, len);
    if (err != ESP_OK) {
        return err;
    }
    if (memcmp(&next, &g_config, sizeof(next)) == 0) {
        return ESP_OK;
    }
    err = node_config_save(&next);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "settings saved, restarting");
        xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    }
    return err;
}
