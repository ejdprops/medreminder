/* Panel side of the one-wire link to the XIAO C6 node (protocol: firmware/main/uart_link.c,
 * layouts: uart_link_proto.h). IO38 (GPIO_D, yellow wire), open-drain with pull-up, UART1 TX and
 * RX on the same pad: every frame we send comes back to us and is checked, then the node's reply
 * follows. Polls every POLL_MS; a take (undo) goes out as TAKE (UNTAKE) instead of the next POLL. */
#include "link.h"

#include <string.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>

static const char *TAG = "link";

#define PIN GPIO_NUM_38
#define PORT UART_NUM_1
#define POLL_MS 200
#define UP_MS 1000

static SemaphoreHandle_t s_lock;
static link_view_t s_view;
static bool s_doses_changed;
static uint8_t s_take_uid; /* 0 = none */
static bool s_take_undo;
static int64_t s_last_ok_ms;
static uint8_t s_buf[LINK_HDR_LEN + sizeof(link_status_t) + sizeof(doses_snapshot_t) + LINK_CRC_LEN];

static bool read_n(uint8_t *p, int n, int ms)
{
    return uart_read_bytes(PORT, p, n, pdMS_TO_TICKS(ms)) == n;
}

/* Send one frame, check its echo, read the reply into s_buf; payload length, or -1 (why says why) */
static int transact(uint8_t type, const uint8_t *payload, int n, const char **why)
{
    uint8_t f[LINK_HDR_LEN + 8 + LINK_CRC_LEN];
    f[0] = LINK_SOF;
    f[1] = type;
    f[2] = n;
    f[3] = 0;
    memcpy(f + LINK_HDR_LEN, payload, n);
    uint32_t crc = esp_rom_crc32_le(0, f + 1, LINK_HDR_LEN - 1 + n);
    memcpy(f + LINK_HDR_LEN + n, &crc, LINK_CRC_LEN);
    int total = LINK_HDR_LEN + n + LINK_CRC_LEN;
    uart_flush_input(PORT);
    uart_write_bytes(PORT, f, total);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(100));
    uint8_t echo[sizeof(f)];
    if (!read_n(echo, total, 20) || memcmp(echo, f, total) != 0) {
        *why = "own bytes not heard back";
        return -1;
    }
    if (!read_n(s_buf, 1, 50)) {
        *why = "no reply";
        return -1;
    }
    if (s_buf[0] != LINK_SOF || !read_n(s_buf + 1, LINK_HDR_LEN - 1, 20)) {
        *why = "bad start";
        return -1;
    }
    int len = s_buf[2] | s_buf[3] << 8;
    if (s_buf[1] != LINK_MSG_REPLY || len < (int)sizeof(link_status_t) ||
        len > (int)(sizeof(s_buf) - LINK_HDR_LEN - LINK_CRC_LEN) ||
        !read_n(s_buf + LINK_HDR_LEN, len + LINK_CRC_LEN, 20 + len / 10)) {
        *why = "short or odd reply";
        return -1;
    }
    memcpy(&crc, s_buf + LINK_HDR_LEN + len, LINK_CRC_LEN);
    if (esp_rom_crc32_le(0, s_buf + 1, LINK_HDR_LEN - 1 + len) != crc) {
        *why = "bad CRC";
        return -1;
    }
    return len;
}

static void task(void *arg)
{
    (void)arg;
    static doses_snapshot_t snap; /* static: off the task stack */
    uint32_t crc = 0;             /* of the doses we hold; 0 = none */
    int fails_logged = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint8_t p[5];
        memcpy(p, &crc, 4);
        uint8_t type = LINK_MSG_POLL;
        int n = 4;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_take_uid) {
            p[4] = s_take_uid;
            s_take_uid = 0;
            type = s_take_undo ? LINK_MSG_UNTAKE : LINK_MSG_TAKE;
            n = 5;
        }
        xSemaphoreGive(s_lock);
        if (type != LINK_MSG_POLL) {
            ESP_LOGI(TAG, "%s dose uid %u", type == LINK_MSG_TAKE ? "take" : "undo", p[4]);
        }
        const char *why = "";
        int len = transact(type, p, n, &why);
        int64_t now_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (len < 0) {
            s_view.failed++;
            if (fails_logged < 5 || s_view.failed % 100 == 0) {
                fails_logged++;
                ESP_LOGW(TAG, "poll failed: %s (%lu ok, %lu failed)", why, (unsigned long)s_view.ok,
                         (unsigned long)s_view.failed);
            }
        } else {
            s_view.ok++;
            s_last_ok_ms = now_ms;
            memcpy(&s_view.status, s_buf + LINK_HDR_LEN, sizeof(link_status_t));
            int body = len - (int)sizeof(link_status_t);
            if (body > 0) {
                memset(&snap, 0, sizeof(snap));
                memcpy(&snap, s_buf + LINK_HDR_LEN + sizeof(link_status_t),
                       body < (int)sizeof(snap) ? body : (int)sizeof(snap));
                if (snap.count <= DOSE_MAX && body == (int)DOSES_SNAPSHOT_LEN(snap.count)) {
                    crc = esp_rom_crc32_le(0, (const uint8_t *)&snap, DOSES_SNAPSHOT_LEN(snap.count));
                    s_view.doses = snap;
                    s_view.have_doses = true;
                    s_doses_changed = true;
                    ESP_LOGI(TAG, "doses: %u, date %lu, taken 0x%02lx", snap.count, (unsigned long)snap.date,
                             (unsigned long)snap.taken);
                } else {
                    ESP_LOGW(TAG, "dose state of %d bytes doesn't fit %u doses", body, snap.count);
                }
            }
        }
        s_view.up = s_last_ok_ms && now_ms - s_last_ok_ms < UP_MS;
        xSemaphoreGive(s_lock);
    }
}

void link_start(void)
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
    ESP_ERROR_CHECK(uart_driver_install(PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
    /* Pad first (gpio_set_direction routes the plain GPIO signal to it), then UART1 TX and RX onto
     * the same pad through the GPIO matrix */
    gpio_reset_pin(PIN);
    gpio_set_pull_mode(PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    esp_rom_gpio_connect_out_signal(PIN, U1TXD_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(PIN, U1RXD_IN_IDX, false);
    xTaskCreate(task, "link", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "one-wire link on IO38, polling every %d ms", POLL_MS);
}

bool link_get(link_view_t *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *v = s_view;
    bool changed = s_doses_changed;
    s_doses_changed = false;
    s_view.up = s_last_ok_ms && esp_timer_get_time() / 1000 - s_last_ok_ms < UP_MS;
    v->up = s_view.up;
    xSemaphoreGive(s_lock);
    return changed;
}

void link_take(uint8_t uid, bool take)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_take_uid = uid;
    s_take_undo = !take;
    xSemaphoreGive(s_lock);
}
