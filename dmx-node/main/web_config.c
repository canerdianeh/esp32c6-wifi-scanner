#include <stdlib.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "node_config.h"
#include "net.h"
#include "universe.h"
#include "web_config.h"

static const char *TAG = "web";

#define MAX_BODY 2048

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static const char *const SRC_NAMES[] = { "none", "sacn", "artnet", "dmx" };

static esp_err_t send_json(httpd_req_t *req, char *json)
{
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* EMBED_TXTFILES adds a terminating NUL, which isn't sent. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t config_get(httpd_req_t *req)
{
    return send_json(req, node_config_to_json(&g_config));
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1-2048 bytes of JSON");
    }
    char *body = malloc(req->content_len);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    esp_err_t err = node_config_apply_json(body, got);
    free(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    } else if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    esp_netif_ip_info_t ip = net_ip_info();
    char addr[16];
    esp_ip4addr_ntoa(&ip.ip, addr, sizeof(addr));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", addr);
    cJSON_AddBoolToObject(root, "station", net_is_station());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON *univs = cJSON_AddArrayToObject(root, "universes");
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < universe_count(); i++) {
        univ_info_t info;
        universe_get_info(i, &info);
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "univ", info.number);
        cJSON_AddStringToObject(u, "src", SRC_NAMES[info.src]);
        cJSON_AddNumberToObject(u, "frames", info.frames);
        cJSON_AddNumberToObject(u, "age_ms", info.last_us ? (double)((now - info.last_us) / 1000) : -1);
        cJSON_AddNumberToObject(u, "priority", info.priority);
        cJSON_AddItemToArray(univs, u);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return send_json(req, json);
}

esp_err_t web_config_start(void)
{
    httpd_handle_t server;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    return ESP_OK;
}
