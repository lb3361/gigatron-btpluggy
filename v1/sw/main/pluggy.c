
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "gigatron.h"
#include "pluggy.h"

#include "esp_log.h"
#include "driver/rmt_rx.h"
#include "soc/soc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "PLUGGY";

/* Receiving bytes from the gigatron */

/* Gigatron task configuration */
#define PLUGGY_RX_TASK_PRIORITY    5
#define PLUGGY_RX_TASK_STACK       4096
#define PLUGGY_RX_TASK_CORE        0

static pluggy_rx_callback_t rx_cb = NULL;
static TaskHandle_t s_gigatron_rx_task_handle = NULL;
static rmt_channel_handle_t rx_chan = NULL;
static int rx_num_symbols = 0;
static rmt_symbol_word_t rx_symbols[4];
static uint8_t rx_byte = 0;
static int rx_bitcount = 0;
static int rx_frame = 0;

IRAM_ATTR
static bool rmt_rx_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    rx_num_symbols = edata->num_symbols;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_gigatron_rx_task_handle, &woken);
    return woken == pdTRUE;
}

void gigatron_rx_task(void *arg) {
    ESP_LOGI(TAG, "Gigatron RX task started on core %d", xPortGetCoreID());
    /* create rx channel */
    rmt_rx_channel_config_t rmt_rx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,   // select source clock
        .resolution_hz = 80000000,        // 80 MHz tick resolution, i.e., 1 tick = 12.5ns
        .mem_block_symbols = 64,          // memory block size, 64 * 4 = 256 Bytes
        .gpio_num = GIGATRON_IE_GPIO,     // GPIO number
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rmt_rx_config, &rx_chan));
    /* set rx channel callback */
    static rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_callback
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &cbs, 0));
    rmt_receive_config_t rmt_recv_config = {
        .signal_range_min_ns = 100,
        .signal_range_max_ns = 1000,
    };
    ESP_ERROR_CHECK(rmt_enable(rx_chan));
    /* loop */
    while (1) {
        /* start receiving */
        ESP_ERROR_CHECK(rmt_receive(rx_chan, rx_symbols, sizeof(rx_symbols), &rmt_recv_config));
        /* wait for pattern */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* decode */
        rmt_symbol_word_t s0 = rx_symbols[0];
        rmt_symbol_word_t s1 = rx_symbols[1];
        if (rx_num_symbols == 2 &&
            s0.duration0 + s0.duration1 + s1.duration0 < 80 &&
            s0.level0 == 0 && s0.level1 == 1 && s1.level0 == 0 )
            {
                rx_byte = (rx_byte >> 1) | ((s1.duration0 > s0.duration0) ? 0x80 : 0);
                rx_bitcount++;
                rx_frame = irq.framecount;
                if (rx_cb && (rx_bitcount & 7) == 0)
                    rx_cb(rx_byte);
            }
        else if (rx_bitcount && irq.framecount - rx_frame >= 2)
            {
                rx_bitcount = 0;
                if (rx_cb)
                    rx_cb(-1);
            }
    }
}

esp_err_t pluggy_init(pluggy_rx_callback_t cb)
{
    esp_err_t err;
    if (! s_gigatron_rx_task_handle) {
        err = xTaskCreatePinnedToCore(gigatron_rx_task,
                                      "gigatron_rx",
                                      PLUGGY_RX_TASK_STACK,
                                      NULL,
                                      PLUGGY_RX_TASK_PRIORITY,
                                      &s_gigatron_rx_task_handle,
                                      PLUGGY_RX_TASK_CORE );
        if (err != pdPASS) {
            ESP_LOGE(TAG, "Failed to create gigatron rx task: %d", err);
            return ESP_FAIL;
        }
    }
    rx_cb = cb;
    return ESP_OK;
}


/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
