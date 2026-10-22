#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "node_config.h"
#include "net.h"
#include "universe.h"
#include "artnet.h"

static const char *TAG = "artnet";

#define ARTNET_PORT 6454
#define OP_POLL 0x2000
#define OP_POLL_REPLY 0x2100
#define OP_DMX 0x5000

#define ESTA_PROTOTYPE 0x7FF0   /* ESTA ID range set aside for prototypes */
#define OEM_UNKNOWN 0x00FF

static const uint8_t ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

/* Art-Net 4 ArtPollReply, 239 bytes. Multi-byte fields are big endian
 * unless marked _lo/_hi. */
typedef struct __attribute__((packed)) {
    uint8_t id[8];
    uint8_t opcode_lo, opcode_hi;
    uint8_t ip[4];
    uint8_t port_lo, port_hi;
    uint8_t vers_hi, vers_lo;
    uint8_t net_switch;
    uint8_t sub_switch;
    uint8_t oem_hi, oem_lo;
    uint8_t ubea;
    uint8_t status1;
    uint8_t esta_lo, esta_hi;
    char short_name[18];
    char long_name[64];
    char node_report[64];
    uint8_t num_ports_hi, num_ports_lo;
    uint8_t port_types[4];
    uint8_t good_input[4];
    uint8_t good_output_a[4];
    uint8_t sw_in[4];
    uint8_t sw_out[4];
    uint8_t acn_priority;
    uint8_t sw_macro;
    uint8_t sw_remote;
    uint8_t spare[3];
    uint8_t style;
    uint8_t mac[6];
    uint8_t bind_ip[4];
    uint8_t bind_index;
    uint8_t status2;
    uint8_t good_output_b[4];
    uint8_t status3;
    uint8_t default_resp_uid[6];
    uint8_t user_hi, user_lo;
    uint8_t refresh_hi, refresh_lo;
    uint8_t filler[11];
} poll_reply_t;

_Static_assert(sizeof(poll_reply_t) == 239, "ArtPollReply is 239 bytes");

static uint32_t s_replies;

/* One reply per subscribed universe ("bind index" 1, 2, ...), since each
 * reply can only describe universes that share a Net and Sub-Net. */
static void send_poll_replies(int sock)
{
    esp_netif_ip_info_t ip = net_ip_info();
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(ARTNET_PORT),
        .sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr,   /* directed broadcast */
    };
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_replies++;

    for (int i = 0; i < universe_count(); i++) {
        univ_info_t info;
        universe_get_info(i, &info);

        poll_reply_t r = { 0 };
        memcpy(r.id, ARTNET_ID, sizeof(r.id));
        r.opcode_lo = OP_POLL_REPLY & 0xFF;
        r.opcode_hi = OP_POLL_REPLY >> 8;
        memcpy(r.ip, &ip.ip.addr, 4);
        r.port_lo = ARTNET_PORT & 0xFF;
        r.port_hi = ARTNET_PORT >> 8;
        r.net_switch = (info.number >> 8) & 0x7F;
        r.sub_switch = (info.number >> 4) & 0x0F;
        r.oem_hi = OEM_UNKNOWN >> 8;
        r.oem_lo = OEM_UNKNOWN & 0xFF;
        r.status1 = 0xE0;       /* indicators normal, addresses set over the network */
        r.esta_lo = ESTA_PROTOTYPE & 0xFF;
        r.esta_hi = ESTA_PROTOTYPE >> 8;
        strlcpy(r.short_name, g_config.name, sizeof(r.short_name));
        strlcpy(r.long_name, g_config.name, sizeof(r.long_name));
        snprintf(r.node_report, sizeof(r.node_report), "#0001 [%04lu] OK, universe %u frames %lu",
                 (unsigned long)(s_replies % 10000), info.number, (unsigned long)info.frames);
        r.num_ports_lo = 1;
        r.port_types[0] = 0x80;                 /* output, DMX512 */
        r.good_output_a[0] = info.frames ? 0x80 : 0x00;
        r.sw_out[0] = info.number & 0x0F;
        r.style = 0x00;                         /* StNode */
        memcpy(r.mac, mac, 6);
        memcpy(r.bind_ip, &ip.ip.addr, 4);
        r.bind_index = i + 1;
        r.status2 = 0x01 | 0x08 | (net_is_station() ? 0x06 : 0x00); /* web setup, 15-bit, DHCP */
        r.refresh_lo = g_config.fps;

        sendto(sock, &r, sizeof(r), 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void handle_packet(int sock, const uint8_t *pkt, int len, uint32_t from_ip)
{
    if (len < 10 || memcmp(pkt, ARTNET_ID, sizeof(ARTNET_ID)) != 0) {
        return;
    }
    uint16_t opcode = pkt[8] | (pkt[9] << 8);

    if (opcode == OP_POLL) {
        send_poll_replies(sock);
    } else if (opcode == OP_DMX && len >= 18) {
        /* 12 sequence, 13 physical, 14 SubUni, 15 Net, 16-17 length (BE), 18 data */
        uint16_t port_address = ((pkt[15] & 0x7F) << 8) | pkt[14];
        uint16_t count = (pkt[16] << 8) | pkt[17];
        int slot = universe_find(port_address);
        if (slot < 0 || count > DMX_SLOTS || 18 + count > len) {
            return;
        }
        /* TODO: merge two controllers sending the same universe (HTP/LTP). */
        universe_write(slot, pkt + 18, count, UNIV_SRC_ARTNET, 100, from_ip);
    }
}

static void artnet_task(void *arg)
{
    static uint8_t pkt[600];

    for (;;) {
        uint32_t gen = net_wait_up();
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(ARTNET_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        int yes = 1;
        if (sock < 0 || setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0
            || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "socket: errno %d", errno);
            if (sock >= 0) {
                close(sock);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval tv = { .tv_sec = 1 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ESP_LOGI(TAG, "listening on port %d", ARTNET_PORT);
        send_poll_replies(sock);    /* announce ourselves */

        while (net_generation() == gen) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
            if (len > 0) {
                handle_packet(sock, pkt, len, from.sin_addr.s_addr);
            }
        }
        close(sock);
    }
}

esp_err_t artnet_start(void)
{
    return xTaskCreatePinnedToCore(artnet_task, "artnet", 4096, NULL, 12, NULL, 0) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
