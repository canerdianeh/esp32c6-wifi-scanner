/*
    Setup web page and JSON API:
      GET  /             the page (web/index.html)
      GET  /api/config   settings as JSON (see README.md)
      POST /api/config   change some or all settings; the node saves and restarts
      GET  /api/status   address, uptime and per-universe frame counts
*/
#pragma once

#include "esp_err.h"

esp_err_t web_config_start(void);
