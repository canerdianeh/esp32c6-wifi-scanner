/*
    GPS from NMEA (module on a UART, phone, or demo simulation); see gps.h.
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "gps.h"
#if CONFIG_SCANNER_DISPLAY
#include "gfx.h"
#endif

static const char *TAG = "gps";

#define GPS_UART        CONFIG_SCANNER_GPS_UART_NUM
#define PRESENT_US      3000000     /* no valid NMEA for this long = module gone */
#define AUTOBAUD_US     2500000     /* try the next baud rate after this long without NMEA */
#define MAX_FIELDS      24
#define SYS_SATS        20          /* satellites kept per system */
#define MAX_USED        48

static const uint32_t s_bauds[] = { 9600, 38400, 115200, 57600, 4800, 230400 };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gps_info_t s_info;           /* guarded by s_mux */
static int64_t s_last_nmea_us;
static int64_t s_last_epoch_us;
static char s_last_epoch_time[12];
static uint8_t s_gsv_count[8];      /* satellites in view per talker */
static int64_t s_gsv_us[8];
static bool s_clock_set;
static int64_t s_src_us[GPS_SRC_COUNT];     /* last sentence per source (accepted or not) */

/* Satellites per system: GSV messages fill `work`, the last one of a group commits it. Guarded by s_mux. */
static gps_sat_t s_sys_sats[GPS_SYS_COUNT][SYS_SATS];
static uint8_t s_sys_n[GPS_SYS_COUNT];
static gps_sat_t s_work[GPS_SYS_COUNT][SYS_SATS];
static uint8_t s_work_n[GPS_SYS_COUNT];
/* Satellites in the fix (GSA), cleared at each new epoch. */
static struct { uint8_t system; uint16_t prn; } s_used[MAX_USED];
static int s_num_used;
static bool s_used_stale;

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
    s_used_stale = true;
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

/* Parse one sentence. True if it was valid and used (from the best current source). */
static bool parse_sentence(char *line, gps_source_t src)
{
    /* $ADDR,f1,f2,...*CS */
    char *star = strrchr(line, '*');
    if (line[0] != '$' || !star || strlen(star) < 3) {
        return false;
    }
    uint8_t sum = 0;
    for (char *p = line + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    int hi = hexval(star[1]), lo = hexval(star[2]);
    if (hi < 0 || lo < 0 || sum != ((hi << 4) | lo)) {
        return false;
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
        return false;
    }
    const char *type = f[0] + 2;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    /* Only the best source that is currently sending is used: module, then phone, then simulation. */
    s_src_us[src] = now;
    for (int h = GPS_SRC_MODULE; h < (int)src; h++) {
        if (s_src_us[h] && now - s_src_us[h] < PRESENT_US) {
            portEXIT_CRITICAL(&s_mux);
            return false;
        }
    }
    if (s_info.source != src) {
        /* New source: forget the old one's satellites and fix details. */
        memset(s_sys_n, 0, sizeof(s_sys_n));
        memset(s_work_n, 0, sizeof(s_work_n));
        memset(s_gsv_us, 0, sizeof(s_gsv_us));
        s_num_used = 0;
        s_info.sats_view = s_info.sats_used = 0;
        s_info.fix_type = 0;
        s_info.rate_hz = 0;
        s_last_epoch_time[0] = 0;
        s_last_epoch_us = 0;
        s_info.source = src;
    }
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
        /* Used PRNs in f[3..14]. NMEA 4.10+ adds the system ID in f[18]; otherwise use the talker. */
        int sysid = atoi(f[18]);
        int sys = sysid >= 1 && sysid <= 6 ? sysid - 1 : talker_index(f[0]);
        if (s_used_stale) {
            s_num_used = 0;
            s_used_stale = false;
        }
        for (int i = 3; i <= 14 && s_num_used < MAX_USED; i++) {
            if (f[i][0]) {
                s_used[s_num_used].system = sys;
                s_used[s_num_used].prn = atoi(f[i]);
                s_num_used++;
            }
        }
    } else if (strcmp(type, "GSV") == 0) {
        int t = talker_index(f[0]);
        s_gsv_count[t] = atoi(f[3]);
        s_gsv_us[t] = now;
        int total_msgs = atoi(f[1]), msg = atoi(f[2]);
        if (msg == 1) {
            s_work_n[t] = 0;
        }
        /* Groups of PRN, elevation, azimuth, SNR (a trailing signal ID is not a full group). */
        for (int k = 4; k + 3 < n && s_work_n[t] < SYS_SATS; k += 4) {
            if (!f[k][0]) {
                continue;
            }
            gps_sat_t *sat = &s_work[t][s_work_n[t]++];
            sat->system = t;
            sat->prn = atoi(f[k]);
            sat->elev = f[k + 1][0] ? atoi(f[k + 1]) : -1;
            sat->az = f[k + 2][0] ? atoi(f[k + 2]) : -1;
            sat->snr = f[k + 3][0] ? atoi(f[k + 3]) : 0;
            sat->used = false;
        }
        if (msg == total_msgs) {
            memcpy(s_sys_sats[t], s_work[t], s_work_n[t] * sizeof(gps_sat_t));
            s_sys_n[t] = s_work_n[t];
        }
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
    return true;
}

/* ---- line assembly, per source ---- */

static char s_lines[GPS_SRC_COUNT][100];
static int s_line_len[GPS_SRC_COUNT];
static void (*s_listener)(const char *sentence, gps_source_t src);

void gps_set_listener(void (*cb)(const char *sentence, gps_source_t src))
{
    s_listener = cb;
}

void gps_feed(gps_source_t src, const char *data, size_t n)
{
    if (src <= GPS_SRC_NONE || src >= GPS_SRC_COUNT) {
        return;
    }
    char *line = s_lines[src];
    int len = s_line_len[src];
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '$') {
            len = 0;
        }
        if (c == '\r' || c == '\n') {
            if (len > 6) {
                line[len] = 0;
                char copy[sizeof(s_lines[0])];
                memcpy(copy, line, len + 1);    /* parsing splits the line in place */
                if (parse_sentence(line, src) && s_listener) {
                    s_listener(copy, src);
                }
            }
            len = 0;
        } else if (len < (int)sizeof(s_lines[0]) - 1) {
            line[len++] = c;
        }
    }
    s_line_len[src] = len;
}

#if CONFIG_SCANNER_DEMO_GPS
/* ---- demo: a simulated multi-GNSS receiver, walking a loop next to Wrigley Field ---- */

#define SIM_LAT     41.947750
#define SIM_LON     -87.655900
#define SIM_R_M     40.0        /* loop radius */
#define SIM_LOOP_S  180.0       /* one loop: ~5 km/h */

typedef struct {
    char talker;                /* P, L, A, B */
    uint8_t sysid;              /* NMEA 4.10 system ID */
    uint8_t prn;
    float az, el;               /* at boot */
    uint8_t snr;                /* at the zenith */
} sim_sat_t;

static const sim_sat_t s_sim_sats[] = {
    { 'P', 1, 2, 48, 71, 46 },   { 'P', 1, 5, 151, 38, 42 },  { 'P', 1, 12, 296, 55, 44 },
    { 'P', 1, 15, 219, 22, 37 }, { 'P', 1, 18, 82, 14, 33 },  { 'P', 1, 25, 3, 31, 40 },
    { 'P', 1, 29, 263, 8, 28 },  { 'P', 1, 31, 118, 63, 45 }, { 'L', 2, 67, 330, 42, 41 },
    { 'L', 2, 68, 25, 12, 30 },  { 'L', 2, 77, 196, 58, 43 }, { 'A', 3, 4, 101, 47, 44 },
    { 'A', 3, 9, 238, 66, 46 },  { 'A', 3, 24, 316, 19, 34 }, { 'B', 4, 22, 168, 74, 45 },
    { 'B', 4, 35, 58, 27, 38 },
};
#define SIM_NUM ((int)(sizeof(s_sim_sats) / sizeof(s_sim_sats[0])))

static void sim_send(const char *body)
{
    uint8_t sum = 0;
    for (const char *p = body; *p; p++) {
        sum ^= (uint8_t)*p;
    }
    char line[100];
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
    gps_feed(GPS_SRC_SIM, line, n);
}

static void sim_coord(char *out, size_t len, double deg, bool is_lat)
{
    double a = fabs(deg);
    int d = (int)a;
    double m = (a - d) * 60.0;
    snprintf(out, len, is_lat ? "%02d%08.5f,%c" : "%03d%08.5f,%c", d, m,
             is_lat ? (deg >= 0 ? 'N' : 'S') : (deg >= 0 ? 'E' : 'W'));
}

static void sim_tick(void)
{
    static time_t base;
    static int64_t base_us;
    int64_t now_us = esp_timer_get_time();
    if (!base) {
        time_t sys = time(NULL);
        base = sys > 1700000000 ? sys : (time_t)SCANNER_BUILD_EPOCH;    /* UTC, from CMake */
        base_us = now_us;
    }
    double t = (now_us - base_us) / 1e6;
    time_t tt = base + (time_t)t;
    struct tm u;
    gmtime_r(&tt, &u);

    double th = 2 * M_PI * fmod(t, SIM_LOOP_S) / SIM_LOOP_S;
    double lat = SIM_LAT + SIM_R_M * cos(th) / 111320.0;
    double lon = SIM_LON + SIM_R_M * sin(th) / (111320.0 * cos(SIM_LAT * M_PI / 180));
    double course = fmod(th * 180 / M_PI + 90, 360);
    double knots = (2 * M_PI * SIM_R_M / SIM_LOOP_S) * 1.94384 * (0.95 + 0.1 * sin(t / 7));
    float alt = 181.0f + 0.6f * sinf(t / 11);
    float hdop = 0.8f + 0.1f * sinf(t / 13);

    /* Satellites drift slowly across the sky; SNR follows elevation with a little jitter. */
    int used[SIM_NUM], el[SIM_NUM], az[SIM_NUM], snr[SIM_NUM], nused = 0;
    for (int i = 0; i < SIM_NUM; i++) {
        const sim_sat_t *s = &s_sim_sats[i];
        az[i] = (int)fmodf(s->az + t * 0.02f, 360);
        el[i] = (int)(s->el + 2.0f * sinf(t / 90 + s->prn));
        snr[i] = s->snr - (90 - el[i]) / 9 + (int)(esp_random() % 3);
        used[i] = el[i] >= 10 && snr[i] >= 28;
        nused += used[i];
    }

    char body[96], la[20], lo[20];
    sim_coord(la, sizeof(la), lat, true);
    sim_coord(lo, sizeof(lo), lon, false);
    snprintf(body, sizeof(body), "GNGGA,%02d%02d%02d.00,%s,%s,1,%02d,%.1f,%.1f,M,-34.0,M,,",
             u.tm_hour, u.tm_min, u.tm_sec, la, lo, nused, hdop, alt);
    sim_send(body);
    snprintf(body, sizeof(body), "GNRMC,%02d%02d%02d.00,A,%s,%s,%.2f,%.1f,%02d%02d%02d,,,A",
             u.tm_hour, u.tm_min, u.tm_sec, la, lo, knots, course, u.tm_mday, u.tm_mon + 1, u.tm_year % 100);
    sim_send(body);

    static const char talkers[] = { 'P', 'L', 'A', 'B' };
    for (int k = 0; k < 4; k++) {
        /* GSA (used PRNs, with the system ID) and GSV (4 satellites per message) per system. */
        int idx[SIM_NUM], n = 0;
        for (int i = 0; i < SIM_NUM; i++) {
            if (s_sim_sats[i].talker == talkers[k]) {
                idx[n++] = i;
            }
        }
        int len = snprintf(body, sizeof(body), "GNGSA,A,3");
        int listed = 0;
        for (int j = 0; j < n; j++) {
            if (used[idx[j]] && listed < 12) {
                len += snprintf(body + len, sizeof(body) - len, ",%d", s_sim_sats[idx[j]].prn);
                listed++;
            }
        }
        for (; listed < 12; listed++) {
            len += snprintf(body + len, sizeof(body) - len, ",");
        }
        snprintf(body + len, sizeof(body) - len, ",%.1f,%.1f,%.1f,%d", hdop * 1.6f, hdop, hdop * 1.3f,
                 s_sim_sats[idx[0]].sysid);
        sim_send(body);

        int msgs = (n + 3) / 4;
        for (int m = 0; m < msgs; m++) {
            len = snprintf(body, sizeof(body), "G%cGSV,%d,%d,%02d", talkers[k], msgs, m + 1, n);
            for (int j = m * 4; j < n && j < m * 4 + 4; j++) {
                int i = idx[j];
                len += snprintf(body + len, sizeof(body) - len, ",%02d,%02d,%03d,%02d", s_sim_sats[i].prn,
                                el[i], az[i], snr[i]);
            }
            sim_send(body);
        }
    }
}
#endif

static const char *source_name(gps_source_t src)
{
    switch (src) {
    case GPS_SRC_MODULE: return "module";
    case GPS_SRC_USB:    return "USB";
    case GPS_SRC_PHONE:  return "phone";
    case GPS_SRC_SIM:    return "simulation (demo)";
    default:             return "none";
    }
}

static void gps_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_SCANNER_GPS_BAUD ? CONFIG_SCANNER_GPS_BAUD : (int)s_bauds[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, CONFIG_SCANNER_GPS_PIN_TX, CONFIG_SCANNER_GPS_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_info.baud = cfg.baud_rate;

    int baud_idx = 0;
    int64_t baud_since = esp_timer_get_time();
    gps_source_t was_src = GPS_SRC_NONE;
    uint8_t buf[128];
#if CONFIG_SCANNER_DEMO_GPS
    int64_t sim_next_us = 0;
#endif

    for (;;) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            gps_feed(GPS_SRC_MODULE, (const char *)buf, n);
        }

        int64_t now = esp_timer_get_time();
#if CONFIG_SCANNER_DEMO_GPS
        if (now >= sim_next_us) {
            sim_next_us = now + 1000000;
            sim_tick();
        }
#endif
        bool present = s_last_nmea_us && now - s_last_nmea_us < PRESENT_US;
        bool module = s_src_us[GPS_SRC_MODULE] && now - s_src_us[GPS_SRC_MODULE] < PRESENT_US;
        portENTER_CRITICAL(&s_mux);
        s_info.present = present;
        if (!present) {
            s_info.valid = false;
            s_info.rate_hz = 0;
        }
        gps_source_t src = present ? s_info.source : GPS_SRC_NONE;
        portEXIT_CRITICAL(&s_mux);
        if (src != was_src) {
            if (src == GPS_SRC_MODULE) {
                ESP_LOGI(TAG, "GPS data from module at %lu baud", (unsigned long)s_info.baud);
            } else if (src != GPS_SRC_NONE) {
                ESP_LOGI(TAG, "GPS data from %s", source_name(src));
            } else {
                ESP_LOGI(TAG, "GPS data lost");
            }
            was_src = src;
        }
        /* No module: try the next baud rate (unless fixed in menuconfig). */
        if (!module && !CONFIG_SCANNER_GPS_BAUD && now - baud_since > AUTOBAUD_US) {
            baud_idx = (baud_idx + 1) % (int)(sizeof(s_bauds) / sizeof(s_bauds[0]));
            uart_set_baudrate(GPS_UART, s_bauds[baud_idx]);
            uart_flush_input(GPS_UART);
            s_info.baud = s_bauds[baud_idx];
            baud_since = now;
            s_line_len[GPS_SRC_MODULE] = 0;
        }
        if (module) {
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

int gps_get_sats(gps_sat_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    for (int t = 0; t < GPS_SYS_COUNT; t++) {
        if (!s_gsv_us[t] || now - s_gsv_us[t] >= PRESENT_US) {
            continue;
        }
        for (int i = 0; i < s_sys_n[t] && n < max; i++) {
            gps_sat_t sat = s_sys_sats[t][i];
            for (int u = 0; u < s_num_used; u++) {
                /* A GN-talker GSA without a system ID can't say which system; match on PRN. */
                if (s_used[u].prn == sat.prn && (s_used[u].system == t || s_used[u].system == GPS_SYS_OTHER)) {
                    sat.used = true;
                    break;
                }
            }
            out[n++] = sat;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

const char *gps_source_label(gps_source_t src)
{
    return src == GPS_SRC_USB ? "USB" : src == GPS_SRC_PHONE ? "phone" : NULL;
}

float gps_accuracy_m(const gps_info_t *info)
{
    /* HDOP x ~2.5 m (typical receiver error) is a rough 1-sigma horizontal estimate. */
    return info->hdop > 0 ? info->hdop * 2.5f : 99.0f;
}

uint16_t gps_fix_color(const gps_info_t *info)
{
#if CONFIG_SCANNER_DISPLAY
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
