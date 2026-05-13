
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "gigatron.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "soc/soc.h"
#include "soc/gpio_struct.h"
#include "soc/interrupts.h"
#include "hal/gpio_ll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "GIGA";

/* Task handle */
static TaskHandle_t s_gigatron_task_handle = NULL;

/* Internal state of the simulated 74hc595 */
uint8_t hc595state;
uint8_t *hc595ptr = &hc595state;

/* Injected event */
uint8_t  hc595inject = 0xff;
uint8_t *hc595framedata = 0;
unsigned hc595framelen = 0;

/* Frame counter */
int framecount;

/* Internal queue for event injection */
static QueueHandle_t giga_event_queue = NULL;

typedef enum {
    GIGA_EVENT_NONE    = 0,
    GIGA_EVENT_KEYBOARD,
    GIGA_EVENT_GAMEPAD,
    GIGA_EVENT_LONG,
} giga_event_type_t;

typedef struct {
    giga_event_type_t type;
    uint8_t code;
} giga_event_t;


/* Scanline counter videoline ranges from -41 to +479,
   - negative during vertical blanking.
   - vsync turns low on row -36 and high on row -28
   - serial input happens on row -27.
*/

int videoline;
int videolines_since_ie;

/* Map bytes to gpio mask */
static uint32_t bytemap[256];

IRAM_ATTR __attribute__((always_inline))
static inline void set_bus(gpio_dev_t *hw, uint8_t x) {
    hw->out_w1ts = bytemap[x];
    hw->out_w1tc = bytemap[x ^ 0xff];
}


/* Gigatron ISR - C version
   Will this be fast enough or do we
   need an assembly code NMI routine? */

IRAM_ATTR
static void gigatron_isr(void *arg)
{
    gpio_dev_t *hw = &GPIO;

    /* SERCLK interrupt */
    if (( hw->status1.val >> (GIGATRON_SERCLK_GPIO-32)) & 1) {

        /* simulate 74hc595 */
        uint32_t in = hw->in1.val; // time sensitive
        uint8_t  ix = *hc595ptr;
        hw->out_w1ts = bytemap[ix];
        hw->out_w1tc = bytemap[ix ^ 0xff];
        hc595state = (hc595state << 1) | ((in >> (GIGATRON_SERIN_GPIO-32)) & 1);

        /* Clear interrupt */
        hw->status1_w1tc.val = (1<<(GIGATRON_SERCLK_GPIO-32));

        /* Increment videline counter */
        videolines_since_ie += 1;
        videoline += 1;

        /* Bail out quickly when rom reads serialRaw */
        if (videoline == -27)
            return;
        if (videoline == -28) {
            if (hc595framelen && hc595framedata)
                hc595ptr = hc595framedata;
            else if (hc595inject != 0xff)
                hc595ptr = &hc595inject;
            else
                hc595ptr = &hc595state;
        } else if (hc595framelen > 0 && videolines_since_ie == 1) {
            hc595ptr++;
            hc595framelen--;
        } else {
            hc595ptr = &hc595state;
        }
        if (videoline >= 480) {
            framecount++;
            videoline = -41;
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_gigatron_task_handle, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
        /* IE interrupt */
    if (( hw->status1.val >> (GIGATRON_IE_GPIO-32)) & 1) {

        /* Track vsync using /ie assertions */
        if (videolines_since_ie == 521)
            videoline  = -27;
        videolines_since_ie = 0;
        /* Clear IE interrupt */
        hw->status1_w1tc.val = (1<<(GIGATRON_IE_GPIO-32));
    }
}

/* Post keyboard and gamepad events into the Gigatron interface. */
void gigatron_post(uint8_t giga_key, uint8_t giga_buttons) {
    static giga_event_t ev = { .type = GIGA_EVENT_NONE };

    if (! giga_event_queue) {
        return;
    } else if (giga_buttons == 0xff) {
        if (ev.type == GIGA_EVENT_GAMEPAD) {
            ev.type = GIGA_EVENT_KEYBOARD;
            ev.code = 0xff;
            xQueueSend(giga_event_queue, &ev, pdMS_TO_TICKS(10));
            ev.type = GIGA_EVENT_NONE;
        }
        if (giga_key != 0xff) {
            ev.type = GIGA_EVENT_KEYBOARD;
            ev.code = giga_key;
            xQueueSend(giga_event_queue, &ev, pdMS_TO_TICKS(10));
        }
    } else if (ev.type == GIGA_EVENT_LONG && ev.code == giga_buttons) {
        /* pass */
    } else {
        ev.type = (giga_buttons != giga_key) ? GIGA_EVENT_GAMEPAD : GIGA_EVENT_LONG;
        ev.code = giga_buttons;
        xQueueSend(giga_event_queue, &ev, pdMS_TO_TICKS(10));
    }
}

/* Main Gigatron task running on core 1 */
void gigatron_task(void *arg) {
    esp_err_t err;

    ESP_LOGI(TAG, "Gigatron task started on core %d", xPortGetCoreID());

    /* Setup core1 interrupts */
    err = esp_intr_alloc(ETS_GPIO_INTR_SOURCE,
                         ESP_INTR_FLAG_LEVEL3|ESP_INTR_FLAG_IRAM,
                         gigatron_isr, 0, 0);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to acquire interrupt (%d)", err);

    /* Make SERCLK and IE core 1 interrupts */
    gpio_dev_t *hw = &GPIO;
    hw->pin[GIGATRON_SERCLK_GPIO].int_type = GPIO_INTR_NEGEDGE;
    hw->pin[GIGATRON_SERCLK_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;
    hw->pin[GIGATRON_IE_GPIO].int_type = GPIO_INTR_POSEDGE;
    hw->pin[GIGATRON_IE_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;

    /* Vertical blanking loop */
    while (1) {
        /* Released on VBL */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Process event injection state machine */
        giga_event_t ev;
        static int delay = 0;
        if (delay > 0)
            {
                delay--;
            }
        else if (giga_event_queue && xQueueReceive(giga_event_queue, &ev, 0) == pdTRUE)
            {
                if (ev.type == GIGA_EVENT_KEYBOARD) {
                    hc595inject = ev.code;
                    delay = 2;
                } else {
                    hc595inject = ev.code;
                    if (ev.type == GIGA_EVENT_LONG)
                        delay = 150;
                    else if (ev.code == 0xff)
                        delay = 0;
                    else
                        delay = -1;
                }
            }
#if LOADER_FRAME_INJECTION_NOT_YET_IMPLEMENTED
        else if ( pending_frames )
            {
                hc595framelen = ...;
                hc595framedata = ...;
            }
#endif
        else if (delay >= 0)
            {
                hc595inject = 0xff;
            }
    }
}


/* GPIO pin array for outputs */
/* Initialize the bytemap lookup table
 * Maps 8-bit values to GPIO register bitmasks for fast parallel output. */
static void init_bytemap(void)
{
    static const gpio_num_t s_output_pins[] = {
        GIGATRON_QA_GPIO, GIGATRON_QB_GPIO,
        GIGATRON_QC_GPIO, GIGATRON_QD_GPIO,
        GIGATRON_QE_GPIO, GIGATRON_QF_GPIO,
        GIGATRON_QG_GPIO, GIGATRON_QH_GPIO
    };
    /* Precompute all 256 possible byte values */
    for (int byte = 0; byte < 256; byte++) {
        uint32_t mask = 0;
        for (int bit = 0; bit < 8; bit++)
            if ((byte >> bit) & 1)
                mask |= (1 << (int)s_output_pins[bit]);
        bytemap[byte] = mask;
    }
    ESP_LOGI(TAG, "bytemap initialized with %d entries", 256);
}


/* Initialize all Gigatron interface GPIO pins and event queue
 * Sets up pin directions and initial states.
 */
esp_err_t gigatron_init(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing Gigatron interface...");

    /* Configure output pins */
    gpio_config_t cfg_out = {
        .pin_bit_mask = ((1ULL << GIGATRON_QA_GPIO)|(1ULL << GIGATRON_QB_GPIO)|
                         (1ULL << GIGATRON_QC_GPIO)|(1ULL << GIGATRON_QD_GPIO)|
                         (1ULL << GIGATRON_QE_GPIO)|(1ULL << GIGATRON_QF_GPIO)|
                         (1ULL << GIGATRON_QG_GPIO)|(1ULL << GIGATRON_QH_GPIO) ),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    err = gpio_config(&cfg_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure output pins: err=%d", err);
        return err;
    }

    /* Initialize bytemap for fast GPIO writes */
    init_bytemap();
    set_bus(&GPIO, 0xff);

    /* Configure input pins */
    gpio_config_t cfg_in = {
        .pin_bit_mask = ((1ULL << GIGATRON_SERCLK_GPIO)|
                         (1ULL << GIGATRON_IE_GPIO)|
                         (1ULL << GIGATRON_SERIN_GPIO) ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    err = gpio_config(&cfg_in);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure input pins: err=%d", err);
        return err;
    }

    /* Create event queue */
    if (giga_event_queue == NULL) {
        giga_event_queue = xQueueCreate(GIGATRON_EVENTQUEUE_SIZE, sizeof(giga_event_t));
        if (giga_event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create event queue");
            return ESP_FAIL;
        }
    }

    /* Create task on core 1 */
    err = xTaskCreatePinnedToCore(
        gigatron_task,
        "gigatron",
        GIGATRON_TASK_STACK,
        NULL,
        GIGATRON_TASK_PRIORITY,
        &s_gigatron_task_handle,
        GIGATRON_TASK_CORE
    );
    if (err != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gigatron task: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Gigatron interface initialized successfully");
    ESP_LOGI(TAG, "  SERCLK: GPIO%d (INPUT)", GIGATRON_SERCLK_GPIO);
    ESP_LOGI(TAG, "  /IE:    GPIO%d (INPUT)", GIGATRON_IE_GPIO);
    ESP_LOGI(TAG, "  SERIN:  GPIO%d (INPUT)", GIGATRON_SERIN_GPIO);
    ESP_LOGI(TAG, "  Q0-Q7:  GPIO%d-GPIO%d (OUTPUT)", GIGATRON_QA_GPIO, GIGATRON_QH_GPIO);

    return ESP_OK;
}


/* Receiving bytes from the gigatron */

static gigatron_rx_callback_t rx_cb = NULL;
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
                rx_frame = framecount;
                if (rx_cb && (rx_bitcount & 7) == 0)
                    rx_cb(rx_byte);
            }
        else if (rx_bitcount && framecount - rx_frame >= 2)
            {
                rx_bitcount = 0;
                if (rx_cb)
                    rx_cb(-1);
            }
    }
}

esp_err_t gigatron_init_rx(gigatron_rx_callback_t cb)
{
    esp_err_t err;
    if (! s_gigatron_rx_task_handle) {
        err = xTaskCreatePinnedToCore(gigatron_rx_task,
                                      "gigatron_rx",
                                      GIGATRON_TASK_STACK,
                                      NULL,
                                      GIGATRON_TASK_PRIORITY,
                                      &s_gigatron_rx_task_handle,
                                      GIGATRON_TASK_CORE );
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
