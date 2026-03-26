
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "gigatron.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/gpio_struct.h"
#include "soc/interrupts.h"
#include "hal/gpio_ll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GIGA";


/* Task handle */
static TaskHandle_t s_gigatron_task_handle = NULL;

/* Internal state of the simulated 74hc595 */
uint8_t hc595state;
uint8_t *hc595ptr = &hc595state;

/* Scanline counter videoline ranges from -41 to +479,
   - negative during vertical blanking.
   - vsync turns low on row -36 and high on row -28
   - serial input happens on row -27.
*/
int videoline;
int videolines_since_ie;

/* Map bytes to gpio mask */
static uint32_t bytemap[256];

/* Gigatron ISR - C version
 * Will this be fast enough?
 */
static void IRAM_ATTR gigatron_isr(void *arg)
{
    gpio_dev_t *hw = &GPIO;
    uint32_t intr = hw->acpu_int1.val;

    /* SERCLK interrupt */
    if ((intr >> (GIGATRON_SERCLK_GPIO-32)) & 1)
        {
            /* Output state */
            uint32_t x = *hc595ptr;
            hw->out_w1ts = bytemap[x];
            hw->out_w1tc = bytemap[x ^ 0xff];
            /* Shift state - proper shift register behavior */
            hc595state = (hc595state << 1) | ((hw->in1.val >> (GIGATRON_SERIN_GPIO-32)) & 1);
            /* Increment counters */
            videolines_since_ie += 1;
            videoline += 1;
            if (videoline >= 480)
                {
                    videoline = -41;
                    /* vertical blanking */
                    BaseType_t woken = pdFALSE;
                    vTaskNotifyGiveFromISR(s_gigatron_task_handle, &woken);
                    portYIELD_FROM_ISR(woken);
                }
            /* Clear interrupt */
            hw->status1_w1tc.val = (1<<(GIGATRON_SERCLK_GPIO-32));
        }

    /* /IE interrupt */
    if ((intr >> (GIGATRON_IE_GPIO-32)) & 1)
        {
            /* Adjust videoline to track vsync */
            if (videolines_since_ie == 521) {
                // we have a typical frame with a single input bus access.
                // videoline should be -27 here.
                videoline  = -27;
            }
            videolines_since_ie = 0;
            /* Clear interrupt */
            hw->status1_w1tc.val = (1<<(GIGATRON_IE_GPIO-32));
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

    /* Make SERCLK and IE generate core 1 interrupts */
    gpio_dev_t *hw = &GPIO;
    hw->pin[GIGATRON_SERCLK_GPIO].int_type = GPIO_INTR_NEGEDGE;
    hw->pin[GIGATRON_SERCLK_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;
    hw->pin[GIGATRON_IE_GPIO].int_type = GPIO_INTR_POSEDGE;
    hw->pin[GIGATRON_IE_GPIO].int_ena = GPIO_LL_APP_CPU_INTR_ENA;

    /* Vertical blanking loop */
    while (1) {
        /* Released on VBL */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Placeholder for future implementation */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



/* GPIO pin array for outputs */
static const gpio_num_t s_output_pins[] = {
    GIGATRON_Q0_GPIO,
    GIGATRON_Q1_GPIO,
    GIGATRON_Q2_GPIO,
    GIGATRON_Q3_GPIO,
    GIGATRON_Q4_GPIO,
    GIGATRON_Q5_GPIO,
    GIGATRON_Q6_GPIO,
    GIGATRON_Q7_GPIO
};

/* Initialize the bytemap lookup table
 * Maps 8-bit values to GPIO register bitmasks for fast parallel output
 */
static void init_bytemap(void)
{
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

/* Configure a single GPIO pin */
static esp_err_t config_gpio(gpio_num_t gpio_num, gpio_mode_t mode) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    return gpio_config(&cfg);
}

/* Initialize all Gigatron interface GPIO pins
 * Sets up pin directions and initial states.
 */
esp_err_t gigatron_init(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing Gigatron interface...");

    /* Configure input pins */
    err = config_gpio(GIGATRON_SERCLK_GPIO, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SERCLK pin: %d", err);
        return err;
    }
    err = config_gpio(GIGATRON_IE_GPIO, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure /IE pin: %d", err);
        return err;
    }
    err = config_gpio(GIGATRON_SERIN_GPIO, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SERIN pin: %d", err);
        return err;
    }

    /* Configure output pins */
    for (int i = 0; i < sizeof(s_output_pins) / sizeof(s_output_pins[0]); i++) {
        err = config_gpio(s_output_pins[i], GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure output pin %d: %d", i, err);
            return err;
        }
        /* Initialize outputs to HIGH */
        gpio_set_level(s_output_pins[i], 1);
    }

    /* Initialize bytemap for fast GPIO writes */
    init_bytemap();

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
    ESP_LOGI(TAG, "  Q0-Q7:  GPIO%d-GPIO%d (OUTPUT)",
             GIGATRON_Q0_GPIO, GIGATRON_Q7_GPIO);
    ESP_LOGI(TAG, "  Task running on core %d", GIGATRON_TASK_CORE);

    return ESP_OK;
}


/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
