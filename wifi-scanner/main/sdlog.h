/*
    CSV scan log on a microSD card, plus serial commands to list, download and
    delete log files over the USB cable (see tools/sdlog.py).
*/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/* Mount the card (if present), start a new log file, and start the command handler. */
void sdlog_init(void);

/* Append one scan. `recs` sorted strongest first. Retries mounting now and then if no card. */
void sdlog_scan(const wifi_ap_record_t *recs, int count);

/* A card is mounted and the log file is open. */
bool sdlog_is_active(void);

typedef struct {
    char name[16];
    long size;
    bool current;           /* the file being written now */
} sdlog_file_t;

/* Files on the card, newest name first. Returns the count. */
int sdlog_list_files(sdlog_file_t *out, int max);

/* Stream a file: on_size once, then on_data per chunk (return false to stop).
 * Holds the card for the whole transfer. */
esp_err_t sdlog_read_file(const char *name, void (*on_size)(long size, void *ctx),
                          bool (*on_data)(const void *data, size_t len, void *ctx), void *ctx);
