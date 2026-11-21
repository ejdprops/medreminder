/* Single-wire UART link to a dashboard panel (CrowPanel ESP32-S3 on its GPIO_D Grove port). One
 * wire, half duplex, 115200 8N1: both ends run the pad open-drain with a pull-up and route UART TX
 * and RX onto it, so each side also hears its own bytes and drops them. The panel asks, this node
 * only answers.
 *
 * Frame (uart_link_proto.h, shared with the panel): 0xA5, type, length (2 bytes LE), payload,
 * CRC32 (LE, esp_rom_crc32_le over type, length and payload).
 *   POLL  panel -> node, every ~200 ms: u32 CRC of the dose state the panel holds (0: none).
 *   TAKE  panel -> node: u32 CRC as in POLL, u8 dose uid. Sent on to the main unit like a double
 *         tap on a round node (node_sync_request_take); the new state comes back in later replies.
 *   UNTAKE panel -> node: as TAKE, undoes it.
 *   REPLY node -> panel, to both: link_status_t, then the doses_snapshot_t (up to defs[count]) when
 *         the panel's CRC differs from this node's.
 * The panel has no network: its clock comes from REPLY (UTC seconds; it applies the same TZ). */
#include "uart_link.h"

#include <sdkconfig.h>

#if CONFIG_MEDREMINDER_UART_LINK_GPIO >= 0

#include <string.h>
#include <time.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_rom_gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>

#include "mem_caps.h"
#include "node_sync.h"
#include "time_sync.h"
#include "uart_link_proto.h"

static const char *TAG = "uart_link";

#define PIN CONFIG_MEDREMINDER_UART_LINK_GPIO
#define PORT UART_NUM_1
#define BYTE_TIMEOUT_MS 20 /* a frame's bytes arrive back to back: a gap this long ends it */
#define HDR_LEN LINK_HDR_LEN
#define MAX_PAYLOAD (sizeof(link_status_t) + sizeof(doses_snapshot_t))

static SemaphoreHandle_t s_lock;
static doses_snapshot_t s_mine; /* doses task -> here */
static uint32_t s_mine_crc;
static uint8_t s_frame[HDR_LEN + MAX_PAYLOAD + 4];

void uart_link_publish(const doses_snapshot_t *snap)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (memcmp(&s_mine, snap, sizeof(*snap)) != 0) {
        s_mine = *snap;
        s_mine_crc = esp_rom_crc32_le(0, (const uint8_t *)snap, DOSES_SNAPSHOT_LEN(snap->count));
    }
    xSemaphoreGive(s_lock);
}

static bool read_exact(uint8_t *buf, int len)
{
    return uart_read_bytes(PORT, buf, len, pdMS_TO_TICKS(BYTE_TIMEOUT_MS + len / 10)) == len;
}

/* Next frame from the panel; its type and payload length, payload in s_frame + HDR_LEN */
static bool read_frame(uint8_t *type, int *len)
{
    uint8_t b;
    if (uart_read_bytes(PORT, &b, 1, portMAX_DELAY) != 1 || b != LINK_SOF) {
        return false;
    }
    if (!read_exact(s_frame, HDR_LEN - 1)) {
        return false;
    }
    int n = s_frame[1] | s_frame[2] << 8;
    if (n > (int)MAX_PAYLOAD || !read_exact(s_frame + HDR_LEN - 1, n + 4)) {
        return false;
    }
    uint32_t crc;
    memcpy(&crc, s_frame + HDR_LEN - 1 + n, 4);
    if (esp_rom_crc32_le(0, s_frame, HDR_LEN - 1 + n) != crc) {
        ESP_LOGW(TAG, "bad CRC (type 0x%02x, %d bytes)", s_frame[0], n);
        return false;
    }
    *type = s_frame[0];
    *len = n;
    memmove(s_frame + HDR_LEN, s_frame + HDR_LEN - 1, n); /* payload where reply() builds its own */
    return true;
}

static void reply(uint32_t panel_crc)
{
    link_status_t st = {0};
    time_t now = time(NULL);
    st.time = time_is_valid() ? (uint32_t)now : 0;
    st.flags = node_sync_connected() ? LINK_FLAG_MAIN_CONNECTED : 0;
    int n = sizeof(st);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st.crc = s_mine_crc;
    memcpy(s_frame + HDR_LEN, &st, sizeof(st));
    if (panel_crc != s_mine_crc) {
        memcpy(s_frame + HDR_LEN + n, &s_mine, DOSES_SNAPSHOT_LEN(s_mine.count));
        n += DOSES_SNAPSHOT_LEN(s_mine.count);
    }
    xSemaphoreGive(s_lock);
    s_frame[0] = LINK_SOF;
    s_frame[1] = LINK_MSG_REPLY;
    s_frame[2] = n & 0xff;
    s_frame[3] = n >> 8;
    uint32_t crc = esp_rom_crc32_le(0, s_frame + 1, HDR_LEN - 1 + n);
    memcpy(s_frame + HDR_LEN + n, &crc, 4);
    int total = HDR_LEN + n + 4;
    uart_write_bytes(PORT, s_frame, total);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(100));
    /* Our own bytes came back on the shared wire: drop them */
    static uint8_t echo[64];
    for (int left = total; left > 0;) {
        int got = uart_read_bytes(PORT, echo, left < (int)sizeof(echo) ? left : (int)sizeof(echo),
                                  pdMS_TO_TICKS(BYTE_TIMEOUT_MS));
        if (got <= 0) {
            uart_flush_input(PORT);
            break;
        }
        left -= got;
    }
}

static void task(void *arg)
{
    (void)arg;
    uint32_t polls = 0;
    while (true) {
        uint8_t type;
        int len;
        if (!read_frame(&type, &len)) {
            continue;
        }
        const uint8_t *p = s_frame + HDR_LEN;
        uint32_t panel_crc = 0;
        if (len >= 4) {
            memcpy(&panel_crc, p, 4);
        }
        if ((type == LINK_MSG_TAKE || type == LINK_MSG_UNTAKE) && len >= 5) {
            bool take = type == LINK_MSG_TAKE;
            ESP_LOGI(TAG, "panel %s dose %u", take ? "takes" : "undoes", p[4]);
            node_sync_request_take(p[4], take);
        } else if (type != LINK_MSG_POLL) {
            ESP_LOGW(TAG, "unknown frame 0x%02x", type);
            continue;
        }
        if (++polls == 1) {
            ESP_LOGI(TAG, "panel connected");
        }
        reply(panel_crc);
    }
}

void uart_link_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(PORT, 1024, 0, 0, NULL, 0) != ESP_OK || uart_param_config(PORT, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "UART setup failed");
        return;
    }
    /* Pad first (gpio_set_direction routes the plain GPIO signal to it), then UART TX and RX onto
     * the same pad through the GPIO matrix */
    gpio_reset_pin(PIN);
    gpio_set_pull_mode(PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    esp_rom_gpio_connect_out_signal(PIN, U1TXD_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(PIN, U1RXD_IN_IDX, false);
    if (xTaskCreatePinnedToCoreWithCaps(task, "uart_link", 3072, NULL, 4, NULL, tskNO_AFFINITY, MEM_CAPS_BULK) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    ESP_LOGI(TAG, "single-wire link on GPIO%d, %d baud", PIN, LINK_BAUD);
}

#else

void uart_link_start(void) {}

void uart_link_publish(const doses_snapshot_t *snap)
{
    (void)snap;
}

#endif
