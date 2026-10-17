/*
    CSV scan log on a microSD card (SPI mode, FAT), plus serial commands to
    list, download and delete log files over the USB-Serial-JTAG console.

    Each boot starts a new file, SCAN0001.CSV, SCAN0002.CSV, ... with one row
    per access point per scan:

        uptime_s,scan,ssid,bssid,channel,rssi,auth,listed,lat,lon,alt_m,speed_kmh,course,fix,sats,hdop,utc

    The GPS columns are empty without a GPS module; with one but no fix, only
    fix (0), sats and utc are filled in.

    The ESP32-C6 has no USB OTG, so the card can't appear as a USB drive.
    Instead, tools/sdlog.py sends these commands (each on its own line):

        #LS             ->  #BEGIN LS / #F <name> <size> ... / #END LS
        #GET <name>     ->  #DATA <name> <size>\n <size raw bytes> #END <crc32 hex>
        #DEL <name>     ->  #OK DEL <name>
        errors          ->  #ERR <message>

    Logging is silenced during a download so log lines can't mix into the data.
*/
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "sdkconfig.h"
#include "ap_rows.h"
#include "wifi_scan.h"
#include "board_spi.h"
#include "sdlog.h"
#if CONFIG_SCANNER_GPS
#include "gps.h"
#endif
#if CONFIG_SCANNER_DISPLAY
#include "ui.h"
#endif
#if CONFIG_SCANNER_SD_DOWNLOAD
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

static const char *TAG = "sdlog";

#define MOUNT_POINT         "/sdcard"
#define RETRY_MOUNT_S       60
#define CSV_HEADER          "uptime_s,scan,ssid,bssid,channel,rssi,auth,listed," \
                            "lat,lon,alt_m,speed_kmh,course,fix,sats,hdop,utc\n"

static SemaphoreHandle_t s_lock;    /* guards the card, the open file and the counters */
static sdmmc_card_t *s_card;
static FILE *s_file;
static char s_name[16];             /* current log file, e.g. "SCAN0007.CSV" */
static uint32_t s_scan_num;
static int64_t s_next_mount_us;
static bool s_quiet_retries;


/* Write an SSID as a CSV field: quoted if it contains a comma, quote or line break. */
static void write_csv_field(FILE *f, const char *s)
{
    if (strpbrk(s, ",\"\r\n") == NULL) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') {
            fputc('"', f);
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

/* Next free SCANnnnn.CSV number. */
static int next_file_number(void)
{
    int max = 0;
    DIR *d = opendir(MOUNT_POINT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n;
            if (sscanf(e->d_name, "SCAN%4d.CSV", &n) == 1 && n > max) {
                max = n;
            }
        }
        closedir(d);
    }
    return max + 1;
}

static void unmount(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Log what's in a FAT boot sector (or why it isn't one). */
static void log_boot_sector(const char *where, const uint8_t *b)
{
    char oem[9], fstype[9];
    memcpy(oem, b + 3, 8);
    oem[8] = 0;
    memcpy(fstype, b + 82, 8);      /* FAT32 type string */
    fstype[8] = 0;
    ESP_LOGW(TAG, "  %s: OEM \"%s\", %u bytes/sector, %u sectors/cluster, type \"%s\", signature %02X%02X",
             where, oem, b[11] | (b[12] << 8), b[13], fstype, b[510], b[511]);
}

/* Mount failed with "no filesystem": read the card directly and log what's there. */
static void diagnose_card(sdmmc_host_t host, sdspi_device_config_t slot)
{
    sdspi_dev_handle_t handle;
    sdmmc_card_t card;
    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!buf || sdspi_host_init() != ESP_OK) {
        free(buf);
        return;
    }
    if (sdspi_host_init_device(&slot, &handle) != ESP_OK) {
        goto out;
    }
    host.slot = handle;
    if (sdmmc_card_init(&host, &card) != ESP_OK || sdmmc_read_sectors(&card, buf, 0, 1) != ESP_OK) {
        ESP_LOGW(TAG, "  Card diagnosis: can't read sector 0");
        sdspi_host_remove_device(handle);
        goto out;
    }
    ESP_LOGW(TAG, "  Card: %s, %llu MB, sector 0 signature %02X%02X", card.cid.name,
             (unsigned long long)card.csd.capacity * card.csd.sector_size / (1024 * 1024), buf[510], buf[511]);
    if (buf[0] == 0xEB || buf[0] == 0xE9) {
        log_boot_sector("No partition table; sector 0", buf);
    } else {
        uint32_t fat_lba = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = buf + 446 + 16 * i;
            if (e[4]) {
                ESP_LOGW(TAG, "  Partition %d: type 0x%02X (%s), start %lu, %lu MB", i + 1, e[4],
                         e[4] == 0x0B || e[4] == 0x0C ? "FAT32" : e[4] == 0x07 ? "exFAT/NTFS" :
                         e[4] == 0xEE ? "GPT - not supported" : e[4] == 0x06 || e[4] == 0x0E ? "FAT16" : "other",
                         (unsigned long)le32(e + 8), (unsigned long)(le32(e + 12) / 2048));
                if (!fat_lba && (e[4] == 0x0B || e[4] == 0x0C || e[4] == 0x07)) {
                    fat_lba = le32(e + 8);
                }
            }
        }
        if (fat_lba && sdmmc_read_sectors(&card, buf, fat_lba, 1) == ESP_OK) {
            log_boot_sector("First partition's boot sector", buf);
        }
    }
    sdspi_host_remove_device(handle);
out:
    sdspi_host_deinit();
    free(buf);
}

/* Mount the card and open a new log file. Call with s_lock held. */
static bool try_mount(void)
{
    if (s_quiet_retries) {
        /* No card at boot: don't flood the log with SD errors on every retry. */
        esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
        esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
        esp_log_level_set("sdspi_transaction", ESP_LOG_NONE);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
        esp_log_level_set("gpio", ESP_LOG_ERROR);     /* "conflict found for GPIO[cs]" on each retry */
    }
    if (board_spi_init() != ESP_OK) {
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = CONFIG_SCANNER_SD_SPI_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SPI_HOST;
    slot.gpio_cs = CONFIG_SCANNER_SD_PIN_CS;
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        if (!s_quiet_retries) {
            if (err == ESP_FAIL) {
                ESP_LOGW(TAG, "Card found but no usable FAT filesystem:");
                diagnose_card(host, slot);
            }
            ESP_LOGW(TAG, "No microSD card (%s); retrying every %d s", esp_err_to_name(err), RETRY_MOUNT_S);
            s_quiet_retries = true;
        }
        return false;
    }

    snprintf(s_name, sizeof(s_name), "SCAN%04d.CSV", next_file_number());
    char path[sizeof(MOUNT_POINT) + 16];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", s_name);
    s_file = fopen(path, "w");
    if (!s_file) {
        ESP_LOGE(TAG, "Can't create %s", path);
        unmount();
        return false;
    }
    fputs(CSV_HEADER, s_file);
    fflush(s_file);
    fsync(fileno(s_file));
    s_scan_num = 0;
    uint64_t mb = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024);
    ESP_LOGI(TAG, "Logging to %s on a %llu MB card", s_name, mb);
    return true;
}

bool sdlog_is_active(void)
{
    return s_file != NULL;
}

/* lat,lon,alt_m,speed_kmh,course,fix,sats,hdop,utc for this scan. */
static void gps_columns(char *buf, size_t len)
{
#if CONFIG_SCANNER_GPS
    gps_info_t g;
    if (!gps_get(&g)) {
        snprintf(buf, len, ",,,,,,,,");
        return;
    }
    char utc[48] = "";
    if (g.time_valid) {
        snprintf(utc, sizeof(utc), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 g.year, g.month, g.day, g.hour, g.minute, g.second);
    }
    if (g.valid) {
        char course[16] = "";
        if (g.course_valid) {
            snprintf(course, sizeof(course), "%.0f", g.course_deg);
        }
        snprintf(buf, len, "%.7f,%.7f,%.1f,%.1f,%s,%d,%d,%.1f,%s", g.lat, g.lon, g.alt_m, g.speed_kmh,
                 course, g.fix_type == 2 ? 2 : 3, g.sats_used, g.hdop, utc);
    } else {
        snprintf(buf, len, ",,,,,0,%d,,%s", g.sats_used, utc);
    }
#else
    snprintf(buf, len, ",,,,,,,,");
#endif
}

static int by_name_desc(const void *a, const void *b)
{
    return strcmp(((const sdlog_file_t *)b)->name, ((const sdlog_file_t *)a)->name);
}

int sdlog_list_files(sdlog_file_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_file) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
    DIR *d = s_card ? opendir(MOUNT_POINT) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            char path[sizeof(MOUNT_POINT) + 256];
            struct stat st;
            snprintf(path, sizeof(path), MOUNT_POINT "/%s", e->d_name);
            if (strlen(e->d_name) < sizeof(out[n].name) && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                strlcpy(out[n].name, e->d_name, sizeof(out[n].name));
                out[n].size = st.st_size;
                out[n].current = s_file && strcmp(e->d_name, s_name) == 0;
                n++;
            }
        }
        closedir(d);
    }
    xSemaphoreGive(s_lock);
    qsort(out, n, sizeof(out[0]), by_name_desc);
    return n;
}

esp_err_t sdlog_read_file(const char *name, void (*on_size)(long size, void *ctx),
                          bool (*on_data)(const void *data, size_t len, void *ctx), void *ctx)
{
    if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strlen(name) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(MOUNT_POINT) + 72];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_file && strcmp(name, s_name) == 0) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
    struct stat st;
    FILE *f = (s_card && stat(path, &st) == 0) ? fopen(path, "rb") : NULL;
    if (!f) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (on_size) {
        on_size((long)st.st_size, ctx);
    }
    static uint8_t buf[2048];
    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (!on_data(buf, n, ctx)) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    xSemaphoreGive(s_lock);
    return err;
}

void sdlog_scan(const wifi_ap_record_t *recs, int count)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    if (!s_file && now_us >= s_next_mount_us) {
        s_next_mount_us = now_us + (int64_t)RETRY_MOUNT_S * 1000000;
        try_mount();
    }
    if (s_file) {
        s_scan_num++;
        double t = now_us / 1e6;
        char gps[192];
        gps_columns(gps, sizeof(gps));
        for (int i = 0; i < count; i++) {
            const wifi_ap_record_t *ap = &recs[i];
            const uint8_t *m = ap->bssid;
            fprintf(s_file, "%.1f,%lu,", t, (unsigned long)s_scan_num);
            write_csv_field(s_file, (const char *)ap->ssid);
            fprintf(s_file, ",%02x:%02x:%02x:%02x:%02x:%02x,%d,%d,%s,%d,%s\n",
                    m[0], m[1], m[2], m[3], m[4], m[5], ap->primary, ap->rssi,
                    wifi_auth_name(ap->authmode), ap_rows_accepts(ap) ? 1 : 0, gps);
        }
        /* Sync every scan, so pulling the power loses at most one scan. */
        if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
            ESP_LOGW(TAG, "Write to %s failed; card removed? Will retry", s_name);
            unmount();
            s_quiet_retries = false;
            s_next_mount_us = now_us + (int64_t)RETRY_MOUNT_S * 1000000;
        }
    }
    xSemaphoreGive(s_lock);
}

#if CONFIG_SCANNER_SD_DOWNLOAD

static void send(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        int n = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(2000));
        if (n <= 0) {
            return;     /* host stopped reading */
        }
        p += n;
        len -= n;
    }
}

static void sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sendf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

/* Standard CRC-32 (same as zlib / Python's zlib.crc32). */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static bool valid_name(const char *name)
{
    return name[0] && strlen(name) < 64 && !strchr(name, '/') && !strchr(name, '\\') && strcmp(name, "..") != 0;
}

static void cmd_ls(void)
{
    if (s_file) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
    sendf("#BEGIN LS\n");
    DIR *d = s_card ? opendir(MOUNT_POINT) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char path[sizeof(MOUNT_POINT) + 256];
            struct stat st;
            snprintf(path, sizeof(path), MOUNT_POINT "/%s", e->d_name);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                sendf("#F %s %ld%s\n", e->d_name, (long)st.st_size,
                      (s_file && strcmp(e->d_name, s_name) == 0) ? " current" : "");
            }
        }
        closedir(d);
    }
    sendf("#END LS\n");
}

static void cmd_get(const char *name)
{
    char path[sizeof(MOUNT_POINT) + 256];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);
    if (s_file && strcmp(name, s_name) == 0) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
    struct stat st;
    FILE *f = (s_card && valid_name(name) && stat(path, &st) == 0) ? fopen(path, "rb") : NULL;
    if (!f) {
        sendf("#ERR no such file: %s\n", name);
        return;
    }

    /* Keep log lines out of the binary data. */
    esp_log_level_set("*", ESP_LOG_NONE);
    fflush(stdout);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));

    sendf("#DATA %s %ld\n", name, (long)st.st_size);
    static uint8_t buf[2048];
    uint32_t crc = 0;
    long left = st.st_size;
    while (left > 0) {
        size_t n = fread(buf, 1, left < (long)sizeof(buf) ? left : sizeof(buf), f);
        if (n == 0) {
            break;
        }
        crc = crc32_update(crc, buf, n);
        send(buf, n);
        left -= n;
    }
    fclose(f);
    sendf("#END %08lx\n", (unsigned long)crc);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
}

static void cmd_del(const char *name)
{
    char path[sizeof(MOUNT_POINT) + 256];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);
    if (s_file && strcmp(name, s_name) == 0) {
        sendf("#ERR %s is the file being written\n", name);
    } else if (!s_card || !valid_name(name) || unlink(path) != 0) {
        sendf("#ERR can't delete %s\n", name);
    } else {
        sendf("#OK DEL %s\n", name);
    }
}

static void handle_line(char *line)
{
#if CONFIG_SCANNER_USB_NMEA
    if (line[0] == '$') {
        /* NMEA from a computer (tools/nmea_usb.py): a GPS source like a module. */
        gps_feed(GPS_SRC_USB, line, strlen(line));
        gps_feed(GPS_SRC_USB, "\n", 1);
        return;
    }
#endif
    if (line[0] != '#') {
        return;
    }
#if CONFIG_SCANNER_DISPLAY
    if (strncmp(line, "#BTN ", 5) == 0) {
        /* Simulated button press: #BTN S (short), L (long), D (double). */
        ui_inject_button(line[5]);
        sendf("#OK BTN %c\n", line[5]);
        return;
    }
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_card) {
        sendf("#ERR no microSD card\n");
    } else if (strcmp(line, "#LS") == 0) {
        cmd_ls();
    } else if (strncmp(line, "#GET ", 5) == 0) {
        cmd_get(line + 5);
    } else if (strncmp(line, "#DEL ", 5) == 0) {
        cmd_del(line + 5);
    } else {
        sendf("#ERR unknown command\n");
    }
    xSemaphoreGive(s_lock);
}

static void cmd_task(void *arg)
{
    char line[96];
    int len = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) {
                handle_line(line);
            }
            len = 0;
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

static void start_commands(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 256,
    };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB serial driver install failed; downloads unavailable");
        return;
    }
    /* Route console output through the driver too, so it doesn't fight the driver for the port. */
    usb_serial_jtag_vfs_use_driver();
    xTaskCreate(cmd_task, "sdcmd", 4096, NULL, 3, NULL);
}

#endif /* CONFIG_SCANNER_SD_DOWNLOAD */

void sdlog_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    try_mount();
    s_next_mount_us = esp_timer_get_time() + (int64_t)RETRY_MOUNT_S * 1000000;
    xSemaphoreGive(s_lock);
#if CONFIG_SCANNER_SD_DOWNLOAD
    start_commands();
#endif
}
