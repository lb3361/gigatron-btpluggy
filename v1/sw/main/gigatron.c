
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

#define DEBUG 1

gigatron_irq_data_t irq = {
    .hc595ptr = &irq.hc595state,
    .hc595ptrlen = 0,
    .hc595state = 0xff,
    .bytemap = 0,
    .inject = 0xff,
    .loaderframe = 0,
    .loaderframelen = 0
};


static const char *TAG = "GIGA";

/* Gigatron task configuration */
#define GIGATRON_TASK_PRIORITY    (configMAX_PRIORITIES - 1)
#define GIGATRON_TASK_STACK       4096
#define GIGATRON_TASK_CORE        1
#define GIGATRON_EVENTQUEUE_SIZE  8

/* Task handle */
static TaskHandle_t s_gigatron_task_handle = NULL;

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
        uint8_t  ix = *irq.hc595ptr;
        hw->out_w1ts = irq.bytemap[ix];
        hw->out_w1tc = irq.bytemap[ix ^ 0xff];
        irq.hc595state = (irq.hc595state << 1) | ((in >> (GIGATRON_SERIN_GPIO-32)) & 1);

        /* Clear interrupt */
        hw->status1_w1tc.val = (1<<(GIGATRON_SERCLK_GPIO-32));

        /* Increment videline counter */
        irq.videolines_since_ie += 1;
        irq.videoline += 1;
        /* Count missed hsync pulses */
#if DEBUG
        if ((in>>(GIGATRON_SERCLK_GPIO-32)) & 1) {
            irq.missed++;
        }
#endif
        /* Bail out quickly when rom reads serialRaw */
        if (irq.videoline == -28) {
            if (irq.loaderframe && irq.loaderframelen > 0) {
                irq.hc595ptr = irq.loaderframe;
                irq.hc595ptrlen = irq.loaderframelen - 1;
                irq.loaderframelen = 0;
                irq.loaderframe = 0;
            } else if (irq.inject != 0xff) {
                irq.hc595ptr = &irq.inject;
                irq.hc595ptrlen = 0;
            } else {
                irq.hc595ptr = &irq.hc595state;
                irq.hc595ptrlen = 0;
            }
        } else if (irq.hc595ptrlen > 0 && irq.videolines_since_ie == 1) {
            irq.hc595ptr++;
            irq.hc595ptrlen--;
        } else {
            irq.hc595ptr = &irq.hc595state;
            irq.hc595ptrlen = 0;
        }
        if (irq.videoline >= 480) {
            irq.framecount++;
            irq.videoline = -41;
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_gigatron_task_handle, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
        /* IE interrupt */
    if (( hw->status1.val >> (GIGATRON_IE_GPIO-32)) & 1) {

        /* Track vsync using /ie assertions */
        if (irq.videolines_since_ie == 521)
            irq.videoline  = -27;
        irq.videolines_since_ie = 0;
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
                    irq.inject = ev.code;
                    delay = 2;
                } else {
                    irq.inject = ev.code;
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
                irq.loaderframe = ...;
                irq.loaderframelen = ...;
            }
#endif
        else if (delay >= 0)
            {
                irq.inject = 0xff;
            }
        /* Debug info */
#if DEBUG
        if (irq.framecount % 60 == 0 && irq.missed)
            ESP_LOGI(TAG,"DBG: Missed hsync pulse (%d times)", irq.missed);
        if (irq.videoline > -28-8)
            ESP_LOGI(TAG,"DBG: Vbl processing ending late (videoline=%d)", irq.videoline);
        irq.missed = 0;
#endif
    }
}


/* GPIO pin array for outputs */
/* Initialize the bytemap lookup table
 * Maps 8-bit values to GPIO register bitmasks for fast parallel output. */
static esp_err_t init_bytemap(void)
{
    static const gpio_num_t s_output_pins[] = {
        GIGATRON_QA_GPIO, GIGATRON_QB_GPIO,
        GIGATRON_QC_GPIO, GIGATRON_QD_GPIO,
        GIGATRON_QE_GPIO, GIGATRON_QF_GPIO,
        GIGATRON_QG_GPIO, GIGATRON_QH_GPIO
    };
    /* Precompute all 256 possible byte values */
    uint32_t *bytemap = malloc(256*sizeof(uint32_t));
    if (! bytemap)
        return ESP_FAIL;
    for (int byte = 0; byte < 256; byte++) {
        uint32_t mask = 0;
        for (int bit = 0; bit < 8; bit++)
            if ((byte >> bit) & 1)
                mask |= (1 << (int)s_output_pins[bit]);
        bytemap[byte] = mask;
    }
    ESP_LOGI(TAG, "bytemap initialized with %d entries", 256);
    irq.bytemap = bytemap;
    return ESP_OK;
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
                         (1ULL << GIGATRON_QG_GPIO)|(1ULL << GIGATRON_QH_GPIO)|
                         (1ULL << PSEUDO_VBL_GPIO)),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&cfg_out));
    ESP_ERROR_CHECK(gpio_set_direction(PSEUDO_VBL_GPIO, GPIO_MODE_INPUT_OUTPUT));

    /* Initialize bytemap for fast GPIO writes */
    ESP_ERROR_CHECK(init_bytemap());
    GPIO.out_w1ts = irq.bytemap[0xff];
    GPIO.out1_w1ts.val = (1 << (PSEUDO_VBL_GPIO-32));

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


/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
