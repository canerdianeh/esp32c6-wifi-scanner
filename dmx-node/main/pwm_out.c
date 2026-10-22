#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "node_config.h"
#include "universe.h"
#include "pwm_out.h"

static const char *TAG = "pwm";

#define SERVO_HZ 50
#define SERVO_BITS LEDC_TIMER_14_BIT    /* 1.2 us steps at 50 Hz */
#define LED_BITS LEDC_TIMER_12_BIT

#if SOC_LEDC_SUPPORT_HS_MODE
#define PWM_SPEED_MODE LEDC_HIGH_SPEED_MODE
#else
#define PWM_SPEED_MODE LEDC_LOW_SPEED_MODE
#endif

#define MAX_CHANNELS (PWM_MAX_CHANNELS < SOC_LEDC_CHANNEL_NUM ? PWM_MAX_CHANNELS : SOC_LEDC_CHANNEL_NUM)

static int s_slot;
static int s_count;
static uint32_t s_duty_max;

/* Duty for a 16-bit level (0..65535). */
static uint32_t level_to_duty(uint32_t level)
{
    if (g_config.pwm.mode == PWM_SERVO) {
        uint32_t us = g_config.pwm.servo_min_us
                      + (int32_t)(g_config.pwm.servo_max_us - g_config.pwm.servo_min_us) * (int32_t)level / 65535;
        return us * (s_duty_max + 1) / (1000000 / SERVO_HZ);
    }
    /* TODO: a dimming curve (gamma / square law) for LEDs. */
    return level * s_duty_max / 65535;
}

static void pwm_task(void *arg)
{
    static uint8_t dmx[DMX_SLOTS];
    const int width = g_config.pwm.fine ? 2 : 1;
    const uint8_t *ch = dmx + g_config.pwm.address - 1;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t age_us = universe_read(s_slot, dmx);
        bool lost = age_us < 0 || age_us > LOSS_TIMEOUT_MS * 1000LL;
        if (lost && (g_config.loss == LOSS_BLACKOUT || age_us < 0)) {
            if (g_config.pwm.mode == PWM_SERVO) {
                continue;   /* don't move servos without data */
            }
            memset(dmx, 0, sizeof(dmx));
        }
        for (int i = 0; i < s_count; i++) {
            uint32_t level = g_config.pwm.fine ? (ch[i * width] << 8 | ch[i * width + 1])
                                               : ch[i] * 257;
            ledc_set_duty(PWM_SPEED_MODE, i, level_to_duty(level));
            ledc_update_duty(PWM_SPEED_MODE, i);
        }
    }
}

esp_err_t pwm_out_start(void)
{
    bool servo = g_config.pwm.mode == PWM_SERVO;
    ledc_timer_config_t timer = {
        .speed_mode = PWM_SPEED_MODE,
        .duty_resolution = servo ? SERVO_BITS : LED_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = servo ? SERVO_HZ : CONFIG_NODE_PWM_LED_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer");
    s_duty_max = (1u << timer.duty_resolution) - 1;

    /* Parse the GPIO list from menuconfig. */
    char gpios[] = CONFIG_NODE_PWM_GPIOS;
    char *save = NULL;
    s_count = 0;
    for (char *tok = strtok_r(gpios, ", ", &save);
         tok && s_count < g_config.pwm.count && s_count < MAX_CHANNELS;
         tok = strtok_r(NULL, ", ", &save)) {
        ledc_channel_config_t chan = {
            .gpio_num = atoi(tok),
            .speed_mode = PWM_SPEED_MODE,
            .channel = s_count,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "channel %d on GPIO%d", s_count, chan.gpio_num);
        s_count++;
    }
    if (s_count < g_config.pwm.count) {
        ESP_LOGW(TAG, "%d channels asked for, %d GPIOs available", g_config.pwm.count, s_count);
    }

    s_slot = universe_subscribe(g_config.pwm.universe);
    ESP_RETURN_ON_FALSE(s_slot >= 0, ESP_ERR_NO_MEM, TAG, "too many universes");
    TaskHandle_t task;
    ESP_RETURN_ON_FALSE(xTaskCreate(pwm_task, "pwm", 3072, NULL, 9, &task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    universe_listen(s_slot, task);
    ESP_LOGI(TAG, "%d %s channels from universe %u address %u%s", s_count, servo ? "servo" : "LED",
             g_config.pwm.universe, g_config.pwm.address, g_config.pwm.fine ? " (16-bit)" : "");
    return ESP_OK;
}
