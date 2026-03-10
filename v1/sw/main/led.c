#include "led.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define LED_RED_GPIO     GPIO_NUM_13
#define NEOPIXEL_PWR_GPIO GPIO_NUM_2
#define NEOPIXEL_DATA_GPIO GPIO_NUM_0

static const char *TAG = "LED";

static rmt_channel_handle_t s_rmt_channel;
static rmt_encoder_handle_t s_rmt_encoder;
static esp_timer_handle_t s_timer;

typedef enum {
    LED_STATE_TRANSIENT,
    LED_STATE_SOLID,
    LED_STATE_FLASHING,
} led_state_t;

static led_state_t s_nextstate = LED_STATE_SOLID;

typedef struct {
    uint8_t r, g, b;
    uint16_t period_ms;
} led_state_data_t;

static led_state_data_t s_persistent = {0};
static led_state_data_t s_transient = {0};

/* WS2812 timing at 10 MHz RMT resolution (100 ns per tick):
 * BIT0: 3 ticks high (300ns), 9 ticks low (900ns)
 * BIT1: 9 ticks high (900ns), 3 ticks low (300ns) */
static void neopixel_send(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 expects GRB order */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_rmt_channel, s_rmt_encoder, grb, sizeof(grb), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_channel, pdMS_TO_TICKS(100));
}

static void timer_cb(void *arg)
{
    switch (s_nextstate) {
    case LED_STATE_TRANSIENT:
        neopixel_send(s_transient.r, s_transient.g, s_transient.b);
        s_nextstate = LED_STATE_SOLID;
        esp_timer_start_once(s_timer, s_transient.period_ms * 1000);
        break;
        
    case LED_STATE_SOLID:
        neopixel_send(s_persistent.r, s_persistent.g, s_persistent.b);
        if (s_persistent.period_ms > 0) {
            s_nextstate = LED_STATE_FLASHING;
            esp_timer_start_once(s_timer, s_persistent.period_ms * 1000 / 2);
        }
        break;
        
    case LED_STATE_FLASHING:
        neopixel_send(0, 0, 0);
        s_nextstate = LED_STATE_SOLID;
        neopixel_send(s_persistent.r, s_persistent.g, s_persistent.b);
        esp_timer_start_once(s_timer, s_persistent.period_ms * 1000 / 2);
        break;
    }
}

esp_err_t led_init(void)
{
    /* Power the NeoPixel */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << NEOPIXEL_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(NEOPIXEL_PWR_GPIO, 1);

    /* Red LED — power indicator, always on */
    gpio_config_t red_cfg = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&red_cfg);
    gpio_set_level(LED_RED_GPIO, 1);

    /* RMT TX channel for NeoPixel data */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = NEOPIXEL_DATA_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, /* 10 MHz → 100 ns per tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_rmt_channel), TAG, "rmt channel");

    /* Bytes encoder: encodes each bit as an RMT symbol pair */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
        .bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 },
        .flags.msb_first = true,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_rmt_encoder), TAG, "rmt encoder");

    ESP_RETURN_ON_ERROR(rmt_enable(s_rmt_channel), TAG, "rmt enable");

    /* Timer for state transitions */
    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "led_state",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG, "timer");

    /* Start dark */
    neopixel_send(0, 0, 0);

    ESP_LOGI(TAG, "LEDs initialized");
    return ESP_OK;
}

void led_set_persistent(uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms)
{
    esp_timer_stop(s_timer);
    s_persistent.r = r;
    s_persistent.g = g;
    s_persistent.b = b;
    s_persistent.period_ms = period_ms;
    s_nextstate = LED_STATE_SOLID;
    esp_timer_start_once(s_timer, 1); /* 1ms */
}

void led_set_transient(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms)
{
    esp_timer_stop(s_timer);
    s_transient.r = r;
    s_transient.g = g;
    s_transient.b = b;
    s_transient.period_ms = duration_ms;
    s_nextstate = LED_STATE_TRANSIENT;
    esp_timer_start_once(s_timer, 1);
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
