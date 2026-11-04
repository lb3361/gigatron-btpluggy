
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <dirent.h>

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
#define PLUGGY_SAVED_PATH     "/saved"     /* RW SPIFFS: recorded BASIC programs */
#define PLUGGY_SAVED_FILE     "/saved/saved.txt"
#define PLUGGY_PROGRAMS_PATH  "/programs"  /* RO FAT: programs for the loader */
#define PLUGGY_SPIFFS_LABEL   "storage"
#define PLUGGY_FAT_LABEL      "fat"

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
            s0.duration0 + s0.duration1 + s1.duration0 < 80 && s0.duration1 > 15 &&
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

/* Mount the read-write SPIFFS and the read-only FAT (loader programs).
   The FAT is skipped silently when unformatted. */
static void mount_program_stores(void)
{
    static bool mounted = false;
    if (mounted)
        return;
    mounted = true;

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = PLUGGY_SAVED_PATH,
        .partition_label = PLUGGY_SPIFFS_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_conf);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "SPIFFS mounted at %s", PLUGGY_SAVED_PATH);

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

/* ── gt1 loader protocol ─────────────────────────────────────────── */

/* One loader packet per video frame:
 *   [0]='L' [1]=len [2]=addrL [3]=addrH [4..63]=payload [64]=checksum
 * The checksum is computed with a zero initial value over the 'L' byte
 * (contributing 'L'<<6, i.e. its two low bits duplicated into the high
 * bits of the length byte), the length byte, the two address bytes and
 * all 60 payload bytes including padding.  gigatron.c patches the byte
 * to account for the running checksum of preceding continued frames. */

static bool has_gt1_extension(const char *fname)
{
    size_t n = strlen(fname);
    return n > 4 && strcasecmp(fname + n - 4, ".gt1") == 0;
}

static char *make_absolute_fname(const char *fname)
{
    if (fname[0] == '/')
        return strdup(fname);
    char *copy = malloc(strlen(PLUGGY_PROGRAMS_PATH) + strlen(fname) + 2);
    if (! copy)
        return NULL;
    strcpy(copy, PLUGGY_PROGRAMS_PATH);
    strcat(copy, "/");
    strcat(copy, fname);
    return copy;
}

#define LOADER_PAYLOAD   60
#define LOADER_FRAMELEN  65

static esp_err_t gt1_post_packet(uint8_t addrl, uint8_t addrh,
                                 const uint8_t *data, int len)
{
    esp_err_t err;
    uint8_t *frame = malloc(LOADER_FRAMELEN);
    if (!frame)
        return ESP_ERR_NO_MEM;
    memset(frame, 0, LOADER_FRAMELEN);
    frame[0] = 'L';
    frame[1] = (uint8_t)(len + (frame[0] << 6));
    frame[2] = addrl;
    frame[3] = addrh;
    memcpy(frame + 4, data, len);
    int sum = 0;
    for (int i = 0; i < LOADER_FRAMELEN - 1; i++)
        sum += frame[i];
    frame[LOADER_FRAMELEN - 1] = (uint8_t)-sum;
    err = gigatron_post_input(0xff, 0, frame, -LOADER_FRAMELEN, portMAX_DELAY);
    if (err != ESP_OK)
        free(frame);
    return err;
}

static esp_err_t gt1_post_segment(uint8_t addrl, uint8_t addrh,
                                 const uint8_t *data, int seglen)
{
    esp_err_t err;
    if (addrl + seglen > 0x100)
        return ESP_FAIL;
    while (seglen > 0) {
        int len = (seglen > LOADER_PAYLOAD) ? LOADER_PAYLOAD : seglen;
        err = gt1_post_packet(addrl, addrh, data, len);
        if (err != ESP_OK)
            return err;
        seglen -= len;
        addrl += len;
        data  += len;
    }
    return ESP_OK;
}

static esp_err_t gt1_post_file(const char *fname)
{
    esp_err_t err;
    uint8_t addrh, addrl;
    int seglen;
    uint8_t data[256];
    FILE *f = fopen(fname, "rb");
    if (! f) {
        ESP_LOGE(TAG, "Gt1 file %s not found", fname);
        return ESP_ERR_NOT_FOUND;
    }
    /* post dummy frame to resynchronize checksums */
    gigatron_post_input(0xff, 2, 0, 0, pdMS_TO_TICKS(10));
    /* segments */
    err = ESP_FAIL;
    addrh = fgetc(f);
    do {
        addrl = fgetc(f);
        seglen = fgetc(f);
        if (seglen == 0)
            seglen = 256;
        if (feof(f) || ferror(f) ||
            fread(data, 1, seglen, f) != seglen )
            goto end;
        err = gt1_post_segment(addrl, addrh, data, seglen);
        if (err != ESP_OK)
            goto end;
        err = ESP_FAIL;
        addrh = fgetc(f);
    } while (addrh);
    /* execution address */
    addrh = fgetc(f);
    addrl = fgetc(f);
    if (feof(f) || ferror(f))
        goto end;
    if (fgetc(f) != EOF)
        ESP_LOGW(TAG, "Garbage after the end of gt1 file %s", fname);
    ESP_LOGI(TAG, "Execution addres for gt1 file %s is 0x%02x%02x",
             fname, addrh, addrl);
    err = gt1_post_packet(addrl, addrh, data, 0);
 end:
    if (err == ESP_FAIL)
        ESP_LOGE(TAG, "Corrupted gt1 file %s", fname);
    else if (err == ESP_ERR_NO_MEM)
        ESP_LOGE(TAG, "Out of memory while transfering %s", fname);
    fclose(f);
    return err;
}

static esp_err_t pluggy_copy_text(const char *fname)
{
    FILE *f = fopen(fname, "rb");
    if (!f) {
        ESP_LOGE(TAG, "File %s not found", fname);
        return ESP_ERR_NOT_FOUND;
    }
    /* send text with simulated keypresses */
    /* replicate babelfish delays, but counted in frames */
    int c, j = 0, n = 0;
    int lineDelay = 3;
    while ((c = fgetc(f)) != EOF) {
        j += 1; n += 1;
        gigatron_post_input((uint8_t)c, 1, 0, 0, portMAX_DELAY);
        if (c == '\r')
            lineDelay = 18 + j * 3;
        if (j >= 26 || c == '\n') {
            gigatron_post_input(0xff, lineDelay, 0, 0, portMAX_DELAY);
            lineDelay = 3;
            j = 0;
        } else
            gigatron_post_input(0xff, 0, 0, 0, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "Replayed %d bytes from %s", n, fname);
    return ESP_OK;
}

static volatile bool s_transfering = false;

static void pluggy_transfer_task(void *arg)
{
    char *fname = (char*)arg;
    if (has_gt1_extension(fname)) {
        gt1_post_file(fname);
    } else {
        pluggy_copy_text(fname);
    }
    free(fname);
    s_transfering = false;
    vTaskDelete(NULL);
}

static void pluggy_transfer(const char *fname)
{
    if (s_transfering) {
        ESP_LOGI(TAG, "Transfer already running");
        return;
    }
    /* compute filename */
    char *copy = make_absolute_fname(fname);
    if (!copy) {
        ESP_LOGE(TAG, "Out of memory starting transfer of %s", fname);
        return;
    }
    /* start transfer task */
    s_transfering = true;
    if (xTaskCreatePinnedToCore(pluggy_transfer_task, "pluggy_replay",
                                PLUGGY_REPLAY_TASK_STACK, copy,
                                PLUGGY_REPLAY_TASK_PRIORITY, NULL,
                                PLUGGY_REPLAY_TASK_CORE) != pdPASS) {
        free(copy);
        s_transfering = false;
        ESP_LOGE(TAG, "Failed to start replay task");
    }
}

/* ── ctrl+fn bindings ────────────────────────────────────────────── */

#define PLUGGY_GT1_MAX_FILES 10   /* slots on Ctrl+F3..Ctrl+F12 */

/* Returns the name of the n-th (1-based) gt1 file in the FAT store,
 * in directory order, or NULL when there is no such file.
 * The filename is held in a static buffer (save before using). */
static char *pluggy_nth_gt1(int n)
{
    DIR *dir = opendir(PLUGGY_PROGRAMS_PATH);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", PLUGGY_PROGRAMS_PATH);
        return NULL;
    }
    int count = 0;
    static struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcasecmp(de->d_name + len - 4, ".gt1") == 0)
            if (++ count == n) {
                closedir(dir);
                return de->d_name;
            }
    }
    closedir(dir);
    return NULL;
}

/* ── help screen (Ctrl+F1) ───────────────────────────────────────── */

/* Code adapted from marcelk's Babelfish.ino */

/* Render text into the Loader screen's frame buffer using loader frames.
 * The screen is 120 rows of 160 pixels; row i lives at 0x800+i*256, so
 * the rows are not contiguous.  A text line is 6 pixel rows tall.  For
 * compatibility with earlier versions of the loader we never write into
 * pages 0x59 and 0x5a, which limits us to rows 0..78, i.e. 13 text
 * lines.  The Loader must be running for these frames to be accepted. */

#define HELP_FG        63   /* white */
#define HELP_BG        32   /* blue  */
#define HELP_LINES     13   /* rows 0..78 divided by 6 rows per line */
#define HELP_MAX_CHARS 40   /* 160 pixels / 4 pixels per character */

static const int tinyfont[96] = {
#include "tinyfont.h"
};

/* Render one text line at frame-buffer address pos, which must be the
 * start of a pixel row.  Each of the 5 bit planes of the glyphs is
 * rendered and converted to pixel colors in turn, then posted as one
 * segment.  The ATtiny85 couldn't hold both arrays, so BabelFish
 * re-rendered the bitmap per plane; here we can afford the row buffer. */
static esp_err_t help_render_string(uint16_t pos, const char *text)
{
    uint8_t row[4 * HELP_MAX_CHARS];
    int n = 0;
    while (text[n] && n < HELP_MAX_CHARS)
        n++;
    for (int b = 32; b; b >>= 1) {
        int x = 0;
        for (int i = 0; i < n; i++) {
            int c = (uint8_t)text[i] - 32;
            if (c < 0 || c >= 96)
                c = 0;                       /* unmapped: render as space */
            int pixels = tinyfont[c];
            if (pixels >= 0) {
                row[x++] = 0;                /* regular position */
                row[x++] = (pixels >> 9)  & 62;
                row[x++] = (pixels >> 4)  & 62;
                row[x++] = (pixels << 1)  & 62;
            } else {
                row[x++] = 0;                /* shifted down for g j p q y */
                row[x++] = (pixels >> 10) & 31;
                row[x++] = (pixels >> 5)  & 31;
                row[x++] =  pixels         & 31;
                if (text[i] == 'j')          /* special case to dot the j */
                    row[x-1] = '.';
            }
        }
        for (int i = 0; i < x; i++)          /* bitmap -> pixel colors */
            row[i] = (row[i] & b) ? HELP_FG : HELP_BG;
        esp_err_t err = gt1_post_segment(pos & 0xff, pos >> 8, row, x);
        if (err != ESP_OK)
            return err;
        pos += 256;                          /* next scanline */
    }
    return ESP_OK;
}

/* Render a text line and advance pos to the start of the next one. */
static esp_err_t help_render_line(uint16_t *pos, const char *text)
{
    esp_err_t err = help_render_string(*pos, text);
    if (err != ESP_OK)
        return err;
    *pos = (*pos & 0xff00) + 0x600;          /* 6 rows down, same page */
    return ESP_OK;
}

static void pluggy_help_task(void *arg)
{
    uint16_t pos = 0x800;
    int line = 0;
    char text[HELP_MAX_CHARS + 1];

    /* post dummy frame to resynchronize checksums */
    gigatron_post_input(0xff, 2, 0, 0, pdMS_TO_TICKS(10));

    if (line < HELP_LINES) {
        help_render_line(&pos, "Ctrl-F1  This help");
        line++;
    }
    for (int i = 1; i <= PLUGGY_GT1_MAX_FILES; i++) {
        char *name = pluggy_nth_gt1(i);
        if (! name)
            break;
        ESP_LOGI(TAG, "Ctrl+F%-2d  %s", i + 1, name);
        if (line >= HELP_LINES)
            continue;
        /* snprintf copies the name: it lives in readdir's static dirent */
        snprintf(text, sizeof text, "Ctrl-F%-2d %s", i + 1, name);
        help_render_line(&pos, text);
        line++;
    }
    if (FILE *f = fopen(PLUGGY_SAVED_FILE, "rb")) {
        fclose(f);
        ESP_LOGI(TAG, "Ctrl+F12  -SAVED-");
        if (line < HELP_LINES) {
            help_render_line(&pos, "Ctrl-F12 -SAVED-");
            line++;
        }
    }
    s_transfering = false;
    vTaskDelete(NULL);
}

void pluggy_handle_ctrl_fn(int fn)
{
    if (fn == 1) {
        /* Ctrl+F1: list the available programs on the Loader screen */
        if (s_transfering) {
            ESP_LOGI(TAG, "Transfer already running");
            return;
        }
        s_transfering = true;
        if (xTaskCreatePinnedToCore(pluggy_help_task, "pluggy_help",
                                    PLUGGY_REPLAY_TASK_STACK, NULL,
                                    PLUGGY_REPLAY_TASK_PRIORITY, NULL,
                                    PLUGGY_REPLAY_TASK_CORE) != pdPASS) {
            s_transfering = false;
            ESP_LOGE(TAG, "Failed to start help task");
        }
    } else if (fn >= 2 && fn <= 11) {
        /* Ctrl+F2..F11: load the n-th gt1 file from the FAT partition */
        char *path = pluggy_nth_gt1(fn - 1);
        if (path)
            pluggy_transfer(path);
        else
            ESP_LOGW(TAG, "No gt1 file #%d in %s", fn - 1, PLUGGY_PROGRAMS_PATH);
    } else if (fn == 12) {
        /* Ctrl+F12: replay the recorded program */
        pluggy_transfer(PLUGGY_SAVED_FILE);
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
