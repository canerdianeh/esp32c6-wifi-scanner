/*
    Screen modes, the mode menu and the BOOT button; see ui.h.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "gfx.h"
#include "ui.h"
#if CONFIG_SCANNER_GPS
#include "gps.h"
#endif
#if CONFIG_SCANNER_SD_LOG
#include "sdlog.h"
#endif

static const char *TAG = "ui";

typedef enum { EV_REDRAW, EV_SHORT, EV_LONG, EV_DOUBLE, EV_DEMO_PICK } ui_event_t;

#define BUTTON_POLL_MS      20
#define BUTTON_DEBOUNCE_MS  40
#define DOUBLE_GAP_MS       350
#define MENU_TIMEOUT_MS     15000
#define MENU_ROW_H          44

static QueueHandle_t s_events;
static const ui_screen_t *s_screen;     /* current screen (the menu is drawn over it) */
static volatile bool s_want_double;
static bool s_menu_open;
static int s_menu_sel;
static int64_t s_menu_last_us;
static volatile int32_t s_passkey = -1;    /* Bluetooth pairing passkey on screen; -1 = none */
static int64_t s_passkey_us;
#define PASSKEY_TIMEOUT_MS  60000

/* Modes offered in the menu, in order. */
static const ui_screen_t *const s_modes[] = {
    &ui_screen_wifi,
#if CONFIG_SCANNER_GPS
    &ui_screen_gps,
#endif
#if CONFIG_SCANNER_MODE_BLE
    &ui_screen_ble,
#endif
#if CONFIG_SCANNER_MODE_BAND
    &ui_screen_band,
#endif
#if CONFIG_SCANNER_MODE_ZIGBEE
    &ui_screen_zigbee,
#endif
#if CONFIG_SCANNER_MODE_FILES
    &ui_screen_files,
#endif
};
#define NUM_MODES ((int)(sizeof(s_modes) / sizeof(s_modes[0])))

void ui_request_redraw(void)
{
    if (s_events) {
        ui_event_t ev = EV_REDRAW;
        xQueueSend(s_events, &ev, 0);
    }
}

void ui_show_passkey(int32_t passkey)
{
    s_passkey_us = esp_timer_get_time();
    s_passkey = passkey;
    ui_request_redraw();
}

void ui_show(const ui_screen_t *screen)
{
    if (screen == s_screen) {
        return;
    }
    if (s_screen && s_screen->leave) {
        s_screen->leave();
    }
    s_screen = screen;
    s_want_double = screen->wants_double;
    ESP_LOGI(TAG, "Mode: %s", screen->name);
    if (screen->enter) {
        screen->enter();
    }
}

void ui_body_message(const char *line1, const char *line2)
{
    int y = UI_BODY_Y + UI_BODY_H / 2 - GFX_FONT_H - 4;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    gfx_band_begin(2 * GFX_FONT_H + 8, C_BLACK);
    gfx_text(MARGIN, 0, line1, LCD_W / GFX_FONT_W, C_GRAY);
    if (line2) {
        gfx_text(MARGIN, GFX_FONT_H + 8, line2, LCD_W / GFX_FONT_W, C_DIM_DOT);
    }
    gfx_band_flush(y);
    gfx_clear_rows(y + 2 * GFX_FONT_H + 8, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

static void draw_header(void)
{
    char title[48] = "", right[24] = "";
    uint16_t title_color = C_WHITE;
    if (s_passkey >= 0) {
        strlcpy(title, "Bluetooth pairing", sizeof(title));
        title_color = C_CYAN;
    } else if (s_menu_open) {
        strlcpy(title, "Modes", sizeof(title));
    } else if (s_screen->header) {
        s_screen->header(title, sizeof(title), &title_color, right, sizeof(right));
    }
    gfx_band_begin(UI_HEADER_H, C_BLACK);
    int right_chars = right[0] ? (int)strlen(right) + 1 : 0;
    gfx_text_right(LCD_W - MARGIN, 3, right, C_GRAY);
    gfx_text(MARGIN, 3, title, (LCD_W - 2 * MARGIN) / GFX_FONT_W - right_chars, title_color);
    gfx_fill_rect(0, UI_HEADER_H - 2, LCD_W, 1, C_LINE);
    gfx_band_flush(0);
}

static void draw_footer(void)
{
    gfx_band_begin(UI_FOOTER_H, C_BLACK);
    gfx_fill_rect(0, 0, LCD_W, 1, C_LINE);
#if CONFIG_SCANNER_GPS
    gps_info_t g;
    if (gps_get(&g)) {
        char buf[24];
        uint16_t c = gps_fix_color(&g);
        gfx_icon(MARGIN, 4, &icon_globe_14, c);
        if (g.valid && gps_source_label(g.source)) {
            snprintf(buf, sizeof(buf), "%s ~%.0fm", gps_source_label(g.source), gps_accuracy_m(&g));
        } else if (g.valid) {
            snprintf(buf, sizeof(buf), "%s %dsat ~%.0fm", g.fix_type == 2 ? "2D" : "3D", g.sats_used,
                     gps_accuracy_m(&g));
        } else {
            snprintf(buf, sizeof(buf), "no fix %dsat", g.sats_view);
        }
        gfx_text(MARGIN + 18, 3, buf, 16, c == C_GRAY ? C_GRAY : C_WHITE);
    }
#endif
#if CONFIG_SCANNER_SD_LOG
    if (sdlog_is_active()) {
        gfx_icon(LCD_W - MARGIN - 14, 4, &icon_sdcard_14, C_GREEN);
    }
#endif
    gfx_band_flush(LCD_H - UI_FOOTER_H);
}

static void draw_menu_body(void)
{
    int y = UI_BODY_Y + 4;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    for (int i = 0; i < NUM_MODES; i++, y += MENU_ROW_H) {
        bool sel = i == s_menu_sel;
        gfx_band_begin(MENU_ROW_H, sel ? C_SEL_BG : C_BLACK);
        gfx_icon(10, (MENU_ROW_H - 24) / 2, s_modes[i]->icon, sel ? C_ACCENT : C_WHITE);
        gfx_text(44, (MENU_ROW_H - GFX_FONT_H) / 2, s_modes[i]->name, 15, sel ? C_ACCENT : C_WHITE);
        if (s_modes[i] == s_screen) {
            gfx_dot(LCD_W - 12, (MENU_ROW_H - 5) / 2, C_GREEN);     /* current mode */
        }
        gfx_band_flush(y);
    }
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

/* Passkey to type on the phone, in large digits. */
static void draw_passkey_body(int32_t pk)
{
    char code[16];
    snprintf(code, sizeof(code), "%03ld %03ld", (long)(pk / 1000), (long)(pk % 1000));
    int y = UI_BODY_Y + 40;
    gfx_clear_rows(UI_BODY_Y, y, C_BLACK);
    gfx_band_begin(GFX_FONT_H + 8, C_BLACK);
    gfx_text(MARGIN, 0, "Enter on the phone:", 21, C_GRAY);
    gfx_band_flush(y);
    y += GFX_FONT_H + 8;
    gfx_band_begin(2 * GFX_FONT_H + 8, C_BLACK);
    gfx_text_scaled((LCD_W - 7 * 2 * GFX_FONT_W) / 2, 4, code, 2, C_WHITE);
    gfx_band_flush(y);
    y += 2 * GFX_FONT_H + 8;
    gfx_band_begin(GFX_FONT_H + 8, C_BLACK);
    gfx_text(MARGIN, 8, "press: hide", 21, C_DIM_DOT);
    gfx_band_flush(y);
    y += GFX_FONT_H + 8;
    gfx_clear_rows(y, UI_BODY_Y + UI_BODY_H, C_BLACK);
}

static void draw_all(void)
{
    if (s_passkey >= 0 && esp_timer_get_time() - s_passkey_us > PASSKEY_TIMEOUT_MS * 1000LL) {
        s_passkey = -1;
    }
    draw_header();
    int32_t pk = s_passkey;
    if (pk >= 0) {
        draw_passkey_body(pk);
    } else if (s_menu_open) {
        draw_menu_body();
    } else {
        s_screen->draw_body();
    }
    draw_footer();
}

static void open_menu(void)
{
    s_menu_open = true;
    s_want_double = false;
    s_menu_sel = 0;
    for (int i = 0; i < NUM_MODES; i++) {
        if (s_modes[i] == s_screen) {
            s_menu_sel = i;
        }
    }
    s_menu_last_us = esp_timer_get_time();
}

static void close_menu(void)
{
    s_menu_open = false;
    s_want_double = s_screen->wants_double;
}

#if CONFIG_SCANNER_DEMO
static volatile int s_demo_pick;
#endif

static void handle(ui_event_t ev)
{
    ESP_LOGD(TAG, "Button event %d (menu %s)", ev, s_menu_open ? "open" : "closed");
    if (s_passkey >= 0 && ev != EV_REDRAW) {
        s_passkey = -1;         /* a press hides the passkey (pairing goes on) */
        return;
    }
#if CONFIG_SCANNER_DEMO
    if (ev == EV_DEMO_PICK) {
        /* Fall back to the first network on the page if none has FTM or a location. */
        if (!s_menu_open && s_screen == &ui_screen_wifi && !ui_wifi_demo_open(s_demo_pick) &&
            s_demo_pick == 0) {
            s_screen->on_double();
        }
        return;
    }
#endif
    if (s_menu_open) {
        s_menu_last_us = esp_timer_get_time();
        if (ev == EV_SHORT || ev == EV_DOUBLE) {
            s_menu_sel = (s_menu_sel + 1) % NUM_MODES;
            ESP_LOGI(TAG, "Menu: %s", s_modes[s_menu_sel]->name);
        } else if (ev == EV_LONG) {
            const ui_screen_t *chosen = s_modes[s_menu_sel];
            close_menu();
            if (chosen == s_screen) {
                /* Re-entering the current mode restarts it (e.g. re-sorts the Wi-Fi list). */
                if (s_screen->leave) {
                    s_screen->leave();
                }
                if (s_screen->enter) {
                    s_screen->enter();
                }
            } else {
                ui_show(chosen);
            }
        }
        return;
    }
    if (ev == EV_LONG) {
        open_menu();
        ESP_LOGI(TAG, "Menu opened");
    } else if (ev == EV_SHORT && s_screen->on_short) {
        s_screen->on_short();
    } else if (ev == EV_DOUBLE && s_screen->on_double) {
        s_screen->on_double();
    }
}

static void ui_task(void *arg)
{
    draw_all();
    for (;;) {
        int timeout = (s_menu_open || s_passkey >= 0) ? 1000 : (s_screen->refresh_ms ? s_screen->refresh_ms : 1000);
        ui_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(timeout)) == pdTRUE) {
            if (ev != EV_REDRAW) {
                handle(ev);
            }
            /* Coalesce queued redraw requests; handle any button events among them. */
            while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
                if (ev != EV_REDRAW) {
                    handle(ev);
                }
            }
        }
        if (s_menu_open && esp_timer_get_time() - s_menu_last_us > MENU_TIMEOUT_MS * 1000LL) {
            close_menu();
        }
        draw_all();
    }
}

static void send_event(ui_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

void ui_inject_button(char kind)
{
    if (s_events) {
        send_event(kind == 'L' ? EV_LONG : kind == 'D' ? EV_DOUBLE : EV_SHORT);
    }
}

#if CONFIG_SCANNER_DEMO
/*
    Demo script: button presses on a timer, for video. Mode switches go through
    the menu like a real user would. A real button press stops it.
*/
typedef struct {
    char op;                    /* S/D/L press, M switch mode, P open Wi-Fi details, W wait */
    const ui_screen_t *mode;    /* for M */
    int arg;                    /* for P: which FTM / location network */
    int wait_ms;                /* time on the resulting screen */
} demo_step_t;

static const demo_step_t s_demo[] = {
    { 'W', NULL, 0, 6000 },                     /* list fills in */
    { 'S', NULL, 0, 4000 },                     /* next page */
    { 'S', NULL, 0, 3000 },
    { 'P', NULL, 0, 14000 },                    /* details: ranging, location */
    { 'P', NULL, 1, 12000 },
    { 'D', NULL, 0, 3000 },                     /* back to the list */
#if CONFIG_SCANNER_GPS
    { 'M', &ui_screen_gps, 0, 9000 },
#endif
#if CONFIG_SCANNER_MODE_BLE
    { 'M', &ui_screen_ble, 0, 8000 },
    { 'S', NULL, 0, 3000 },
    { 'D', NULL, 0, 6000 },                     /* device details */
    { 'D', NULL, 0, 2000 },
#endif
#if CONFIG_SCANNER_MODE_BAND
    { 'M', &ui_screen_band, 0, 12000 },
#endif
#if CONFIG_SCANNER_MODE_ZIGBEE
    { 'M', &ui_screen_zigbee, 0, 10000 },
#endif
#if CONFIG_SCANNER_MODE_FILES
    { 'M', &ui_screen_files, 0, 4000 },
    { 'D', NULL, 0, 8000 },                     /* share screen */
    { 'S', NULL, 0, 3000 },                     /* stop sharing */
#endif
    { 'M', &ui_screen_wifi, 0, 0 },
};

static volatile bool s_demo_stop;

/* Wait, scaled by the pace setting. False if the demo was stopped. */
static bool demo_wait(int ms)
{
    ms = ms * CONFIG_SCANNER_DEMO_PACE / 100;
    for (; ms > 0 && !s_demo_stop; ms -= 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !s_demo_stop;
}

static int mode_index(const ui_screen_t *screen)
{
    for (int i = 0; i < NUM_MODES; i++) {
        if (s_modes[i] == screen) {
            return i;
        }
    }
    return 0;
}

static void demo_task(void *arg)
{
    ESP_LOGI(TAG, "Demo running; press the button to stop it");
    while (!s_demo_stop) {
        for (int i = 0; i < (int)(sizeof(s_demo) / sizeof(s_demo[0])) && !s_demo_stop; i++) {
            const demo_step_t *st = &s_demo[i];
            switch (st->op) {
            case 'S':
                send_event(EV_SHORT);
                break;
            case 'D':
                send_event(EV_DOUBLE);
                break;
            case 'L':
                send_event(EV_LONG);
                break;
            case 'P':
                s_demo_pick = st->arg;
                send_event(EV_DEMO_PICK);
                break;
            case 'M': {
                send_event(EV_LONG);                    /* open the menu */
                if (!demo_wait(1200)) {
                    break;
                }
                int steps = (mode_index(st->mode) - mode_index(s_screen) + NUM_MODES) % NUM_MODES;
                for (int k = 0; k < steps && !s_demo_stop; k++) {
                    send_event(EV_SHORT);
                    demo_wait(700);
                }
                demo_wait(500);
                send_event(EV_LONG);                    /* open the highlighted mode */
                break;
            }
            }
            demo_wait(st->wait_ms);
        }
    }
    ESP_LOGI(TAG, "Demo stopped by the button");
    vTaskDelete(NULL);
}
#endif

static bool button_down(void)
{
    return gpio_get_level(CONFIG_SCANNER_BUTTON_GPIO) == 0;
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_SCANNER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    for (;;) {
        /* Wait for a press. */
        while (!button_down()) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
#if CONFIG_SCANNER_DEMO
        if (!s_demo_stop) {
            s_demo_stop = true;     /* hand the button back; this press is swallowed */
            while (button_down()) {
                vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            }
            continue;
        }
#endif
        int held = 0;
        bool long_sent = false;
        while (button_down()) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            held += BUTTON_POLL_MS;
            if (!long_sent && held >= CONFIG_SCANNER_BUTTON_LONG_MS) {
                send_event(EV_LONG);
                long_sent = true;
            }
        }
        if (long_sent || held < BUTTON_DEBOUNCE_MS) {
            continue;
        }
        if (!s_want_double) {
            send_event(EV_SHORT);
            continue;
        }
        /* Short press: wait briefly for a second one. */
        int gap = 0;
        while (!button_down() && gap < DOUBLE_GAP_MS) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            gap += BUTTON_POLL_MS;
        }
        if (gap >= DOUBLE_GAP_MS) {
            send_event(EV_SHORT);
            continue;
        }
        held = 0;
        while (button_down()) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            held += BUTTON_POLL_MS;
        }
        send_event(held < CONFIG_SCANNER_BUTTON_LONG_MS ? EV_DOUBLE : EV_LONG);
    }
}

void ui_init(void)
{
    s_events = xQueueCreate(16, sizeof(ui_event_t));
    ui_show(&ui_screen_wifi);
    xTaskCreate(ui_task, "ui", 6144, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 2560, NULL, 6, NULL);
#if CONFIG_SCANNER_DEMO
    xTaskCreate(demo_task, "demo", 2560, NULL, 4, NULL);
#endif
}
