
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

#include <stdbool.h>
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"

static const char *TAG = "PLUGGY";

/* ── configuration ───────────────────────────────────────────────── */

/* Gigatron receive task */
#define PLUGGY_RX_TASK_PRIORITY    5
#define PLUGGY_RX_TASK_STACK       4096
#define PLUGGY_RX_TASK_CORE        0

/* Program replay task */
#define PLUGGY_REPLAY_TASK_PRIORITY  5
#define PLUGGY_REPLAY_TASK_STACK     4096
#define PLUGGY_REPLAY_TASK_CORE      0
#define PLUGGY_FRAME_MS              17   /* ~60 Hz video frame */
#define PLUGGY_KEY_FRAMES            3    /* frames a key press is held */
#define PLUGGY_EVENT_QUEUE_SIZE      8    /* GIGATRON_EVENTQUEUE_SIZE */
#define PLUGGY_RX_TIMEOUT_FRAMES     30   /* timeout to close a save */

/* Mount points and partition labels for the two program stores. */
#define PLUGGY_BASIC_PATH     "/basic"     /* RW SPIFFS: recorded BASIC programs */
#define PLUGGY_PROGRAMS_PATH  "/programs"  /* RO FAT: programs for the loader */
#define PLUGGY_SPIFFS_LABEL   "storage"
#define PLUGGY_FAT_LABEL      "fat"
#define PLUGGY_SAVED_FILE     PLUGGY_BASIC_PATH "/saved.txt"

/* A received stream whose first byte is in the control range is a
 * pluggy-reloaded command; anything else is a program to record. Bytes
 * arrive slowly (~1 bit per video frame), so programs are streamed straight
 * to SPIFFS with stdio buffering instead of held in a big RAM buffer. */
#define PLUGGY_CMD_MAX       7
#define PLUGGY_CMD_MAX_LEN   32

/* ── receiving bytes from the gigatron ───────────────────────────── */

static TaskHandle_t s_gigatron_rx_task_handle = NULL;
static rmt_channel_handle_t rx_chan = NULL;
static int rx_num_symbols = 0;
static rmt_symbol_word_t rx_symbols[4];
static uint8_t rx_byte = 0;
static int rx_bitcount = 0;
static int rx_frame = 0;

/* One received stream at a time, streamed to SPIFFS as bytes arrive. */
typedef enum { RX_IDLE = 0, RX_PROGRAM, RX_COMMAND } rx_state_t;
static rx_state_t rx_state = RX_IDLE;
static FILE      *rx_file = NULL;
static size_t     rx_bytes = 0;
static uint8_t    rx_cmd[PLUGGY_CMD_MAX_LEN];
static size_t     rx_cmd_len = 0;

static void pluggy_rx_cb(int byte);

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
                if ((rx_bitcount & 7) == 0)
                    pluggy_rx_cb(rx_byte);
            }
        else if (rx_bitcount && irq.framecount - rx_frame >= PLUGGY_RX_TIMEOUT_FRAMES)
            {
                rx_bitcount = 0;
                pluggy_rx_cb(-1);
            }
    }
}

/* ── program stores (mount) ──────────────────────────────────────── */

/* Mount the read-write SPIFFS (recorded BASIC programs) and the read-only
 * FAT (loader programs). The FAT is skipped silently when unformatted. */
static void mount_program_stores(void)
{
    static bool mounted = false;
    if (mounted)
        return;
    mounted = true;

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = PLUGGY_BASIC_PATH,
        .partition_label = PLUGGY_SPIFFS_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_conf);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "SPIFFS mounted at %s", PLUGGY_BASIC_PATH);

    esp_vfs_fat_mount_config_t fat_conf = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    err = esp_vfs_fat_spiflash_mount_ro(PLUGGY_PROGRAMS_PATH, PLUGGY_FAT_LABEL, &fat_conf);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "FAT not mounted at %s (%s)", PLUGGY_PROGRAMS_PATH, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "FAT mounted read-only at %s", PLUGGY_PROGRAMS_PATH);
}

/* ── recording received programs ─────────────────────────────────── */

/* pluggy-reloaded commands (first byte in the control range). */
static void pluggy_handle_command(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "Received pluggy command %u (%zu bytes) - not implemented",
             data[0], len);
}

/* Called from the RX task with each received byte, then -1 at end of stream.
 * Bytes arrive at ~1 bit per video frame, so there is ample time to write
 * them straight to SPIFFS (stdio buffers the flash writes). */
static void pluggy_rx_cb(int byte)
{
    ESP_LOGI(TAG, "RX callback received %d (%c)", byte,
             (byte >= 32 && byte < 127) ? byte : '?');

    if (byte < 0) {                          /* end of transmission */
        if (rx_state == RX_PROGRAM) {
            fclose(rx_file);
            rx_file = NULL;
            ESP_LOGI(TAG, "Recorded %zu bytes to %s", rx_bytes, PLUGGY_SAVED_FILE);
        } else if (rx_state == RX_COMMAND) {
            pluggy_handle_command(rx_cmd, rx_cmd_len);
        }
        rx_state = RX_IDLE;
        rx_cmd_len = 0;
        return;
    }

    uint8_t b = (uint8_t)byte;

    switch (rx_state) {
    case RX_IDLE:                             /* first byte decides the mode */
        if (b <= PLUGGY_CMD_MAX) {
            rx_state = RX_COMMAND;
            rx_cmd_len = 0;
            rx_cmd[rx_cmd_len++] = b;
        } else if (b >= 32) {
            rx_file = fopen(PLUGGY_SAVED_FILE, "wb");
            if (!rx_file) {
                ESP_LOGE(TAG, "Cannot open %s for writing", PLUGGY_SAVED_FILE);
                return;                        /* stay IDLE, drop this stream */
            }
            rx_state = RX_PROGRAM;
            rx_bytes = 0;
            fputc(b, rx_file);
            rx_bytes++;
        }
        break;
    case RX_PROGRAM:
        if (fputc(b, rx_file) == EOF) {
            ESP_LOGE(TAG, "Write error at %zu bytes, aborting recording", rx_bytes);
            fclose(rx_file);
            rx_file = NULL;
            rx_state = RX_IDLE;
        } else {
            rx_bytes++;
        }
        break;
    case RX_COMMAND:
        if (rx_cmd_len < sizeof(rx_cmd))
            rx_cmd[rx_cmd_len++] = b;
        break;
    }
}

/* ── replaying a recorded program as keyboard input ──────────────── */

static volatile bool s_replaying = false;

static void pluggy_replay_task(void *arg)
{
    const char *fname = (const char*)arg;
    FILE *f = fopen(fname, "rb");
    if (!f) {
        ESP_LOGW(TAG, "No recorded program at %s", PLUGGY_SAVED_FILE);
        s_replaying = false;
        vTaskDelete(NULL);
        return;
    }
    /* replicate babelfish delays */
    int c, j = 0, n = 0;
    int lineDelay = 50;
    while ((c = fgetc(f)) != EOF) {
        j += 1; n += 1;
        gigatron_post_input((uint8_t)c, 0xff, pdMS_TO_TICKS(100));
        if (c == '\r')
            lineDelay = 300 + j * 50;
        if (j >= 26 || c == '\n') {
            while (uxQueueMessagesWaiting(input_event_queue))
                vTaskDelay(pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(lineDelay));
            lineDelay = 50;
            j = 0;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Replayed %d bytes from %s", n, fname);
    s_replaying = false;
    vTaskDelete(NULL);
}

static void pluggy_start_replay(const char *fname)
{
    if (s_replaying) {
        ESP_LOGI(TAG, "Replay already running");
        return;
    }
    s_replaying = true;
    if (xTaskCreatePinnedToCore(pluggy_replay_task, "pluggy_replay",
                                PLUGGY_REPLAY_TASK_STACK, (void*)fname,
                                PLUGGY_REPLAY_TASK_PRIORITY, NULL,
                                PLUGGY_REPLAY_TASK_CORE) != pdPASS) {
        s_replaying = false;
        ESP_LOGE(TAG, "Failed to start replay task");
    }
}

void pluggy_handle_ctrl_fn(int fn)
{
    switch (fn) {
    case 2:  /* Ctrl+F2: replay the recorded program */
        pluggy_start_replay(PLUGGY_SAVED_FILE);
        break;
    default:
        break;
    }
}

/* ── init ────────────────────────────────────────────────────────── */

esp_err_t pluggy_init(void)
{
    esp_err_t err;
    mount_program_stores();
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
    return ESP_OK;
}


/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
