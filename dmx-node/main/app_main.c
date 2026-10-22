/*
    DMX node: receives DMX512 over sACN, Art-Net or a wired RS485 input, and
    drives two DMX ports, a pixel strip and PWM channels.

    Inputs write universes into the universe table (universe.h); each output
    subscribes to the universes it needs and reads from there.
*/
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "node_config.h"
#include "universe.h"
#include "net.h"
#include "dmx_port.h"
#include "sacn.h"
#include "artnet.h"
#include "pixels.h"
#include "pwm_out.h"
#include "web_config.h"
#if CONFIG_NODE_BLE_SETUP
#include "ble_config.h"
#endif

static const char *TAG = "node";

static const char *const SOURCE_LABELS[] = { "sACN", "Art-Net", "DMX in" };

static void start_outputs(void)
{
    const dmx_port_pins_t port_a = {
        .uart = CONFIG_NODE_PORT_A_UART,
        .tx_gpio = CONFIG_NODE_PORT_A_TX_GPIO,
        .rx_gpio = CONFIG_NODE_PORT_A_RX_GPIO,
        .dir_gpio = CONFIG_NODE_PORT_A_DIR_GPIO,
    };
    const dmx_port_pins_t port_b = {
        .uart = CONFIG_NODE_PORT_B_UART,
        .tx_gpio = CONFIG_NODE_PORT_B_TX_GPIO,
        .rx_gpio = -1,
        .dir_gpio = CONFIG_NODE_PORT_B_DIR_GPIO,
    };

    /* With wired input, port A feeds its universe number; outputs set to
     * the same number repeat it. */
    if (g_config.port_a.mode == PORT_IN) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(dmx_port_start_input(&port_a, universe_subscribe(g_config.port_a.universe)));
    } else if (g_config.port_a.mode == PORT_OUT) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(dmx_port_start_output(&port_a, universe_subscribe(g_config.port_a.universe),
                                                            g_config.fps));
    }
    if (g_config.port_b.mode == PORT_OUT) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(dmx_port_start_output(&port_b, universe_subscribe(g_config.port_b.universe),
                                                            g_config.fps));
    }
    if (g_config.pix.type != PIX_OFF) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(pixels_start());
    }
    if (g_config.pwm.mode != PWM_OFF) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(pwm_out_start());
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK_WITHOUT_ABORT(node_config_load());
    ESP_LOGI(TAG, "%s, input %s", g_config.name, SOURCE_LABELS[g_config.source]);

    /* Outputs first: they subscribe the universes the receivers listen to. */
    universe_init();
    start_outputs();

    ESP_ERROR_CHECK(net_start());
    if (g_config.source == SOURCE_SACN) {
        ESP_ERROR_CHECK(sacn_start());
    } else if (g_config.source == SOURCE_ARTNET) {
        ESP_ERROR_CHECK(artnet_start());
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(web_config_start());

#if CONFIG_NODE_BLE_SETUP
    ESP_ERROR_CHECK_WITHOUT_ABORT(ble_config_start());
#else
    net_set_low_latency();
#endif
}
