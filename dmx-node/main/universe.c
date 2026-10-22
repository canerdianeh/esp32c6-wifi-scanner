#include <string.h>
#include "esp_timer.h"
#include "node_config.h"
#include "universe.h"

typedef struct {
    univ_info_t info;
    uint16_t len;
    uint8_t data[DMX_SLOTS];
    TaskHandle_t listeners[UNIVERSE_LISTENERS];
} univ_slot_t;

static univ_slot_t s_slots[UNIVERSE_SLOTS];
static int s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void universe_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_count = 0;
}

int universe_find(uint16_t number)
{
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].info.number == number) {
            return i;
        }
    }
    return -1;
}

int universe_subscribe(uint16_t number)
{
    int slot = universe_find(number);
    if (slot >= 0 || s_count >= UNIVERSE_SLOTS) {
        return slot;
    }
    s_slots[s_count].info.number = number;
    return s_count++;
}

int universe_count(void)
{
    return s_count;
}

bool universe_write(int slot, const uint8_t *data, uint16_t len, univ_src_t src,
                    uint8_t priority, uint32_t source_ip)
{
    if (slot < 0 || slot >= s_count) {
        return false;
    }
    if (len > DMX_SLOTS) {
        len = DMX_SLOTS;
    }
    int64_t now = esp_timer_get_time();
    univ_slot_t *u = &s_slots[slot];

    /* TODO: merge (HTP) two sources at the same priority, and keep
     * per-source state so a busy lower-priority source can't hide a
     * higher one that only just went quiet. */
    portENTER_CRITICAL(&s_lock);
    bool active = u->info.last_us != 0 && now - u->info.last_us < LOSS_TIMEOUT_MS * 1000LL;
    bool accept = !active || priority >= u->info.priority || source_ip == u->info.source_ip;
    if (accept) {
        memcpy(u->data, data, len);
        if (len < u->len) {
            memset(u->data + len, 0, u->len - len);
        }
        u->len = len;
        u->info.src = src;
        u->info.priority = priority;
        u->info.source_ip = source_ip;
        u->info.last_us = now;
        u->info.frames++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (accept) {
        for (int i = 0; i < UNIVERSE_LISTENERS && u->listeners[i]; i++) {
            xTaskNotifyGive(u->listeners[i]);
        }
    }
    return accept;
}

void universe_release(int slot, uint32_t source_ip)
{
    if (slot < 0 || slot >= s_count) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_slots[slot].info.source_ip == source_ip) {
        s_slots[slot].info.priority = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

int64_t universe_read(int slot, uint8_t out[DMX_SLOTS])
{
    if (slot < 0 || slot >= s_count) {
        memset(out, 0, DMX_SLOTS);
        return -1;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_slots[slot].data, DMX_SLOTS);
    int64_t last = s_slots[slot].info.last_us;
    portEXIT_CRITICAL(&s_lock);
    return last ? esp_timer_get_time() - last : -1;
}

void universe_get_info(int slot, univ_info_t *info)
{
    portENTER_CRITICAL(&s_lock);
    *info = s_slots[slot].info;
    portEXIT_CRITICAL(&s_lock);
}

void universe_listen(int slot, TaskHandle_t task)
{
    if (slot < 0 || slot >= s_count) {
        return;
    }
    for (int i = 0; i < UNIVERSE_LISTENERS; i++) {
        if (!s_slots[slot].listeners[i]) {
            s_slots[slot].listeners[i] = task;
            return;
        }
    }
}
