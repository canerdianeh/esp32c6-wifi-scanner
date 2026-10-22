/*
    The universes this node listens to, written by the inputs (sACN, Art-Net,
    wired DMX) and read by the outputs.

    Outputs subscribe to universe numbers at startup; inputs only keep data
    for subscribed universes. Outputs that only update on new data register
    with universe_listen() and wait with ulTaskNotifyTake().
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UNIVERSE_SLOTS 8        /* up to 4 for pixels, 1 each for port A, port B, PWM */
#define DMX_SLOTS 512
#define UNIVERSE_LISTENERS 2

/* Who wrote a universe last. */
typedef enum {
    UNIV_SRC_NONE,
    UNIV_SRC_SACN,
    UNIV_SRC_ARTNET,
    UNIV_SRC_DMX,
} univ_src_t;

typedef struct {
    uint16_t number;
    univ_src_t src;
    uint8_t priority;           /* sACN priority; Art-Net and wired DMX count as 100 */
    uint32_t source_ip;         /* network byte order, 0 for wired */
    uint32_t frames;
    int64_t last_us;            /* esp_timer time of the last write, 0 if none yet */
} univ_info_t;

/* Clears the table. Call before subscribing. */
void universe_init(void);

/* Returns the slot for this universe, adding it if needed, or -1 if the
 * table is full. */
int universe_subscribe(uint16_t number);

/* Returns the slot for this universe, or -1 if nobody listens to it. */
int universe_find(uint16_t number);

int universe_count(void);

/* Stores a frame (start code removed). Lower priority data is ignored while a
 * higher priority source is still sending. Returns false if ignored. */
bool universe_write(int slot, const uint8_t *data, uint16_t len, univ_src_t src,
                    uint8_t priority, uint32_t source_ip);

/* An sACN source said it stopped: lower priorities may take over at once. */
void universe_release(int slot, uint32_t source_ip);

/* Copies the latest frame (always 512 slots, zero padded) and returns how old
 * it is in microseconds, or -1 if nothing has arrived yet. */
int64_t universe_read(int slot, uint8_t out[DMX_SLOTS]);

void universe_get_info(int slot, univ_info_t *info);

/* Every accepted write to this slot gives `task` a notification. */
void universe_listen(int slot, TaskHandle_t task);
