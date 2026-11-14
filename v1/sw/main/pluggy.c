
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
#define PLUGGY_RX_TASK_PRIORITY   5
#define PLUGGY_RX_TASK_STACK      4096
#define PLUGGY_RX_TASK_CORE       0
#define PLUGGY_RX_QUEUE_SIZE      8
#define PLUGGY_RX_TIMEOUT_FRAMES  30   /* timeout to close a save */

/* Program cmd task */
#define PLUGGY_CMD_TASK_PRIORITY  5
#define PLUGGY_CMD_TASK_STACK     4096
#define PLUGGY_CMD_TASK_CORE      0
#define PLUGGY_CMD_QUEUE_SIZE     8

/* Mount points and partition labels for the two program stores. */
#define PLUGGY_SAVED_PATH     "/saved"     /* RW SPIFFS: recorded BASIC programs */
#define PLUGGY_SAVED_FILE     "/saved/saved.txt"
#define PLUGGY_PROGRAMS_PATH  "/programs"  /* RO FAT: programs for the loader */
#define PLUGGY_SPIFFS_LABEL   "storage"
#define PLUGGY_FAT_LABEL      "fat"
#define PLUGGY_PATH_MAX       120

/* Bits received by RMT are queued and processed by the rx task. */
static rmt_channel_handle_t rx_chan = NULL;
static rmt_symbol_word_t rx_symbols[4];
static QueueHandle_t rx_queue = NULL;
static TaskHandle_t s_pluggy_rx_task_handle = NULL;

typedef struct {
    int bit;
    int frame;
} rmsg_t;


/* All outbound work (key bindings and browser commands) is funnelled through
 * one queue into one worker task, which makes the transfers mutually exclusive
 * by construction and keeps the directory state single-threaded. */
static QueueHandle_t cmd_queue = NULL;
static TaskHandle_t s_gigatron_cmd_task_handle = NULL;

typedef enum {
    /* Browser protocol */
    CMD_SD_LIST   = 1,   /* send the next chunk of the directory listing */
    CMD_SD_EXEC   = 2,   /* transfer and execute <name> */
    CMD_SD_OPEN   = 3,   /* enter directory <name> ("" = root, ".." = parent) */
    CMD_SD_CLOSE  = 4,   /* leave the browser protocol */
    CMD_SD_BEGIN  = 5,   /* mount the store */
    CMD_SD_INIT   = 6,   /* enter the browser protocol */
    /* Original pluggy commands */
    CMD_HELP      = 100,
    CMD_LOAD_GT1  = 101,
    CMD_REPLAY    = 102,
} pcmd_t;

typedef struct {
    pcmd_t cmd;
    char arg[PLUGGY_PATH_MAX];
} pmsg_t;


/* ── cmd queue ─────────────────────────────────────────── */

/* All outbound work (Ctrl+Fn bindings and browser commands) is funnelled
 * into one queue drained by one worker task.  A single worker makes the
 * transfers mutually exclusive by construction (no s_transfering flag) and
 * keeps the directory state single-threaded.  The RX task only decodes
 * bytes and enqueues; it never blocks on the Gigatron. */

static void post_pcmd(pcmd_t cmd, const char *arg)
{
    if (! cmd_queue) {
        ESP_LOGE(TAG, "post_pcmd cmd=%d: queue not created", cmd);
        return;
    }
    pmsg_t m = { .cmd = cmd };
    m.arg[0] = '\0';
    if (arg)
        strlcpy(m.arg, arg, sizeof m.arg);
    ESP_LOGI(TAG, "post cmd=%d arg='%s'", cmd, m.arg);
    if (xQueueSend(cmd_queue, &m, pdMS_TO_TICKS(10)) != pdTRUE)
        ESP_LOGW(TAG, "command queue full, dropped cmd %d", cmd);
}


/* ── receiving bytes from the gigatron ───────────────────────────── */

static void pluggy_rx_cb(int byte);

IRAM_ATTR
static bool rmt_rx_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    int bit = -1;
    rmt_symbol_word_t s0 = edata->received_symbols[0];
    rmt_symbol_word_t s1 = edata->received_symbols[1];
    if (edata->num_symbols == 2
        && s0.duration0 + s0.duration1 + s1.duration0 < 80 && s0.duration1 > 15
        && s0.level0 == 0 && s0.level1 == 1 && s1.level0 == 0 )
        {
            bit = (s1.duration0 > s0.duration0);
            /* prevent SYS_SendSerial from aborting */
            irq.inject = 0xff;
            irq.delay = 2;
        }
    rmsg_t rmsg;
    rmsg.bit = bit;
    rmsg.frame = irq.framecount;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rx_queue, &rmsg, &woken);
    return woken == pdTRUE;
}

void pluggy_rx_task(void *arg) {
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
    /* statics */
    static int rx_frame = 0;
    static uint8_t rx_byte = 0;
    static uint8_t rx_last = -1;
    static uint8_t rx_inbit = 1;
    static int rx_bitcount = 0;
    /* loop */
    rx_frame = irq.framecount;
    while (1) {
        /* start receiving */
        ESP_ERROR_CHECK(rmt_receive(rx_chan, rx_symbols, sizeof(rx_symbols), &rmt_recv_config));
        /* wait bit */
        rmsg_t rmsg;
        xQueueReceive(rx_queue, &rmsg, portMAX_DELAY);
        if (rmsg.frame - rx_frame > 1)
            {
                if (rx_inbit > 1) {
                    if (rx_inbit == 2 && rx_byte == 0  && rx_last == 0x0a) {
                        ESP_LOGI(TAG,"rx: phantom sd_begin hack");
                        pluggy_rx_cb(rx_last >> 1); /* phantom sd_init/sd_begin pair */
                    } else {
                        ESP_LOGI(TAG,"rx: incomplete byte");
                    }
                    rx_byte = 0;
                    rx_inbit = 1;
                }
                if (rx_bitcount && rmsg.frame - rx_frame > PLUGGY_RX_TIMEOUT_FRAMES) {
                    ESP_LOGI(TAG,"rx: timeout");
                    rx_bitcount = 0;
                    pluggy_rx_cb(-1);
                }
                rx_last = -1;
            }        
        /* good bit */
        if (rmsg.bit >= 0)
            {
                ESP_LOGI(TAG,"rx: bit %d inbit %02x frame %d", rmsg.bit, rx_inbit, rmsg.frame);
                rx_frame = rmsg.frame;
                if (rmsg.bit)
                    rx_byte |= rx_inbit;
                rx_inbit <<= 1;
                rx_bitcount += 1;
                if (! rx_inbit) {
                    ESP_LOGI(TAG,"rx: received byte %02x", rx_byte);
                    pluggy_rx_cb(rx_byte);
                    rx_last = rx_byte;
                    rx_inbit = 1;
                    rx_byte = 0;
                }
            }
    }
}

/* ── processing received bytes ───────────────────────────── */

typedef enum { RX_IDLE = 0, RX_PROGRAM, RX_COMMAND } rx_state_t;
static rx_state_t rx_state = RX_IDLE;
static FILE      *rx_file = NULL;
static size_t     rx_bytes = 0;
static uint8_t    rx_cmd[PLUGGY_PATH_MAX];
static size_t     rx_cmd_len = 0;

/* Called from the RX task with each received byte, then -1 at end of
 * stream.  A stream that starts with a browser command byte (1..6) is
 * protocol; EXEC/OPEN carry a NUL-terminated name. A stream starting
 * with a printable byte is a program, streamed straight to SPIFFS. */
static void pluggy_rx_cb(int byte)
{
    if (byte < 0) {                          /* end of transmission */
        if (rx_state == RX_PROGRAM) {
            fclose(rx_file);
            rx_file = NULL;
            ESP_LOGI(TAG, "RX: program end, recorded %zu bytes to %s",
                     rx_bytes, PLUGGY_SAVED_FILE);
        } else if (rx_state == RX_COMMAND) {
            /* stream ended before the NUL terminator arrived */
            rx_cmd[rx_cmd_len < sizeof rx_cmd ? rx_cmd_len : sizeof rx_cmd - 1] = '\0';
            ESP_LOGW(TAG, "RX: command stream ended without NUL (cmd=%d partial='%s')",
                     rx_cmd[0], (char *)rx_cmd + 1);
        } else {
            ESP_LOGD(TAG, "RX: idle end-of-stream");
        }
        rx_state = RX_IDLE;
        rx_cmd_len = 0;
        return;
    }
    uint8_t b = (uint8_t)byte;
    switch (rx_state) {
    case RX_IDLE:                             /* first byte decides the mode */
        if (b >= 32)
            {
                rx_file = fopen(PLUGGY_SAVED_FILE, "wb");
                if (!rx_file) {
                    ESP_LOGE(TAG, "Cannot open %s for writing", PLUGGY_SAVED_FILE);
                    return;
                }
                rx_state = RX_PROGRAM;
                rx_bytes = 0;
                ESP_LOGI(TAG, "RX: program mode, first byte 0x%02x", b);
                fputc(b, rx_file);
                rx_bytes++;
            }                
        else if (b >= CMD_SD_LIST && b <= CMD_SD_INIT)
            {
                ESP_LOGI(TAG, "RX: command byte %d", b);
                if (b == CMD_SD_EXEC || b == CMD_SD_OPEN) {
                    rx_state = RX_COMMAND;        /* collect the name */
                    rx_cmd[0] = b;
                    rx_cmd_len = 1;
                } else {
                    post_pcmd((pcmd_t)(b), NULL);
                }
            }
        else
            {
                ESP_LOGI(TAG, "RX: byte 0x%02x ignored (state=IDLE)", b);
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
        if (b == 0) {                          /* NUL terminator: dispatch */
            rx_cmd[rx_cmd_len++] = '\0';
            ESP_LOGI(TAG, "RX: %s name='%s'",
                     rx_cmd[0] == CMD_SD_EXEC ? "EXEC" : "OPEN",
                     (char *)rx_cmd + 1);
            if (rx_cmd[0] == CMD_SD_EXEC)
                post_pcmd(CMD_SD_EXEC, (char *)rx_cmd + 1);
            else
                post_pcmd(CMD_SD_OPEN, (char *)rx_cmd + 1);
            rx_state = RX_IDLE;
            rx_cmd_len = 0;
        } else if (b >= 32 && b < 127 && rx_cmd_len < sizeof rx_cmd - 1) {
            rx_cmd[rx_cmd_len++] = b;
        } else {
            ESP_LOGD(TAG, "RX: command byte 0x%02x dropped (len=%zu)",
                     b, rx_cmd_len);
        }
        break;
    }
}

/* ── gt1 loader protocol ─────────────────────────────────────────── */

/* One loader packet per video frame:
 *   [0]='L' [1]=len [2]=addrL [3]=addrH [4..63]=payload [64]=checksum
 * The checksum is computed with a zero initial value over the 'L' byte
 * (contributing 'L'<<6, i.e. its two low bits duplicated into the high
 * bits of the length byte), the length byte, the two address bytes and
 * all 60 payload bytes inluding padding.  gigatron.c patches the byte
 * to account for the running checksum of preceding continued frames. */

#define LOADER_PAYLOAD   60
#define LOADER_FRAMELEN  65

static bool has_gt1_extension(const char *fname)
{
    size_t n = strlen(fname);
    return (n > 4 && strcasecmp(fname + n - 4, ".gt1") == 0) ||
           (n > 5 && strcasecmp(fname + n - 5, ".gt1x") == 0);
}

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
    ESP_LOGI(TAG, "Execution address for gt1 file %s is 0x%02x%02x",
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

/* ── browser directory state (owned by the worker task) ──────────── */

#define DIR_NAME_MAX   22   /* browser packets cap name length */
#define DIR_PAYLOAD    63   /* payload bytes in a listing frame */
#define ENTRY_FILE     1
#define ENTRY_DIR      2
#define ENTRY_ERROR    3

/* The browser listing uses the raw FatFs API (not POSIX readdir) so it can
 * read FILINFO.altname, the 8.3 short name.  Old browsers only understand
 * short names, and the name shown must be the one that round-trips back
 * through OPEN/EXEC.  s_path stays a VFS path for gt1_post_file()/fopen(). */
static FF_DIR  s_dir;
static bool    s_dir_open = false;
static FILINFO s_fno;          /* pending entry, like the old s_dirent */
static bool    s_have_fno = false;
static bool    s_backdir = false;
static bool    s_shortnames = true;
static char    s_path[PLUGGY_PATH_MAX];   /* current dir (VFS path) */

static void wake_browser(void)
{
    /* Release the browser's waitSDCard loop with a 0 byte. */
    gigatron_post_input(0, 0, 0, 0, portMAX_DELAY);
    gigatron_post_input(0xff, 2, 0, 0, portMAX_DELAY);
}

static void dir_close(void)
{
    if (s_dir_open)
        f_closedir(&s_dir);
    s_dir_open = false;
    s_have_fno = false;
}

/* The name to show the browser.
 * Either the long name or the short name, depending
 * on the long name length and the s_shortname override.
 * Flag s_shortname is set during CmdOpen
 * and cleared during CmdInit (otherwise a no-op)
 */
static const char *fno_name(const FILINFO *fi)
{
    if (s_shortnames || strlen(fi->fname) > DIR_NAME_MAX)
        if (fi->altname[0])
            return fi->altname;
    return fi->fname;
}

/* Convert a VFS path under PLUGGY_PROGRAMS_PATH to a FatFs path.  The RO
 * FAT is the only FatFs volume, so it is logical drive 0 and the drive
 * prefix can be omitted.  Returns a pointer into (or past) vfs. */
static const char *fatfs_path(const char *vfs)
{
    size_t base = sizeof PLUGGY_PROGRAMS_PATH - 1;
    if (! strncmp(vfs, PLUGGY_PROGRAMS_PATH, base))
        vfs += base;
    if (*vfs == 0)
        return "/";
    return vfs;
}

static const char *dir_relative(char *buffer, const char *cwd, const char *name)
{
    if (cwd == 0 || cwd[0] == 0)
        cwd = PLUGGY_PROGRAMS_PATH;
    int pos = strlen(cwd);
    if (pos + 1 > PLUGGY_PATH_MAX) {
        ESP_LOGE(TAG, "dir_relative \"%s\" \"%s\" too long", cwd, name);
        return NULL;
    }
    strcpy(buffer, cwd);
    while (pos > 0 && buffer[pos-1]=='/')
        pos -= 1;
    buffer[pos] = 0;
    if (! strcmp(name, ".")) {
        return buffer;
    } else if (!strcmp(name, "..")) {
        while (pos > 0 && buffer[pos-1]!='/')
            pos -= 1;
    } else if (pos + strlen(name) + 2 > PLUGGY_PATH_MAX) {
        ESP_LOGE(TAG, "dir_relative \"%s\" \"%s\" too long", cwd, name);
        return NULL;
    } else {
        buffer[pos++] = '/';
        strcpy(buffer + pos, name);
        pos = strlen(buffer);
    }
    while (pos > 0 && buffer[pos-1]=='/')
        pos -= 1;
    if (pos > 0)
        buffer[pos] = 0;
    return buffer;
}

static void dir_open(const char *name)
{
    char full[PLUGGY_PATH_MAX];
    const char *s;
    dir_close();
    if ((s = dir_relative(full, s_path, name))) {
        FRESULT res = f_opendir(&s_dir, fatfs_path(s));
        ESP_LOGI(TAG, "dir_open('%s') %s", s, res == FR_OK ? "ok" : "fail");
        if (res == FR_OK) {
            strcpy(s_path, s);
            s_dir_open = true;
            s = strrchr(s_path, '/');
            s_backdir = (s && s > s_path);
        }
    }
    if (! s_dir_open) {
        strcpy(s_path, PLUGGY_PROGRAMS_PATH);
        FRESULT res = f_opendir(&s_dir, fatfs_path(s_path));
        ESP_LOGE(TAG, "reverting to dir_open('%s') %s", s_path,
                 res == FR_OK ? "ok" : "fail");
        s_dir_open = (res == FR_OK);
        s_backdir = false;
    }
}

/* Append one <type><len><name><0> entry; return false if it won't fit. */
static bool dir_pack_entry(uint8_t *payload, int *index,
                           int type, const char *name)
{
    int n = strlen(name);
    if (n > DIR_NAME_MAX)
        n = DIR_NAME_MAX;
    if (*index + 2 + n + 1 > DIR_PAYLOAD)
        return false;
    payload[(*index)++] = type;
    payload[(*index)++] = n;
    memcpy(payload + *index, name, n);
    *index += n;
    payload[(*index)++] = 0;
    return true;
}

static void dir_send_listing(void)
{
    uint8_t *frame = malloc(LOADER_FRAMELEN);
    if (! frame) {
        ESP_LOGE(TAG, "Out of memory for directory listing");
        return;
    }
    uint8_t *payload = frame + 2;
    bool exhausted = false;
    int type = ENTRY_ERROR;
    int index = 0;
    memset(frame, 0, LOADER_FRAMELEN);
    for(;;) {
        if (! s_dir_open) {
            exhausted = true;
            dir_pack_entry(payload, &index, ENTRY_ERROR, "SDCard Error");
            break;
        }
        if (s_backdir) {
            dir_pack_entry(payload, &index, ENTRY_DIR, "..");
            s_backdir = false;
        }
        if (! s_have_fno) {
            if (f_readdir(&s_dir, &s_fno) != FR_OK || ! s_fno.fname[0]) {
                exhausted = true;
                f_rewinddir(&s_dir);
                break;
            }
            s_have_fno = true;
        }
        const char *name = fno_name(&s_fno);
        if (s_fno.fname[0] == '.')
            goto skip;
        if (s_fno.fattrib & AM_DIR)
            type = ENTRY_DIR;
        else if (has_gt1_extension(name))
            type = ENTRY_FILE;
        else
            goto skip;
        if (strlen(name) > DIR_NAME_MAX)
            goto skip;
        if (! dir_pack_entry(payload, &index, type, name)) {
            exhausted = false;
            break;
        }
    skip:
        s_have_fno = false;
    }
    frame[0] = exhausted;
    frame[1] = index + (frame[0] << 6);   /* length, 6 bits */
    esp_err_t err = gigatron_post_input(0xff, 0, frame, LOADER_FRAMELEN, portMAX_DELAY);
    ESP_LOGI(TAG, "listing: path='%s' bytes=%d isLast=%d post err=%d",
             s_path, index, exhausted, err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post directory listing");
        free(frame);
    }
}

static void dir_exec(const char *name)
{
    char full[PLUGGY_PATH_MAX];
    const char *s;
    dir_close();
    if ((s = dir_relative(full, s_path, name))) {
        vTaskDelay(pdMS_TO_TICKS(250)); /* wait for loader */
        gt1_post_file(s);
    }
}


/* ── worker task: drains the command queue ───────────────────────── */

static void pluggy_help_screen(void);

static void pluggy_worker_task(void *arg)
{
    pmsg_t m;
    while (1) {
        if (xQueueReceive(cmd_queue, &m, portMAX_DELAY) != pdTRUE)
            continue;
        switch (m.cmd) {
        case CMD_HELP:
            ESP_LOGI(TAG, "worker: HELP");
            pluggy_help_screen();
            break;
        case CMD_LOAD_GT1: {
            ESP_LOGI(TAG, "worker: LOAD_GT1 %s", m.arg);
            char full[PLUGGY_PATH_MAX];
            const char *s;
            if ((s = dir_relative(full, NULL, m.arg)))
                gt1_post_file(s);
            break;
        }
        case CMD_REPLAY:
            ESP_LOGI(TAG, "worker: REPLAY %s", m.arg);
            pluggy_copy_text(m.arg);
            break;
        /* Browser protocol commands */
        case CMD_SD_INIT:
            ESP_LOGI(TAG, "worker: SD_INIT");
            s_shortnames = false;
            break;
        case CMD_SD_BEGIN:
            ESP_LOGI(TAG, "worker: SD_BEGIN");
            s_path[0] = 0;
            wake_browser();
            break;
        case CMD_SD_OPEN:
            ESP_LOGI(TAG, "worker: SD_OPEN '%s'", m.arg);
            dir_open(m.arg);
            s_shortnames = true;
            wake_browser();
            break;
        case CMD_SD_LIST:
            ESP_LOGI(TAG, "worker: SD_LIST");
            dir_send_listing();
            break;
        case CMD_SD_EXEC: {
            ESP_LOGI(TAG, "worker: SD_EXEC %s", m.arg);
            dir_exec(m.arg);
            break;
        }
        case CMD_SD_CLOSE:
            ESP_LOGI(TAG, "worker: SD_CLOSE");
            dir_close();
            break;
        default:
            ESP_LOGW(TAG, "worker: unknown cmd %d", m.cmd);
            break;
        }
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
        if (has_gt1_extension(de->d_name))
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

static void pluggy_help_screen(void)
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
}

void pluggy_handle_ctrl_fn(int fn)
{
    if (uxQueueMessagesWaiting(cmd_queue)) {
        ESP_LOGE(TAG, "command queue is not empty. skipping.");
        return;
    }
    if (fn == 1) {
        /* Ctrl+F1: list the available programs on the Loader screen */
        post_pcmd(CMD_HELP, NULL);
    } else if (fn >= 2 && fn <= 11) {
        /* Ctrl+F2..F11: load the n-th gt1 file from the FAT partition */
        char *path = pluggy_nth_gt1(fn - 1);
        if (path)
            post_pcmd(CMD_LOAD_GT1, path);
        else
            ESP_LOGW(TAG, "No gt1 file #%d in %s", fn - 1, PLUGGY_PROGRAMS_PATH);
    } else if (fn == 12) {
        /* Ctrl+F12: replay the recorded program */
        post_pcmd(CMD_REPLAY, PLUGGY_SAVED_FILE);
    }
}

/* ── init ────────────────────────────────────────────────────────── */


esp_err_t pluggy_init(void)
{
    /* mount spiffs program store */
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
    /* mount fat ro program store */
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
    /* rx queue and task */
    if (! rx_queue)
        rx_queue = xQueueCreate(PLUGGY_RX_QUEUE_SIZE, sizeof(rmsg_t));
    if (! s_pluggy_rx_task_handle) {
        err = xTaskCreatePinnedToCore(pluggy_rx_task,
                                      "pluggy_rx",
                                      PLUGGY_RX_TASK_STACK,
                                      NULL,
                                      PLUGGY_RX_TASK_PRIORITY,
                                      &s_pluggy_rx_task_handle,
                                      PLUGGY_RX_TASK_CORE );
        if (err != pdPASS) {
            ESP_LOGE(TAG, "Failed to create gigatron rx task: %d", err);
            return ESP_FAIL;
        }
    }
    /* cmd queue and task */
    if (! cmd_queue)
        cmd_queue = xQueueCreate(PLUGGY_CMD_QUEUE_SIZE, sizeof(pmsg_t));
    if (! s_gigatron_cmd_task_handle) {
        err = xTaskCreatePinnedToCore(pluggy_worker_task,
                                      "pluggy_work",
                                      PLUGGY_CMD_TASK_STACK,
                                      NULL,
                                      PLUGGY_CMD_TASK_PRIORITY,
                                      &s_gigatron_cmd_task_handle,
                                      PLUGGY_CMD_TASK_CORE );
        if (err != pdPASS) {
            ESP_LOGE(TAG, "Failed to create worker task: %d", err);
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
