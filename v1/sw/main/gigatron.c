
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


#if NMI_IRQ_PSEUDOCODE

/* Symbol NMI_IRQ_PSEUDOCODE is not defined and must remain so.
   This section merely documents what the assembly code NMI handler does.
   Defining this symbol is only useful to generate assembly code with
   the C compiler and manually replace the contents of nmi.S. */

gigatron_irq_data_t irq = {
    .hc595ptr = &irq.hc595state,
    .hc595ptrlen = 0,
    .hc595state = 0xff,
    .bytemap = 0,
    .inject = 0xff,
    .loaderframe = 0,
    .loaderframelen = 0
};

IRAM_ATTR
void nmi_handler(void)
{
    gpio_dev_t *hw = &GPIO;
    gigatron_irq_data_t *d = &irq;

    /* simulate 74hc595 */
    uint32_t in = hw->in1.val; // time sensitive
    uint8_t  ix = *(d->hc595ptr);
    hw->out_w1ts = d->bytemap[ix];
    hw->out_w1tc = d->bytemap[ix ^ 0xff];
    d->hc595state = (d->hc595state << 1) | ((in >> (GIGATRON_SERIN_GPIO-32)) & 1);

    /* Clear interrupt */
    hw->status1_w1tc.val = (1<<(GIGATRON_SERCLK_GPIO-32));

    /* Increment videoline counter */
    d->videolines_since_ie += 1;
    d->videoline += 1;
    /* Count missed hsync pulses */
    if ((in>>(GIGATRON_SERCLK_GPIO-32)) & 1)
        d->missed++;
    /* Determine next hc595ptr */
    if (d->videoline == -28) {
        /* determine next serialraw */
        if (d->loaderframe && d->loaderframelen > 0) {
            d->hc595ptr = d->loaderframe;
            d->hc595ptrlen = d->loaderframelen;
            d->loaderframelen = 0;
            d->loaderframe = 0;
        } else if (d->inject != 0xff) {
            d->hc595ptr = &d->inject;
            d->hc595ptrlen = 0;
        } else {
            d->hc595ptr = &d->hc595state;
            d->hc595ptrlen = 0;
        }
    } else {
        if (d->hc595ptrlen > 0 && d->videolines_since_ie == 1) {
            /* next byte */
            d->hc595ptr++;
            d->hc595ptrlen--;
        }
        if (d->hc595ptrlen == 0) {
            /* pass thru */
            d->hc595ptr = &d->hc595state;
            d->hc595ptrlen = 0;
        }
        if (d->videoline == 0) {
            /* reset pseudovbl */
            hw->out1_w1ts.val = (1<<(PSEUDO_VBL_GPIO-32));
        }
        if (d->videoline >= 480) {
            d->framecount++;
            d->videoline = -41;
            /* set pseudovbl */
            hw->out1_w1tc.val = (1<<(PSEUDO_VBL_GPIO-32));
        }
    }
}

/* End of the documentation-only part of the file */

#else

#define DEBUG 1

static const char *TAG = "GIGA";

/* Gigatron task configuration */
#define GIGATRON_TASK_PRIORITY    (configMAX_PRIORITIES - 1)
#define GIGATRON_TASK_STACK       4096
#define GIGATRON_TASK_CORE        1
#define GIGATRON_EVENTQUEUE_SIZE  8

/* Task handle */
static TaskHandle_t s_gigatron_task_handle = NULL;

/* Event injection queues */
static QueueHandle_t input_event_queue = NULL;
static QueueHandle_t loader_frame_queue = NULL;

typedef enum {
    INPUT_EVENT_NONE    = 0,
    INPUT_EVENT_KEYBOARD,
    INPUT_EVENT_GAMEPAD,
    INPUT_EVENT_LONG,
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint8_t code;
} input_event_t;


/* This ISR captures processes the /IE pulse and the PSEUDO_VBL pulses
   created by the NMI handler to releases the gigatron_task */
IRAM_ATTR
static void gigatron_isr(void *arg)
{
    gpio_dev_t *hw = &GPIO;
    /* IE interrupt */
    if (( hw->status1.val >> (GIGATRON_IE_GPIO-32)) & 1) {
        /* Track vsync using /ie assertions */
        if (irq.videolines_since_ie == 521)
            irq.videoline  = -27;
        irq.videolines_since_ie = 0;
        /* Clear IE interrupt */
        hw->status1_w1tc.val = (1<<(GIGATRON_IE_GPIO-32));
    }
    /* PSEUDO_VBL interrupt */
    if (( hw->status1.val >> (PSEUDO_VBL_GPIO-32)) & 1) {
        /* Release gigatron task */
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_gigatron_task_handle, &woken);
        portYIELD_FROM_ISR(woken);
        /* Clear PSEUDO_VBL interrupt */
        hw->status1_w1tc.val = (1<<(PSEUDO_VBL_GPIO-32));
    }
}

/* Post keyboard and gamepad events into the Gigatron interface. */
void gigatron_post_input(uint8_t giga_key, uint8_t giga_buttons) {
    static input_event_t ev = { .type = INPUT_EVENT_NONE };

    if (! input_event_queue) {
        return;
    } else if (giga_buttons == 0xff) {
        if (ev.type == INPUT_EVENT_GAMEPAD) {
            ev.type = INPUT_EVENT_KEYBOARD;
            ev.code = 0xff;
            xQueueSend(input_event_queue, &ev, pdMS_TO_TICKS(10));
            ev.type = INPUT_EVENT_NONE;
        }
        ev.type = INPUT_EVENT_KEYBOARD;
        ev.code = giga_key;
        xQueueSend(input_event_queue, &ev, pdMS_TO_TICKS(10));
    } else if (! (ev.type == INPUT_EVENT_LONG && ev.code == giga_buttons)) {
        ev.type = (giga_buttons != giga_key) ? INPUT_EVENT_GAMEPAD : INPUT_EVENT_LONG;
        ev.code = giga_buttons;
        xQueueSend(input_event_queue, &ev, pdMS_TO_TICKS(10));
    }
}

/**
 * Post a loader frame for transmission to the Gigatron.
 * Caller retains ownership of `frame` until this function returns ESP_OK.
 * The frame data must remain valid until the next VBL after transmission starts.
 *
 * @param frame Pointer to the frame data (64 or 65 bytes). Caller must not free this until transmission is complete.
 * @param length Length of the frame (must be 64 or 65).
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if length is not 64 or 65
 *   - ESP_ERR_NO_MEM if queue is full or allocation fails
 */
esp_err_t gigatron_post_frame(const uint8_t *frame, size_t length) {
    if (!loader_frame_queue) {
        ESP_LOGE(TAG, "Loader frame queue not initialized");
        return ESP_FAIL;
    }

    if (!frame || (length != 64 && length != 65)) {
        ESP_LOGE(TAG, "Invalid frame or length (must be 64 or 65): %p, %zu", frame, length);
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(loader_frame_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to enqueue loader frame (queue full or timeout)");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Main Gigatron task running on core 1 */
void gigatron_task(void *arg) {
    ESP_LOGI(TAG, "Gigatron task started on core %d", xPortGetCoreID());

    /* Setup core1 interrupts */
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_GPIO_NMI_SOURCE,
                                   ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_NMI,
                                   NULL, NULL, 0));
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_GPIO_INTR_SOURCE,
                                   ESP_INTR_FLAG_LEVEL3|ESP_INTR_FLAG_IRAM,
                                   gigatron_isr, 0, 0));

    /* Make SERCLK and IE core 1 interrupts */
    gpio_dev_t *hw = &GPIO;
    hw->pin[GIGATRON_IE_GPIO].int_type = GPIO_INTR_POSEDGE;
    hw->pin[GIGATRON_IE_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;
    hw->pin[PSEUDO_VBL_GPIO].int_type = GPIO_INTR_NEGEDGE;
    hw->pin[PSEUDO_VBL_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;
    hw->pin[GIGATRON_SERCLK_GPIO].int_type = GPIO_INTR_NEGEDGE;
    hw->pin[GIGATRON_SERCLK_GPIO].int_ena = GPIO_LL_APP_CPU_NMI_INTR_ENA;

    /* Vertical blanking loop */
    while (1) {
        /* Released on VBL */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Process event injection state machine */
        input_event_t ev;
        static int delay = 0;
        if (delay > 0)
            {
                delay--;
            }
        else if (input_event_queue && xQueueReceive(input_event_queue, &ev, 0) == pdTRUE)
            {
                if (ev.type == INPUT_EVENT_KEYBOARD) {
                    irq.inject = ev.code;
                    delay = 2;
                } else {
                    irq.inject = ev.code;
                    if (ev.type == INPUT_EVENT_LONG)
                        delay = 150;
                }
            }
#if LOADER_FRAME_INJECTION_NOT_YET_IMPLEMENTED
        else if ( pending_frames )
            {
                irq.loaderframe = ...;
                irq.loaderframelen = ...;
            }
#endif
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
        .pull_up_en = GPIO_PULLUP_ENABLE,
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
        .pull_up_en = GPIO_PULLUP_DISABLE,  // input only pads have no pullups :-(
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    err = gpio_config(&cfg_in);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure input pins: err=%d", err);
        return err;
    }

    /* Create event queue */
    if (input_event_queue == NULL) {
        input_event_queue = xQueueCreate(GIGATRON_EVENTQUEUE_SIZE, sizeof(input_event_t));
        if (input_event_queue == NULL) {
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

    /* Create loader frame queue */
    if (loader_frame_queue == NULL) {
        loader_frame_queue = xQueueCreate(GIGATRON_LOADER_FRAME_QUEUE_SIZE, sizeof(const uint8_t *));
        if (loader_frame_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create loader frame queue");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

#endif

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
