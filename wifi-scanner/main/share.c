/*
    Share log files over Wi-Fi; see share.h.

    The access point uses WPA2 with a fresh random password each time, shown
    on the screen. The page is at http://192.168.4.1/.
*/
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "sdlog.h"
#include "share.h"

static const char *TAG = "share";

#define MAX_FILES 64

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static share_status_t s_status;
static char s_highlight[16];

static void html_escape_append(char *out, size_t len, const char *s)
{
    size_t o = strlen(out);
    for (; *s && o + 7 < len; s++) {
        const char *rep = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (rep) {
            strcpy(out + o, rep);
            o += strlen(rep);
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

static esp_err_t index_get(httpd_req_t *req)
{
    static sdlog_file_t files[MAX_FILES];
    int n = sdlog_list_files(files, MAX_FILES);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Wi-Fi scanner logs</title><style>"
        "body{font-family:system-ui,sans-serif;margin:16px;background:#111;color:#eee}"
        "a{color:#6cf;text-decoration:none}li{margin:10px 0;font-size:1.1em}"
        ".s{color:#999;font-size:.85em}.h{color:#fe6}"
        "</style></head><body><h2>Scan logs</h2><ul>");
    for (int pass = 0; pass < 2; pass++) {
        /* The file chosen on the scanner first, then the rest. */
        for (int i = 0; i < n; i++) {
            bool hl = strcmp(files[i].name, s_highlight) == 0;
            if (hl != (pass == 0)) {
                continue;
            }
            char line[256] = "<li><a href='/f/";
            html_escape_append(line, sizeof(line), files[i].name);
            strlcat(line, hl ? "' class=h download>" : "' download>", sizeof(line));
            html_escape_append(line, sizeof(line), files[i].name);
            char tail[64];
            snprintf(tail, sizeof(tail), "</a> <span class=s>%.1f KB%s</span></li>", files[i].size / 1024.0,
                     files[i].current ? ", recording" : "");
            strlcat(line, tail, sizeof(line));
            httpd_resp_sendstr_chunk(req, line);
        }
    }
    if (n == 0) {
        httpd_resp_sendstr_chunk(req, "<li class=s>No files (is a card inserted?)</li>");
    }
    httpd_resp_sendstr_chunk(req, "</ul><p class=s>Files are CSV: one row per access point per scan.</p>"
                                  "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

typedef struct {
    httpd_req_t *req;
    char disposition[80];
} download_t;

/* Called once the file is open: now it's safe to send download headers. */
static void on_size(long size, void *ctx)
{
    download_t *dl = ctx;
    httpd_resp_set_type(dl->req, "text/csv");
    httpd_resp_set_hdr(dl->req, "Content-Disposition", dl->disposition);
}

static bool on_data(const void *data, size_t len, void *ctx)
{
    return httpd_resp_send_chunk(((download_t *)ctx)->req, data, len) == ESP_OK;
}

static esp_err_t file_get(httpd_req_t *req)
{
    const char *name = req->uri + 3;    /* after "/f/" */
    download_t dl = { .req = req };
    snprintf(dl.disposition, sizeof(dl.disposition), "attachment; filename=\"%.40s\"", name);
    esp_err_t err = sdlog_read_file(name, on_size, on_data, &dl);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
    }
    if (err == ESP_OK) {
        s_status.downloads++;
        ESP_LOGI(TAG, "Sent %s", name);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

bool share_start(const char *highlight)
{
    strlcpy(s_highlight, highlight ? highlight : "", sizeof(s_highlight));
    memset(&s_status, 0, sizeof(s_status));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_status.ssid, sizeof(s_status.ssid), "WiFiScanner-%02X%02X", mac[4], mac[5]);
    snprintf(s_status.password, sizeof(s_status.password), "%08lu", (unsigned long)(esp_random() % 100000000));
    strlcpy(s_status.url, "http://192.168.4.1", sizeof(s_status.url));

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = {
        .ap = {
            .channel = 6,
            .max_connection = 3,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strlcpy((char *)ap.ap.ssid, s_status.ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_status.ssid);
    strlcpy((char *)ap.ap.password, s_status.password, sizeof(ap.ap.password));

    esp_wifi_stop();
    /* 20 MHz only: 40 MHz channels on 2.4 GHz overlap most of the band. */
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20) != ESP_OK || esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Access point failed to start");
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start");
        return false;
    }
    static const httpd_uri_t idx = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    static const httpd_uri_t file = { .uri = "/f/*", .method = HTTP_GET, .handler = file_get };
    httpd_register_uri_handler(s_server, &idx);
    httpd_register_uri_handler(s_server, &file);
    ESP_LOGI(TAG, "Sharing: join \"%s\" (password %s), open %s", s_status.ssid, s_status.password, s_status.url);
    return true;
}

void share_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    ESP_LOGI(TAG, "Sharing stopped");
}

void share_get_status(share_status_t *st)
{
    wifi_sta_list_t list;
    if (s_server && esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        s_status.clients = list.num;
    }
    *st = s_status;
}
