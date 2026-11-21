/* Shake the node to take the next dose, from the QMI8658 accelerometer (replaced a double tap on
 * the case: easier for the person using it). Detected here, not by the chip's motion engine (its
 * tap engine never reported anything on this board).
 *
 * Every sample (500 Hz) is examined: the chip queues them in its FIFO and they are read in batches
 * every POLL_MS. Gravity is a slow average of the samples (GRAVITY_SHIFT: ~0.5 s); what is left is
 * motion. A swing is the motion going over SHAKE_MG (after falling under half of it, at least
 * SWING_MIN_MS after the last swing); SHAKE_SWINGS swings within SHAKE_WINDOW_MS are a shake, then
 * COOLDOWN_MS is ignored. Tilting or turning it slowly stays under the threshold; picking it up or a
 * bump on the table makes one to three swings, spread out more.
 *
 * Register sequences (CTRL9 commands, FIFO read) after SensorLib's SensorQMI8658 (lewisxhe/SensorLib,
 * MIT); no code copied. */
#include "imu_shake.h"

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

static const char *TAG = "imu_shake";

#define SHAKE_MG 600
#define SHAKE_SWINGS 4
#define SHAKE_WINDOW_MS 1000
#define SWING_MIN_MS 60 /* a swing's own wobble doesn't count twice */
#define COOLDOWN_MS 2000
#define GRAVITY_SHIFT 8 /* gravity += (sample - gravity) / 256 per sample: ~0.5 s at 500 Hz */

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
static volatile bool s_shake;

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
              && wr(CTRL2, 0x24) == ESP_OK   /* +-8 g, 500 Hz */
              && command(CMD_RST_FIFO) && wr(FIFO_CTRL, FIFO_STREAM_64) == ESP_OK &&
              wr(CTRL7, 0x01) == ESP_OK;     /* accelerometer only */
    if (!ok) {
        ESP_LOGE(TAG, "setup failed");
    }
    return ok;
}

/* One sample at time t (ms); NULL: invalid (the chip's -32768 marker), skipped */
static void sample(const int32_t s[3], int64_t t)
{
    static const int64_t shake_lsb = (int64_t)SHAKE_MG * LSB_PER_G / 1000;
    static int32_t gravity[3]; /* << GRAVITY_SHIFT */
    static bool have_gravity, over;
    static int64_t swings[SHAKE_SWINGS], quiet_until;
    static int n_swings;
    if (!s) {
        return;
    }
    if (!have_gravity) {
        for (int i = 0; i < 3; i++) {
            gravity[i] = s[i] << GRAVITY_SHIFT;
        }
        have_gravity = true;
    }
    int64_t d2 = 0;
    for (int i = 0; i < 3; i++) {
        gravity[i] += s[i] - (gravity[i] >> GRAVITY_SHIFT);
        int32_t d = s[i] - (gravity[i] >> GRAVITY_SHIFT);
        d2 += (int64_t)d * d;
    }
    if (over) {
        over = d2 > shake_lsb * shake_lsb / 4; /* back under half: ready for the next swing */
        return;
    }
    if (d2 <= shake_lsb * shake_lsb || t < quiet_until) {
        return;
    }
    over = true;
    if (n_swings && t - swings[n_swings - 1] < SWING_MIN_MS) {
        return;
    }
    /* Keep the swings within the window, add this one */
    int k = 0;
    for (int i = 0; i < n_swings; i++) {
        if (t - swings[i] <= SHAKE_WINDOW_MS) {
            swings[k++] = swings[i];
        }
    }
    n_swings = k < SHAKE_SWINGS ? k : SHAKE_SWINGS - 1;
    swings[n_swings++] = t;
    ESP_LOGI(TAG, "swing %d of %d", n_swings, SHAKE_SWINGS);
    if (n_swings >= SHAKE_SWINGS) {
        ESP_LOGI(TAG, "shake");
        s_shake = true;
        n_swings = 0;
        quiet_until = t + COOLDOWN_MS;
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
    ESP_LOGI(TAG, "shake: %d swings over %d mg within %d ms", SHAKE_SWINGS, SHAKE_MG, SHAKE_WINDOW_MS);
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

void imu_shake_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(task, "imu_shake", 3072, NULL, 2, NULL, tskNO_AFFINITY, MEM_CAPS_BULK) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

bool imu_shake_take(void)
{
    bool d = s_shake;
    s_shake = false;
    return d;
}

#else

void imu_shake_start(void) {}

bool imu_shake_take(void)
{
    return false;
}

#endif
