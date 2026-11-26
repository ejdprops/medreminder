/* App link: the Did I? app (apple/, Med Dot in code) on an iPhone, iPad or Apple TV reads and changes this unit's
 * settings over IP. The phone is on the home Wi-Fi and the unit on Thread; the Apple border routers
 * route between them (the unit's OMR address) and publish the unit's SRP service, so the app finds
 * it as _meddot._udp. Wire format and crypto: app_link_proto.h.
 *
 * Anyone on the home network can reach the port, so nothing is accepted unsigned: an app pairs once
 * (ECDH; the unit shows a 6-digit code and someone taps Pair on it) and signs every request with
 * the key from that. Up to MAX_CLIENTS pairings in NVS (app_link/clients); a new one past that
 * replaces the oldest.
 *
 * The app may change only what all devices share (the overdue time; the dose schedule next). Dark
 * mode, brightness, volume and screen off stay local to each device.
 *
 * Threads: this module's task owns the socket and the crypto; it takes the display lock to show the
 * pairing dialog and to apply settings (the same setters the Settings screen uses). The dialog's
 * buttons answer through app_link_pair_answer (LVGL context). */
#include "app_link.h"

#include "sdkconfig.h"

#if CONFIG_MEDREMINDER_ROLE_MAIN && __has_include("lvgl.h") && __has_include("bsp/esp-bsp.h")

#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_netif_glue.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <nvs.h>
#include <openthread/srp_client.h>

#include "app_link_proto.h"
#include "bsp/esp-bsp.h"
#include "doses.h"
#include "fonts/fonts.h"
#include "mem_caps.h"
#include "ui_doses.h"
#include "ui_settings.h"
#include "ui_sleep.h"
#include "ui_tap.h"
#include "ui_theme.h"

static const char *TAG = "app_link";

#define MAX_CLIENTS 6
#define PAIR_TIMEOUT_S 60   /* the dialog closes and the pairing is dropped after this */
#define SRP_CHECK_S 30      /* re-add the SRP service if Matter's SRP handling removed it */
#define NVS_NS "app_link"

typedef struct {
    uint8_t used;
    uint8_t id;
    char name[APP_NAME_MAX + 1];
    uint8_t key[32];
    uint32_t counter; /* last counter seen (saved on SET / FORGET; GETs only in RAM) */
    uint32_t paired;  /* pairing order, to replace the oldest */
} client_t;

typedef enum { PAIR_NONE, PAIR_WAITING, PAIR_APPROVED, PAIR_REFUSED } pair_state_t;

typedef struct {
    pair_state_t state;
    uint8_t app_pub[APP_PUB_LEN];
    uint8_t unit_pub[APP_PUB_LEN];
    uint8_t key[32];
    uint32_t code;
    char name[APP_NAME_MAX + 1];
    uint8_t client; /* id given once approved */
    TickType_t started;
} pairing_t;

static SemaphoreHandle_t s_lock; /* s_pair, s_clients */
static client_t s_clients[MAX_CLIENTS];
static uint8_t s_next_id;
static uint32_t s_pair_seq;
static pairing_t s_pair;
static int s_sock = -1;
static uint8_t s_buf[512]; /* largest: a RESP with 16 doses (~370 bytes) */
static lv_obj_t *s_dialog; /* LVGL objects: display lock */

/* ---- NVS ---- */

static void clients_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_clients);
    if (nvs_get_blob(h, "clients", s_clients, &len) != ESP_OK || len != sizeof(s_clients)) {
        memset(s_clients, 0, sizeof(s_clients));
    }
    nvs_get_u8(h, "next_id", &s_next_id);
    nvs_get_u32(h, "seq", &s_pair_seq);
    nvs_close(h);
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        n += s_clients[i].used;
    }
    ESP_LOGI(TAG, "%d paired app(s)", n);
}

static void clients_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "clients", s_clients, sizeof(s_clients));
    nvs_set_u8(h, "next_id", s_next_id);
    nvs_set_u32(h, "seq", s_pair_seq);
    nvs_commit(h);
    nvs_close(h);
}

static client_t *client_find(uint8_t id)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].id == id) {
            return &s_clients[i];
        }
    }
    return NULL;
}

/* ---- Crypto ---- */

static void mac16(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t out[APP_MAC_LEN])
{
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, data, len, full);
    memcpy(out, full, APP_MAC_LEN);
}

static bool mac_ok(const uint8_t key[32], const uint8_t *data, size_t len, const uint8_t *mac)
{
    uint8_t want[APP_MAC_LEN];
    mac16(key, data, len, want);
    uint8_t diff = 0;
    for (int i = 0; i < APP_MAC_LEN; i++) {
        diff |= want[i] ^ mac[i];
    }
    return diff == 0;
}

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* New unit key pair, the shared key and the code for this app key. False if app_pub isn't a valid
 * P-256 point. */
static bool derive(const uint8_t app_pub[APP_PUB_LEN], uint8_t unit_pub[APP_PUB_LEN], uint8_t key[32],
                   uint32_t *code)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q, peer;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_point_init(&peer);
    bool ok = false;
    size_t olen = 0;
    uint8_t shared[32], okm[36], info[2 * APP_PUB_LEN];
    static const char salt[] = "meddot-pair-v1";
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_ecp_point_read_binary(&grp, &peer, app_pub, APP_PUB_LEN) != 0 ||
        mbedtls_ecp_check_pubkey(&grp, &peer) != 0 || mbedtls_ecp_gen_keypair(&grp, &d, &q, rng, NULL) != 0 ||
        mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, unit_pub, APP_PUB_LEN) != 0 ||
        olen != APP_PUB_LEN || mbedtls_ecdh_compute_shared(&grp, &z, &peer, &d, rng, NULL) != 0 ||
        mbedtls_mpi_write_binary(&z, shared, sizeof(shared)) != 0) {
        goto out;
    }
    memcpy(info, app_pub, APP_PUB_LEN);
    memcpy(info + APP_PUB_LEN, unit_pub, APP_PUB_LEN);
    if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)salt, sizeof(salt) - 1, shared,
                     sizeof(shared), info, sizeof(info), okm, sizeof(okm)) != 0) {
        goto out;
    }
    memcpy(key, okm, 32);
    *code = (okm[32] | okm[33] << 8 | okm[34] << 16 | (uint32_t)okm[35] << 24) % 1000000;
    ok = true;
out:
    memset(shared, 0, sizeof(shared));
    memset(okm, 0, sizeof(okm));
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_point_free(&peer);
    return ok;
}

/* ---- Pairing dialog (display lock held) ---- */

static void dialog_close(void)
{
    if (s_dialog) {
        lv_obj_delete_async(s_dialog);
        s_dialog = NULL;
    }
}

static lv_obj_t *dlg_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *dlg_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t border, uint32_t fg)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 40);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_darken(lv_color_hex(bg), LV_OPA_10), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &inter_14_semibold, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    return b;
}

static void cancel_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    app_link_pair_answer(false);
}

static void pair_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    app_link_pair_answer(true);
}

/* "Pair EDITH with this Pill Reminder?" / "123 456" / "Check the app shows this code" / [Cancel] [Pair] */
static void dialog_open(const char *name, uint32_t code)
{
    dialog_close();
    ui_sleep_wake("app pairing");
    s_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE); /* taps outside the panel go nowhere */

    lv_obj_t *panel = lv_obj_create(s_dialog);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 260, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_T->bg), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char text[80];
    snprintf(text, sizeof(text), "Pair %s with\nthis Pill Reminder?", name);
    dlg_label(panel, text, &inter_14_semibold, UI_T->text);
    snprintf(text, sizeof(text), "%03lu %03lu", (unsigned long)(code / 1000), (unsigned long)(code % 1000));
    dlg_label(panel, text, &inter_30_semibold, UI_T->text);
    dlg_label(panel, "Pair only if the app shows this code", &inter_11_semibold, UI_T->muted);
    ui_on_tap(dlg_button(panel, "Cancel", UI_T->btn_bg, UI_T->btn_border, UI_T->text2), cancel_cb, NULL);
    ui_on_tap(dlg_button(panel, "Pair", 0x1d4ed8, 0x1d4ed8, 0xffffff), pair_cb, NULL);
}

void app_link_pair_answer(bool approve)
{
    dialog_close(); /* LVGL context: the display lock is held */
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pair.state == PAIR_WAITING) {
        if (approve) {
            /* Free slot, or replace the oldest pairing */
            client_t *c = &s_clients[0];
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!s_clients[i].used) {
                    c = &s_clients[i];
                    break;
                }
                if (s_clients[i].paired < c->paired) {
                    c = &s_clients[i];
                }
            }
            do {
                s_next_id++;
            } while (s_next_id == 0 || client_find(s_next_id));
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->id = s_next_id;
            strlcpy(c->name, s_pair.name, sizeof(c->name));
            memcpy(c->key, s_pair.key, sizeof(c->key));
            c->paired = ++s_pair_seq;
            clients_save();
            s_pair.client = c->id;
            s_pair.state = PAIR_APPROVED;
            ESP_LOGI(TAG, "paired \"%s\" as app %u", c->name, c->id);
        } else {
            s_pair.state = PAIR_REFUSED;
            ESP_LOGI(TAG, "pairing cancelled");
        }
        s_pair.started = xTaskGetTickCount(); /* answer repeats of PAIR for a while longer */
    }
    xSemaphoreGive(s_lock);
}

/* ---- Shared settings (display lock held) ----
 * Only what every device shares lives here (the overdue time); dark mode, brightness, volume and the
 * screen-off timer stay local to each device and the app never changes the unit's. */

static const uint8_t s_overdue_values[] = {5, 10, 15, 20, 30, 45, 60, 90, 120};

static bool one_of(uint8_t v, const uint8_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (list[i] == v) {
            return true;
        }
    }
    return false;
}

static void settings_read(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = 1;
    s->overdue = (uint8_t)doses_overdue_min();
}

static bool settings_apply(const app_settings_t *s)
{
    uint8_t m = s->mask;
    if ((m & ~APP_SET_OVERDUE) || ((m & APP_SET_OVERDUE) && !one_of(s->overdue, s_overdue_values,
                                                                     sizeof(s_overdue_values)))) {
        return false;
    }
    if (m & APP_SET_OVERDUE) {
        doses_set_overdue_min(s->overdue);
        ui_settings_changed();
        ESP_LOGI(TAG, "overdue after %u min (from the app)", s->overdue);
    }
    return true;
}

/* ---- Dose schedule (display lock held) ---- */

static size_t doses_write(uint8_t *out)
{
    int n = doses_count();
    out[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        const dose_def_t *d = dose_def(i);
        app_dose_t a = {.uid = d->uid, .minute = (uint16_t)d->minute, .days = d->days, .color = d->color};
        strncpy(a.name, d->name, sizeof(a.name)); /* NUL-padded; DOSE_NAME_MAX 15 keeps a NUL */
        memcpy(out + 1 + i * sizeof(a), &a, sizeof(a));
    }
    return 1 + n * sizeof(app_dose_t);
}

static size_t refused(uint8_t *out, const char *why)
{
    size_t n = strlen(why);
    memcpy(out, why, n);
    return n;
}

/* The schedule changed: the home screen redraws on the next doses_tick (doses_set/delete mark it);
 * an open Doses list redraws now */
static void doses_changed(const char *what, const app_dose_t *a)
{
    ui_doses_changed();
    ESP_LOGI(TAG, "%s %.16s %02d%02d from the app", what, a ? a->name : "", a ? a->minute / 60 : 0,
             a ? a->minute % 60 : 0);
}

static uint8_t handle_op(uint8_t op, const uint8_t *body, int body_len, uint8_t *out, size_t *out_len)
{
    switch (op) {
    case APP_OP_GET:
    case APP_OP_SET: {
        app_settings_t s = {0};
        uint8_t st = APP_ST_OK;
        if (op == APP_OP_SET) {
            if (body_len != sizeof(s)) {
                st = APP_ST_BAD_VALUE;
            } else {
                memcpy(&s, body, sizeof(s));
                if (!settings_apply(&s)) {
                    st = APP_ST_BAD_VALUE;
                }
            }
        }
        settings_read(&s);
        memcpy(out, &s, sizeof(s));
        *out_len = sizeof(s);
        return st;
    }
    case APP_OP_GET_DOSES:
        *out_len = doses_write(out);
        return APP_ST_OK;
    case APP_OP_SET_DOSE:
    case APP_OP_DELETE_DOSE: {
        if (ui_doses_editing()) {
            *out_len = refused(out, "Someone is editing doses on the C5");
            return APP_ST_REFUSED;
        }
        if (op == APP_OP_DELETE_DOSE) {
            int i = body_len == 1 ? doses_find_uid(body[0]) : -1;
            if (i < 0) {
                return APP_ST_BAD_VALUE;
            }
            app_dose_t a = {.minute = (uint16_t)dose_def(i)->minute};
            strncpy(a.name, dose_def(i)->name, sizeof(a.name));
            doses_delete(i);
            doses_changed("deleted", &a);
            *out_len = doses_write(out);
            return APP_ST_OK;
        }
        app_dose_t a;
        if (body_len != sizeof(a)) {
            return APP_ST_BAD_VALUE;
        }
        memcpy(&a, body, sizeof(a));
        a.name[sizeof(a.name) - 1] = 0;
        bool one_day = a.days && !(a.days & (a.days - 1)) && a.days < 0x80;
        if (a.minute >= 24 * 60 || a.minute % 5 || !(a.days == DOSE_DAILY || one_day) || a.color >= DOSE_COLOR_COUNT) {
            return APP_ST_BAD_VALUE;
        }
        int index = -1;
        if (a.uid) {
            index = doses_find_uid(a.uid);
            if (index < 0) {
                *out_len = refused(out, "That dose was deleted");
                return APP_ST_REFUSED;
            }
        }
        dose_def_t d = {0};
        /* The name without leading/trailing spaces, ASCII only (the fonts) */
        const char *p = a.name;
        while (*p == ' ') {
            p++;
        }
        strlcpy(d.name, p, sizeof(d.name));
        for (int n = strlen(d.name); n > 0 && d.name[n - 1] == ' '; n--) {
            d.name[n - 1] = 0;
        }
        for (char *c = d.name; *c; c++) {
            if (*c < 0x20 || *c > 0x7e) {
                *c = '?';
            }
        }
        d.minute = a.minute;
        d.color = a.color;
        d.opacity = 100;
        d.days = a.days;
        strlcpy(d.period, dose_period_for(d.minute), sizeof(d.period));
        const char *err = doses_set(index, &d);
        if (err) {
            *out_len = refused(out, err);
            return APP_ST_REFUSED;
        }
        doses_changed(a.uid ? "changed" : "added", &a);
        *out_len = doses_write(out);
        return APP_ST_OK;
    }
    default:
        return APP_ST_BAD_OP;
    }
}

/* ---- Messages ---- */

static void reply(const struct sockaddr_in6 *to, size_t len)
{
    sendto(s_sock, s_buf, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static size_t put_hdr(uint8_t type)
{
    s_buf[0] = 'M';
    s_buf[1] = 'A';
    s_buf[2] = APP_LINK_VERSION;
    s_buf[3] = type;
    return 4;
}

static void on_pair(const uint8_t *p, int len, const struct sockaddr_in6 *from)
{
    if (len < APP_PUB_LEN + 1 || len < APP_PUB_LEN + 1 + p[APP_PUB_LEN]) {
        return;
    }
    char name[APP_NAME_MAX + 1] = "";
    int nlen = p[APP_PUB_LEN] > APP_NAME_MAX ? APP_NAME_MAX : p[APP_PUB_LEN];
    for (int i = 0; i < nlen; i++) {
        char c = (char)p[APP_PUB_LEN + 1 + i];
        name[i] = (c >= 0x20 && c < 0x7f) ? c : '?'; /* the fonts are ASCII */
    }
    name[nlen] = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_pair.state != PAIR_NONE && memcmp(s_pair.app_pub, p, APP_PUB_LEN) == 0;
    bool busy = s_pair.state == PAIR_WAITING && !same;
    bool show = false;
    uint32_t code = 0;
    if (!same && !busy) {
        memset(&s_pair, 0, sizeof(s_pair));
        memcpy(s_pair.app_pub, p, APP_PUB_LEN);
        strlcpy(s_pair.name, name, sizeof(s_pair.name));
        if (derive(s_pair.app_pub, s_pair.unit_pub, s_pair.key, &s_pair.code)) {
            s_pair.state = PAIR_WAITING;
            s_pair.started = xTaskGetTickCount();
            show = true;
            code = s_pair.code;
            ESP_LOGI(TAG, "pairing request from \"%s\"", name);
        } else {
            ESP_LOGW(TAG, "pairing request with a bad key");
        }
    }
    size_t n = put_hdr(busy || s_pair.state == PAIR_NONE    ? APP_MSG_PAIR_NO
                       : s_pair.state == PAIR_WAITING        ? APP_MSG_PAIR_WAIT
                       : s_pair.state == PAIR_APPROVED       ? APP_MSG_PAIR_OK
                                                             : APP_MSG_PAIR_NO);
    if (busy || s_pair.state == PAIR_NONE) {
        memset(s_buf + n, 0, APP_PUB_LEN);
    } else {
        memcpy(s_buf + n, s_pair.unit_pub, APP_PUB_LEN);
    }
    n += APP_PUB_LEN;
    if (!busy && s_pair.state == PAIR_APPROVED) {
        s_buf[n++] = s_pair.client;
        mac16(s_pair.key, s_buf, n, s_buf + n);
        n += APP_MAC_LEN;
    }
    xSemaphoreGive(s_lock);
    reply(from, n);

    if (show && bsp_display_lock(UINT32_MAX)) {
        dialog_open(name, code);
        bsp_display_unlock();
    }
}

static void on_request(int len, const struct sockaddr_in6 *from)
{
    /* hdr 4, client 1, counter 4, op 1, body, mac 16 */
    if (len < 4 + 1 + 4 + 1 + APP_MAC_LEN) {
        return;
    }
    uint8_t id = s_buf[4];
    uint32_t counter = s_buf[5] | s_buf[6] << 8 | s_buf[7] << 16 | (uint32_t)s_buf[8] << 24;
    uint8_t op = s_buf[9];
    const uint8_t *body = s_buf + 10;
    int body_len = len - 10 - APP_MAC_LEN;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    client_t *c = client_find(id);
    if (!c) {
        xSemaphoreGive(s_lock);
        size_t n = put_hdr(APP_MSG_RESP);
        s_buf[n++] = id;
        memcpy(s_buf + n, s_buf + 5, 4); /* counter: still in place */
        n += 4;
        s_buf[n++] = op;
        s_buf[n++] = APP_ST_UNKNOWN;
        reply(from, n);
        return;
    }
    if (!mac_ok(c->key, s_buf, len - APP_MAC_LEN, s_buf + len - APP_MAC_LEN) || counter <= c->counter) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "dropped a request from app %u (bad MAC or old counter)", id);
        return;
    }
    c->counter = counter;
    uint8_t key[32];
    memcpy(key, c->key, sizeof(key));
    bool save = op != APP_OP_GET && op != APP_OP_GET_DOSES; /* the counter: reads only in RAM */
    if (op == APP_OP_FORGET) {
        ESP_LOGI(TAG, "app %u (\"%s\") unpaired itself", c->id, c->name);
        memset(c, 0, sizeof(*c));
    }
    if (save) {
        clients_save();
    }
    xSemaphoreGive(s_lock);

    /* Everything below touches UI-task state (settings, the schedule): under the display lock */
    static uint8_t out[1 + DOSE_MAX * sizeof(app_dose_t)];
    size_t out_len = 0;
    uint8_t status = APP_ST_OK;
    if (op != APP_OP_FORGET && bsp_display_lock(UINT32_MAX)) {
        status = handle_op(op, body, body_len, out, &out_len);
        bsp_display_unlock();
    }

    size_t n = put_hdr(APP_MSG_RESP);
    s_buf[n++] = id;
    s_buf[n++] = counter & 0xff;
    s_buf[n++] = counter >> 8 & 0xff;
    s_buf[n++] = counter >> 16 & 0xff;
    s_buf[n++] = counter >> 24 & 0xff;
    s_buf[n++] = op;
    s_buf[n++] = status;
    memcpy(s_buf + n, out, out_len);
    n += out_len;
    mac16(key, s_buf, n, s_buf + n);
    n += APP_MAC_LEN;
    memset(key, 0, sizeof(key));
    reply(from, n);
}

/* ---- SRP: advertise _meddot._udp through the border routers ---- */

/* Matter's SRP callback clears every removed service's memory as if it were one of its own (larger)
 * service records: ours sits at the start of a buffer big enough for that. */
typedef struct {
    otSrpClientService svc;
    uint8_t room_for_matter[1536];
} srp_slot_t;

static srp_slot_t *s_srp;
static char s_instance[40];
static char s_id_txt[13];
static otDnsTxtEntry s_txt;

static bool srp_listed(otInstance *ot)
{
    for (const otSrpClientService *s = otSrpClientGetServices(ot); s; s = s->mNext) {
        if (s == &s_srp->svc) {
            return true;
        }
    }
    return false;
}

static void srp_check(void)
{
    if (!s_srp) {
        s_srp = heap_caps_calloc(1, sizeof(*s_srp), MEM_CAPS_BULK);
        if (!s_srp) {
            return;
        }
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(s_instance, sizeof(s_instance), "Pill Reminder %02X%02X%02X", mac[3], mac[4], mac[5]);
        snprintf(s_id_txt, sizeof(s_id_txt), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ot = esp_openthread_get_instance();
    if (ot && !srp_listed(ot)) {
        memset(s_srp, 0, sizeof(*s_srp));
        s_txt.mKey = "id";
        s_txt.mValue = (const uint8_t *)s_id_txt;
        s_txt.mValueLength = strlen(s_id_txt);
        s_srp->svc.mName = "_meddot._udp";
        s_srp->svc.mInstanceName = s_instance;
        s_srp->svc.mTxtEntries = &s_txt;
        s_srp->svc.mNumTxtEntries = 1;
        s_srp->svc.mPort = APP_LINK_PORT;
        otError err = otSrpClientAddService(ot, &s_srp->svc);
        ESP_LOGI(TAG, "SRP: added %s._meddot._udp (%d)", s_instance, err);
    }
    esp_openthread_lock_release();
}

/* ---- Task ---- */

static bool open_socket(void)
{
    esp_netif_t *ot = esp_openthread_get_netif();
    if (!ot) {
        return false;
    }
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return false;
    }
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(APP_LINK_PORT), .sin6_addr = in6addr_any};
    struct ifreq ifr = {0};
    esp_netif_get_netif_impl_name(ot, ifr.ifr_name);
    struct timeval tv = {.tv_sec = 1};
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGW(TAG, "socket setup failed (errno %d), retrying", errno);
        close(s);
        return false;
    }
    s_sock = s;
    ESP_LOGI(TAG, "listening on port %d", APP_LINK_PORT);
    return true;
}

static void pair_expire(void)
{
    bool close_dialog = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pair.state != PAIR_NONE &&
        xTaskGetTickCount() - s_pair.started > pdMS_TO_TICKS(PAIR_TIMEOUT_S * 1000)) {
        close_dialog = s_pair.state == PAIR_WAITING;
        if (close_dialog) {
            ESP_LOGI(TAG, "pairing timed out");
        }
        memset(&s_pair, 0, sizeof(s_pair));
    }
    xSemaphoreGive(s_lock);
    if (close_dialog && bsp_display_lock(UINT32_MAX)) {
        dialog_close();
        bsp_display_unlock();
    }
}

static void task(void *arg)
{
    (void)arg;
    while (!open_socket()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    TickType_t srp_checked = 0;
    while (true) {
        struct sockaddr_in6 from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, s_buf, sizeof(s_buf), 0, (struct sockaddr *)&from, &from_len);
        if (len >= 4 && s_buf[0] == 'M' && s_buf[1] == 'A' && s_buf[2] == APP_LINK_VERSION) {
            if (s_buf[3] == APP_MSG_PAIR) {
                on_pair(s_buf + 4, len - 4, &from);
            } else if (s_buf[3] == APP_MSG_REQ) {
                on_request(len, &from);
            }
        }
        pair_expire();
        if (srp_checked == 0 || xTaskGetTickCount() - srp_checked > pdMS_TO_TICKS(SRP_CHECK_S * 1000)) {
            srp_check();
            srp_checked = xTaskGetTickCount();
        }
    }
}

void app_link_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    clients_load();
    /* mbedTLS ECP needs a few KB of stack */
    if (xTaskCreatePinnedToCoreWithCaps(task, "app_link", 8192, NULL, 3, NULL, tskNO_AFFINITY, MEM_CAPS_BULK) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

#else

void app_link_start(void) {}
void app_link_pair_answer(bool approve) { (void)approve; }

#endif
