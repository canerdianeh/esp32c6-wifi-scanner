/*
    GPS module on a UART; see gps.h.
*/
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "gps.h"
#if 0    /* no display on the responder */
#include "gfx.h"
#endif

static const char *TAG = "gps";

#define GPS_UART        CONFIG_RESPONDER_GPS_UART_NUM
#define PRESENT_US      3000000     /* no valid NMEA for this long = module gone */
#define AUTOBAUD_US     2500000     /* try the next baud rate after this long without NMEA */
#define MAX_FIELDS      24

static const uint32_t s_bauds[] = { 9600, 38400, 115200, 57600, 4800, 230400 };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gps_info_t s_info;           /* guarded by s_mux */
static int64_t s_last_nmea_us;
static int64_t s_last_epoch_us;
static char s_last_epoch_time[12];
static uint8_t s_gsv_count[8];      /* satellites in view per talker */
static int64_t s_gsv_us[8];
static bool s_clock_set;

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1));
}

/* ddmm.mmmm + hemisphere -> signed degrees. */
static double parse_coord(const char *v, const char *hemi)
{
    if (!v[0]) {
        return 0;
    }
    double raw = atof(v);
    int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    return (hemi[0] == 'S' || hemi[0] == 'W') ? -d : d;
}

static int talker_index(const char *addr)
{
    switch (addr[1]) {          /* GP, GL, GA, GB/BD, GQ, GN ... */
    case 'P': return 0;
    case 'L': return 1;
    case 'A': return 2;
    case 'B': case 'D': return 3;
    case 'Q': return 4;
    case 'I': return 5;
    default:  return 6;
    }
}

static void note_epoch(const char *time_field)
{
    /* A new position time = one update. Rate = smoothed 1/interval. */
    if (!time_field[0] || strcmp(time_field, s_last_epoch_time) == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_epoch_us) {
        float dt = (now - s_last_epoch_us) / 1e6f;
        if (dt > 0.02f && dt < 5.0f) {
            float hz = 1.0f / dt;
            s_info.rate_hz = s_info.rate_hz > 0 ? 0.8f * s_info.rate_hz + 0.2f * hz : hz;
        }
    }
    s_last_epoch_us = now;
    strlcpy(s_last_epoch_time, time_field, sizeof(s_last_epoch_time));
}

static void set_clock_once(void)
{
    if (s_clock_set || !s_info.time_valid || s_info.year < 2020) {
        return;
    }
    struct tm t = {
        .tm_year = s_info.year - 1900, .tm_mon = s_info.month - 1, .tm_mday = s_info.day,
        .tm_hour = s_info.hour, .tm_min = s_info.minute, .tm_sec = s_info.second,
    };
    setenv("TZ", "UTC0", 1);
    tzset();
    struct timeval tv = { .tv_sec = mktime(&t) };
    settimeofday(&tv, NULL);
    s_clock_set = true;
    ESP_LOGI(TAG, "Clock set from GPS: %04d-%02d-%02d %02d:%02d:%02d UTC",
             s_info.year, s_info.month, s_info.day, s_info.hour, s_info.minute, s_info.second);
}

static void parse_sentence(char *line)
{
    /* $ADDR,f1,f2,...*CS */
    char *star = strrchr(line, '*');
    if (line[0] != '$' || !star || strlen(star) < 3) {
        return;
    }
    uint8_t sum = 0;
    for (char *p = line + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    int hi = hexval(star[1]), lo = hexval(star[2]);
    if (hi < 0 || lo < 0 || sum != ((hi << 4) | lo)) {
        return;
    }
    *star = 0;

    char *f[MAX_FIELDS];
    int n = 0;
    char *p = line + 1;
    f[n++] = p;
    while (*p && n < MAX_FIELDS) {
        if (*p == ',') {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }
    for (int i = n; i < MAX_FIELDS; i++) {
        f[i] = "";
    }
    if (strlen(f[0]) != 5) {
        return;
    }
    const char *type = f[0] + 2;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    s_last_nmea_us = now;
    if (strcmp(type, "GGA") == 0) {
        int q = atoi(f[6]);
        s_info.quality = q;
        s_info.sats_used = atoi(f[7]);
        s_info.hdop = f[8][0] ? atof(f[8]) : 0;
        if (q > 0) {
            s_info.lat = parse_coord(f[2], f[3]);
            s_info.lon = parse_coord(f[4], f[5]);
            s_info.alt_m = atof(f[9]);
        }
        s_info.valid = q > 0;
        note_epoch(f[1]);
    } else if (strcmp(type, "RMC") == 0) {
        bool ok = f[2][0] == 'A';
        if (ok) {
            s_info.lat = parse_coord(f[3], f[4]);
            s_info.lon = parse_coord(f[5], f[6]);
            s_info.speed_kmh = atof(f[7]) * 1.852f;
            s_info.course_valid = f[8][0] != 0;
            s_info.course_deg = atof(f[8]);
        }
        if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
            s_info.hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
            s_info.minute = (f[1][2] - '0') * 10 + (f[1][3] - '0');
            s_info.second = (f[1][4] - '0') * 10 + (f[1][5] - '0');
            s_info.day = (f[9][0] - '0') * 10 + (f[9][1] - '0');
            s_info.month = (f[9][2] - '0') * 10 + (f[9][3] - '0');
            s_info.year = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
            s_info.time_valid = true;
        }
        if (!s_info.quality) {
            s_info.valid = ok;      /* modules that send no GGA */
            note_epoch(f[1]);
        }
    } else if (strcmp(type, "GSA") == 0) {
        s_info.fix_type = atoi(f[2]);
        if (f[16][0]) {
            s_info.hdop = atof(f[16]);
        }
    } else if (strcmp(type, "GSV") == 0) {
        int t = talker_index(f[0]);
        s_gsv_count[t] = atoi(f[3]);
        s_gsv_us[t] = now;
        int total = 0;
        for (int i = 0; i < 8; i++) {
            if (now - s_gsv_us[i] < PRESENT_US) {
                total += s_gsv_count[i];
            }
        }
        s_info.sats_view = total;
    } else if (strcmp(type, "VTG") == 0) {
        if (f[1][0]) {
            s_info.course_valid = true;
            s_info.course_deg = atof(f[1]);
        }
        if (f[7][0]) {
            s_info.speed_kmh = atof(f[7]);
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (strcmp(type, "RMC") == 0) {
        set_clock_once();
    }
}

static void gps_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_RESPONDER_GPS_BAUD ? CONFIG_RESPONDER_GPS_BAUD : (int)s_bauds[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, CONFIG_RESPONDER_GPS_PIN_TX, CONFIG_RESPONDER_GPS_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_info.baud = cfg.baud_rate;

    char line[100];
    int len = 0;
    int baud_idx = 0;
    int64_t baud_since = esp_timer_get_time();
    bool was_present = false;
    uint8_t buf[128];

    for (;;) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') {
                len = 0;
            }
            if (c == '\r' || c == '\n') {
                if (len > 6) {
                    line[len] = 0;
                    parse_sentence(line);
                }
                len = 0;
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = c;
            }
        }

        int64_t now = esp_timer_get_time();
        bool present = s_last_nmea_us && now - s_last_nmea_us < PRESENT_US;
        portENTER_CRITICAL(&s_mux);
        s_info.present = present;
        if (!present) {
            s_info.valid = false;
            s_info.rate_hz = 0;
        }
        portEXIT_CRITICAL(&s_mux);
        if (present != was_present) {
            if (present) {
                ESP_LOGI(TAG, "GPS module found at %lu baud", (unsigned long)s_info.baud);
            } else {
                ESP_LOGI(TAG, "GPS module lost");
            }
            was_present = present;
        }
        /* No NMEA: try the next baud rate (unless fixed in menuconfig). */
        if (!present && !CONFIG_RESPONDER_GPS_BAUD && now - baud_since > AUTOBAUD_US) {
            baud_idx = (baud_idx + 1) % (int)(sizeof(s_bauds) / sizeof(s_bauds[0]));
            uart_set_baudrate(GPS_UART, s_bauds[baud_idx]);
            uart_flush_input(GPS_UART);
            s_info.baud = s_bauds[baud_idx];
            baud_since = now;
            len = 0;
        }
        if (present) {
            baud_since = now;
        }
    }
}

void gps_init(void)
{
    xTaskCreate(gps_task, "gps", 4096, NULL, 4, NULL);
}

bool gps_get(gps_info_t *info)
{
    portENTER_CRITICAL(&s_mux);
    *info = s_info;
    portEXIT_CRITICAL(&s_mux);
    return info->present;
}

float gps_accuracy_m(const gps_info_t *info)
{
    /* HDOP x ~2.5 m (typical receiver error) is a rough 1-sigma horizontal estimate. */
    return info->hdop > 0 ? info->hdop * 2.5f : 99.0f;
}

uint16_t gps_fix_color(const gps_info_t *info)
{
#if 0    /* no display on the responder */
    if (!info->valid) {
        return C_GRAY;
    }
    float acc = gps_accuracy_m(info);
    if (info->fix_type == 2 || acc > 15) {
        return C_RED;
    }
    if (info->quality == 2 || info->quality == 4 || info->quality == 5) {
        return C_CYAN;          /* differential / RTK */
    }
    if (acc > 7) {
        return C_ORANGE;
    }
    if (acc > 3) {
        return C_YELLOW;
    }
    return C_GREEN;
#else
    return 0;
#endif
}
