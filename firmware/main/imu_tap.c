/* Double tap on the node's case, from the QMI8658 accelerometer. Detected here, not by the chip's
 * tap engine: on this board it never reported a tap (status stayed 0 while taps showed plainly in
 * the samples).
 *
 * Every sample (500 Hz) is examined: the chip queues them in its FIFO and they are read in batches
 * every POLL_MS (reading only the latest sample each tick saw one in five and missed most taps,
 * whose spike lasts a few ms). A tap is a sharp jump: a sample more than TAP_MG away from the one
 * before, after QUIET_MS without a jump over QUIET_MG (so handling, carrying or
 * shaking doesn't count; the samples of the spike's own rise, ONSET_MS, are not "before"); then DEAD_MS is ignored while the case rings. Slow changes (the board
 * tilting under a finger, being turned) don't count either way: a slow gravity average lagged
 * behind them and turned real taps into "moving". Two taps DTAP_MIN_MS..DTAP_MAX_MS apart are a
 * double tap; closer ones start over.
 *
 * Register sequences (CTRL9 commands, FIFO read) after SensorLib's SensorQMI8658 (lewisxhe/SensorLib,
 * MIT); no code copied. */
#include "imu_tap.h"

#include <sdkconfig.h>

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#endif

#if CONFIG_MEDREMINDER_ROLE_NODE && BSP_CAPS_IMU

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "mem_caps.h"

static const char *TAG = "imu_tap";

#define DTAP_MIN_MS 500
#define DTAP_MAX_MS 2000
#define TAP_MG 150
#define QUIET_MG 80
#define QUIET_MS 100
#define ONSET_MS 20 /* a tap's spike builds over a few samples: those don't count as "moving" */
#define DEAD_MS 150

#define POLL_MS 40
#define SAMPLE_MS 2 /* 500 Hz */
#define FIFO_MAX 64 /* samples, stream mode: the oldest are dropped if a read comes late */

#define LSB_PER_G 4096 /* +-8 g */
#define ADDR 0x6B
#define WHOAMI 0x00
#define CTRL1 0x02
#define CTRL2 0x03
#define CTRL7 0x08
#define CTRL8 0x09
#define CTRL9 0x0A
#define FIFO_CTRL 0x14
#define FIFO_COUNT 0x15 /* + FIFO_STATUS 0x16 bits 1:0 */
#define FIFO_DATA 0x17
#define STATUS_INT 0x2D
#define CMD_ACK 0x00
#define CMD_RST_FIFO 0x04
#define CMD_REQ_FIFO 0x05
#define FIFO_STREAM_64 ((2 << 2) | 2)
#define RST_RESULT 0x4D
#define RESET 0x60

static i2c_master_dev_handle_t s_dev;
static volatile bool s_double;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, sizeof(b), 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

/* CTRL9 protocol: command, wait for CmdDone (STATUS_INT bit 7), acknowledge, wait for it to clear.
 * Done within microseconds: busy-wait (a tick is 10 ms). */
static bool command(uint8_t cmd)
{
    if (wr(CTRL9, cmd) != ESP_OK) {
        return false;
    }
    for (int step = 0; step < 2; step++) {
        int64_t until = esp_timer_get_time() + 20000;
        uint8_t st = 0;
        while (rd(STATUS_INT, &st, 1) == ESP_OK && !(st & 0x80) == !step) {
            if (esp_timer_get_time() > until) {
                ESP_LOGE(TAG, "command 0x%02x: no %s", cmd, step ? "ack" : "done");
                return false;
            }
        }
        if (step == 0 && wr(CTRL9, CMD_ACK) != ESP_OK) {
            return false;
        }
    }
    return true;
}

static bool setup(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000};
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    wr(RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20)); /* NACKs while resetting; up to 15 ms */
    uint8_t v = 0;
    for (int i = 0; i < 20 && (rd(RST_RESULT, &v, 1) != ESP_OK || v != 0x80); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rd(WHOAMI, &v, 1) != ESP_OK || v != 0x05) {
        ESP_LOGE(TAG, "no QMI8658 (id 0x%02x)", v);
        return false;
    }
    bool ok = wr(CTRL1, 0x40) == ESP_OK      /* address auto-increment, little endian */
              && wr(CTRL8, 0x80) == ESP_OK   /* CTRL9 handshake on STATUS_INT bit 7 */
              && wr(CTRL2, 0x24) == ESP_OK   /* +-8 g, 500 Hz (low-pass off: keep the tap's peak) */
              && command(CMD_RST_FIFO) && wr(FIFO_CTRL, FIFO_STREAM_64) == ESP_OK &&
              wr(CTRL7, 0x01) == ESP_OK;     /* accelerometer only */
    if (!ok) {
        ESP_LOGE(TAG, "setup failed");
    }
    return ok;
}

/* d2 (LSB^2) as mg, for the log */
static int to_mg(int64_t d2)
{
    int mg = 0;
    while ((int64_t)mg * mg * LSB_PER_G * LSB_PER_G < d2 * 1000000 && mg < 16000) {
        mg += 25;
    }
    return mg;
}

/* One sample at time t (ms); NULL: invalid (the chip's -32768 marker), compare afresh after it */
static void sample(const int32_t s[3], int64_t t)
{
    static const int64_t tap_lsb = (int64_t)TAP_MG * LSB_PER_G / 1000, quiet_lsb = (int64_t)QUIET_MG * LSB_PER_G / 1000;
    static int32_t prev[3];
    static bool have_prev;
    static int64_t last_tap, busy_until, last_loud, burst_start;
    static bool burst_after_quiet; /* the current run of jumps began after QUIET_MS of calm */
    if (!s) {
        have_prev = false;
        return;
    }
    int64_t d2 = 0;
    for (int i = 0; i < 3; i++) {
        int32_t d = have_prev ? s[i] - prev[i] : 0;
        d2 += (int64_t)d * d;
        prev[i] = s[i];
    }
    have_prev = true;
    if (d2 > quiet_lsb * quiet_lsb) {
        if (t - last_loud >= QUIET_MS) {
            burst_start = t;
            burst_after_quiet = true;
        }
        last_loud = t;
    }
    bool quiet_before = burst_after_quiet && t - burst_start <= ONSET_MS;
    if (t < busy_until || d2 <= tap_lsb * tap_lsb) {
        return;
    }
    busy_until = t + DEAD_MS;
    if (!quiet_before) {
        ESP_LOGI(TAG, "jolt ~%d mg while moving: not a tap", to_mg(d2));
        last_tap = 0;
        return;
    }
    int gap = last_tap ? (int)(t - last_tap) : -1;
    ESP_LOGI(TAG, "tap ~%d mg, %d ms after the last", to_mg(d2), gap);
    if (gap >= DTAP_MIN_MS && gap <= DTAP_MAX_MS) {
        ESP_LOGI(TAG, "double tap");
        s_double = true;
        last_tap = 0;
    } else {
        last_tap = t;
    }
}

/* FIFO: count (in 2-byte words), request read mode, read, back to filling */
static int read_fifo(uint8_t *buf, int max_bytes)
{
    uint8_t c[2];
    if (rd(FIFO_COUNT, c, 2) != ESP_OK) {
        return 0;
    }
    int bytes = 2 * (((c[1] & 0x03) << 8) | c[0]);
    bytes -= bytes % 6;
    bytes = bytes > max_bytes ? max_bytes : bytes;
    if (bytes == 0 || !command(CMD_REQ_FIFO)) {
        return 0;
    }
    esp_err_t err = rd(FIFO_DATA, buf, bytes);
    wr(FIFO_CTRL, FIFO_STREAM_64); /* clears read mode */
    return err == ESP_OK ? bytes : 0;
}

static void task(void *arg)
{
    (void)arg;
    if (!setup()) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ESP_LOGI(TAG, "double tap: %d-%d ms apart", DTAP_MIN_MS, DTAP_MAX_MS);
    static uint8_t buf[FIFO_MAX * 6];
    /* The first read after power-up holds junk (-32768s, a half-written sample): skip a while */
    int64_t start = esp_timer_get_time() / 1000 + 300;
    int reads = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int n = read_fifo(buf, sizeof(buf)) / 6;
        if (++reads == 25) {
            ESP_LOGI(TAG, "%d samples per read (expect ~%d)", n, POLL_MS / SAMPLE_MS);
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now < start) {
            continue;
        }
        for (int k = 0; k < n; k++) {
            const uint8_t *a = buf + k * 6;
            int32_t s[3] = {(int16_t)(a[0] | a[1] << 8), (int16_t)(a[2] | a[3] << 8), (int16_t)(a[4] | a[5] << 8)};
            sample(s[0] == INT16_MIN && s[1] == INT16_MIN && s[2] == INT16_MIN ? NULL : s,
                   now - (int64_t)(n - 1 - k) * SAMPLE_MS);
        }
    }
}

void imu_tap_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(task, "imu_tap", 3072, NULL, 2, NULL, tskNO_AFFINITY, MEM_CAPS_BULK) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

bool imu_tap_take_double(void)
{
    bool d = s_double;
    s_double = false;
    return d;
}

#else

void imu_tap_start(void) {}

bool imu_tap_take_double(void)
{
    return false;
}

#endif
