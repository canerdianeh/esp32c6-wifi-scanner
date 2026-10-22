#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "net.h"
#include "universe.h"
#include "sacn.h"

static const char *TAG = "sacn";

#define SACN_PORT 5568

/* Offsets into an E1.31 data packet (ANSI E1.31-2018, table 4-1). */
#define OFS_ACN_ID 4
#define OFS_ROOT_VECTOR 18
#define OFS_CID 22
#define OFS_FRAMING_VECTOR 40
#define OFS_PRIORITY 108
#define OFS_SEQUENCE 111
#define OFS_OPTIONS 112
#define OFS_UNIVERSE 113
#define OFS_DMP_VECTOR 117
#define OFS_PROP_COUNT 123
#define OFS_START_CODE 125
#define OFS_DATA 126

#define VECTOR_ROOT_E131_DATA 0x00000004
#define VECTOR_E131_DATA_PACKET 0x00000002
#define VECTOR_DMP_SET_PROPERTY 0x02
#define OPT_PREVIEW 0x80
#define OPT_TERMINATED 0x40

static const uint8_t ACN_ID[12] = { 'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0 };

static uint8_t s_last_seq[UNIVERSE_SLOTS];

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void handle_packet(const uint8_t *pkt, int len, uint32_t from_ip)
{
    if (len < OFS_DATA || memcmp(pkt + OFS_ACN_ID, ACN_ID, sizeof(ACN_ID)) != 0
        || be32(pkt + OFS_ROOT_VECTOR) != VECTOR_ROOT_E131_DATA
        || be32(pkt + OFS_FRAMING_VECTOR) != VECTOR_E131_DATA_PACKET
        || pkt[OFS_DMP_VECTOR] != VECTOR_DMP_SET_PROPERTY) {
        return;     /* not E1.31 data (sync and discovery packets land here too) */
    }
    int slot = universe_find(be16(pkt + OFS_UNIVERSE));
    if (slot < 0) {
        return;
    }
    uint8_t options = pkt[OFS_OPTIONS];
    if (options & OPT_TERMINATED) {
        universe_release(slot, from_ip);
        return;
    }
    if (options & OPT_PREVIEW) {
        return;     /* visualiser-only data */
    }

    /* Out-of-order packets: the spec drops anything 0..19 behind the last.
     * TODO: track sequence per source CID once merging is added. */
    int8_t diff = (int8_t)(pkt[OFS_SEQUENCE] - s_last_seq[slot]);
    if (diff <= 0 && diff > -20) {
        return;
    }
    s_last_seq[slot] = pkt[OFS_SEQUENCE];

    int count = be16(pkt + OFS_PROP_COUNT);     /* includes the start code */
    if (count < 1 || OFS_START_CODE + count > len || pkt[OFS_START_CODE] != 0x00) {
        return;
    }
    universe_write(slot, pkt + OFS_DATA, count - 1, UNIV_SRC_SACN, pkt[OFS_PRIORITY], from_ip);
}

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SACN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Universe N is multicast on 239.255.<N high byte>.<N low byte>. */
    for (int i = 0; i < universe_count(); i++) {
        univ_info_t info;
        universe_get_info(i, &info);
        if (info.number < 1 || info.number > 63999) {
            continue;
        }
        struct ip_mreq mreq = {
            .imr_multiaddr.s_addr = htonl(0xEFFF0000 | info.number),
            .imr_interface.s_addr = htonl(INADDR_ANY),
        };
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            ESP_LOGW(TAG, "can't join universe %u multicast", info.number);
        } else {
            ESP_LOGI(TAG, "listening to universe %u", info.number);
        }
    }
    return sock;
}

static void sacn_task(void *arg)
{
    static uint8_t pkt[640];

    for (;;) {
        uint32_t gen = net_wait_up();
        int sock = open_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* Rebuild the socket (and multicast joins) when the address changes. */
        while (net_generation() == gen) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
            if (len > 0) {
                handle_packet(pkt, len, from.sin_addr.s_addr);
            }
        }
        close(sock);
    }
}

esp_err_t sacn_start(void)
{
    return xTaskCreatePinnedToCore(sacn_task, "sacn", 4096, NULL, 12, NULL, 0) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
