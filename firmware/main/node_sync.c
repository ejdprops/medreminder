/* Node sync: nodes (CONFIG_MEDREMINDER_ROLE_NODE) mirror the main unit's schedule, today's taken
 * flags and the overdue setting, over UDP on the Thread mesh (no Apple/Matter involvement; Thread
 * encrypts it and only devices on the home's Thread network can send).
 *
 *   BEACON  main -> group ff03::4d52 (realm-local: the whole mesh), every BEACON_S and right after a
 *           change: the CRC of the current state.
 *   HELLO   node -> main (unicast to the beacon's sender), on every beacon and right after taking a
 *           new state; to the group at boot and while no beacon has been heard: the CRC the node
 *           holds. The main unit records the node; if the CRC is old it answers with STATE. The same
 *           state goes to a node at most every STATE_MIN_S (so a node that can't keep it can't make
 *           it spin); a newer one goes at once.
 *   STATE   main -> node: the doses_snapshot_t, CRC in the header.
 *   TAKE    node -> main: take the dose with this uid (in the header's CRC field, bits 0-7; a shake
 *           of a round node, the dashboard), undo it with TAKE_UNDO set, or snooze the overdue chime
 *           with TAKE_SNOOZE (uid 0: an older main unit finds no such dose and ignores it). Repeated every TAKE_RETRY_S, at most
 *           TAKE_TRIES times, until a STATE arrives (the take changes the main unit's state). Taking and
 *           undoing are idempotent, so a repeat does no harm.
 *
 * "Synced" on the main unit: heard within SYNCED_S and holding the current CRC. A node is forgotten
 * after FORGET_S of silence. The UI task hands states over with node_sync_publish /
 * node_sync_take_state; the socket lives in this module's task. */
#include "node_sync.h"

#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_openthread_netif_glue.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include "mem_caps.h"

static const char *TAG = "node_sync";

#define PORT 49231
#define GROUP "ff03::4d52"
#define MAGIC0 'M'
#define MAGIC1 'R'
#define PROTO 1
#define BEACON_S 30
#define SYNCED_S 120     /* a node counts as synced if heard this recently with the current CRC */
#define FORGET_S 600     /* nodes silent this long are dropped */
#define CONNECTED_S 90   /* node: the main unit counts as connected for this long after a beacon */
#define STATE_MIN_S 5
#define MAX_NODES 8
#define TAKE_RETRY_S 2
#define TAKE_TRIES 5
#define TAKE_QUEUE 4
#define TAKE_UNDO 0x100   /* in a TAKE's CRC field: undo instead of take */
#define TAKE_SNOOZE 0x200 /* ...snooze the overdue chime */

enum { MSG_BEACON = 1, MSG_HELLO = 2, MSG_STATE = 3, MSG_TAKE = 4 };

typedef struct __attribute__((packed)) {
    uint8_t magic[2];
    uint8_t proto;
    uint8_t type;
    uint8_t id[6]; /* sender's base MAC */
    uint8_t reserved[2];
    uint32_t crc;
} hdr_t;

typedef struct {
    uint8_t id[6];
    uint32_t crc;
    TickType_t heard, state_sent;
    uint32_t sent_crc; /* CRC of the last STATE sent to it */
    bool used;
} node_t;

static SemaphoreHandle_t s_lock;
static int s_sock = -1;
static uint8_t s_id[6];
/* What this device holds (UI task -> here) */
static doses_snapshot_t s_mine;
static uint32_t s_mine_crc;
static bool s_mine_changed;
/* Node: state received from the main unit (here -> UI task) */
static doses_snapshot_t s_rx;
static bool s_rx_new;
#if CONFIG_MEDREMINDER_ROLE_NODE
static struct sockaddr_in6 s_main;
#endif
static bool s_main_known;
static TickType_t s_main_heard;
#if CONFIG_MEDREMINDER_ROLE_NODE
static uint16_t s_take;         /* request being sent: uid | TAKE_UNDO / TAKE_SNOOZE; 0 = none */
static int s_take_tries;
static TickType_t s_take_sent;
#else
/* Main: nodes, and take requests from them (here -> UI task) */
static node_t s_nodes[MAX_NODES];
static uint16_t s_takes[TAKE_QUEUE]; /* uid | TAKE_UNDO / TAKE_SNOOZE */
static int s_take_count;
#endif
/* Socket buffer: header + a full snapshot */
static uint8_t s_buf[sizeof(hdr_t) + sizeof(doses_snapshot_t)];

static uint32_t snap_crc(const doses_snapshot_t *snap)
{
    return esp_rom_crc32_le(0, (const uint8_t *)snap, DOSES_SNAPSHOT_LEN(snap->count));
}

static bool older_than(TickType_t t, int seconds)
{
    return xTaskGetTickCount() - t > pdMS_TO_TICKS(seconds * 1000);
}

void node_sync_publish(const doses_snapshot_t *snap)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (memcmp(&s_mine, snap, sizeof(*snap)) != 0) {
        s_mine = *snap;
        s_mine_crc = snap_crc(snap);
        s_mine_changed = true;
    }
    xSemaphoreGive(s_lock);
}

bool node_sync_take_state(doses_snapshot_t *snap)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool got = s_rx_new;
    if (got) {
        *snap = s_rx;
        s_rx_new = false;
    }
    xSemaphoreGive(s_lock);
    return got;
}

void node_sync_status(int *synced, int *known)
{
    *synced = *known = 0;
#if !CONFIG_MEDREMINDER_ROLE_NODE
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used) {
            (*known)++;
            *synced += s_nodes[i].crc == s_mine_crc && !older_than(s_nodes[i].heard, SYNCED_S);
        }
    }
    xSemaphoreGive(s_lock);
#endif
}

void node_sync_request(uint8_t uid, node_req_t req)
{
#if CONFIG_MEDREMINDER_ROLE_NODE
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_take = req == NODE_REQ_SNOOZE ? TAKE_SNOOZE : uid | (req == NODE_REQ_UNDO ? TAKE_UNDO : 0);
    s_take_tries = 0;
    xSemaphoreGive(s_lock);
#else
    (void)uid;
    (void)req;
#endif
}

bool node_sync_next_request(uint8_t *uid, node_req_t *req)
{
#if CONFIG_MEDREMINDER_ROLE_NODE
    (void)uid;
    (void)req;
    return false;
#else
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool got = s_take_count > 0;
    if (got) {
        *uid = (uint8_t)s_takes[0];
        *req = s_takes[0] & TAKE_SNOOZE ? NODE_REQ_SNOOZE : s_takes[0] & TAKE_UNDO ? NODE_REQ_UNDO : NODE_REQ_TAKE;
        memmove(s_takes, s_takes + 1, --s_take_count * sizeof(s_takes[0]));
    }
    xSemaphoreGive(s_lock);
    return got;
#endif
}

bool node_sync_connected(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_main_known && !older_than(s_main_heard, CONNECTED_S);
    xSemaphoreGive(s_lock);
    return ok;
}

/* ---- Socket ---- */

/* GROUP as an in6_addr. Not inet6_aton() into one: that writes an lwIP ip6_addr_t, 4 bytes longer
 * (zone), and zeroed the field after it (ipv6mr_interface). */
static void group_in6(struct in6_addr *out)
{
    ip6_addr_t a;
    ip6addr_aton(GROUP, &a);
    inet6_addr_from_ip6addr(out, &a);
}

static struct sockaddr_in6 group_addr(void)
{
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(PORT)};
    group_in6(&a.sin6_addr);
    return a;
}

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
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(PORT), .sin6_addr = in6addr_any};
    struct ifreq ifr = {0};
    esp_netif_get_netif_impl_name(ot, ifr.ifr_name);
    struct ipv6_mreq mreq = {.ipv6mr_interface = esp_netif_get_netif_impl_index(ot)};
    group_in6(&mreq.ipv6mr_multiaddr);
    struct timeval tv = {.tv_sec = 1};
    int step = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        (++step, setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) ||
        (++step, setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) ||
        (++step, setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)) {
        ESP_LOGW(TAG, "socket setup step %d failed (errno %d), retrying", step, errno);
        close(s);
        return false;
    }
    s_sock = s;
    ESP_LOGI(TAG, "listening on [%s]:%d via %s", GROUP, PORT, ifr.ifr_name);
    return true;
}

static void send_to(const struct sockaddr_in6 *to, uint8_t type, uint32_t crc, const doses_snapshot_t *snap)
{
    hdr_t *h = (hdr_t *)s_buf;
    memset(h, 0, sizeof(*h));
    h->magic[0] = MAGIC0;
    h->magic[1] = MAGIC1;
    h->proto = PROTO;
    h->type = type;
    memcpy(h->id, s_id, sizeof(h->id));
    h->crc = crc;
    size_t len = sizeof(*h);
    if (snap) {
        memcpy(s_buf + len, snap, DOSES_SNAPSHOT_LEN(snap->count));
        len += DOSES_SNAPSHOT_LEN(snap->count);
    }
    if (sendto(s_sock, s_buf, len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        ESP_LOGD(TAG, "send type %d failed: errno %d", type, errno);
    }
}


#if !CONFIG_MEDREMINDER_ROLE_NODE
/* ---- Main unit ---- */

static node_t *find_node(const uint8_t *id)
{
    node_t *free_slot = NULL, *oldest = &s_nodes[0];
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &s_nodes[i];
        if (n->used && memcmp(n->id, id, 6) == 0) {
            return n;
        }
        if (!n->used && !free_slot) {
            free_slot = n;
        }
        if (n->heard < oldest->heard) {
            oldest = n;
        }
    }
    node_t *n = free_slot ? free_slot : oldest;
    memset(n, 0, sizeof(*n));
    memcpy(n->id, id, 6);
    n->used = true;
    ESP_LOGI(TAG, "new node %02x%02x%02x", id[3], id[4], id[5]);
    return n;
}

static void main_rx(const hdr_t *h, const struct sockaddr_in6 *from)
{
    static doses_snapshot_t snap; /* sent outside the lock */
    if (h->type == MSG_TAKE) {
        uint16_t req = h->crc & (0xff | TAKE_UNDO | TAKE_SNOOZE);
        if (req & TAKE_SNOOZE) {
            ESP_LOGI(TAG, "node %02x%02x%02x snoozes", h->id[3], h->id[4], h->id[5]);
        } else {
            ESP_LOGI(TAG, "node %02x%02x%02x %s dose %u", h->id[3], h->id[4], h->id[5],
                     req & TAKE_UNDO ? "undoes" : "takes", req & 0xff);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_take_count < TAKE_QUEUE) {
            s_takes[s_take_count++] = req;
        }
        xSemaphoreGive(s_lock);
        return;
    }
    if (h->type != MSG_HELLO) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    node_t *n = find_node(h->id);
    n->heard = xTaskGetTickCount();
    n->crc = h->crc;
    bool send = h->crc != s_mine_crc &&
                (n->sent_crc != s_mine_crc || n->state_sent == 0 || older_than(n->state_sent, STATE_MIN_S));
    if (send) {
        n->state_sent = xTaskGetTickCount();
        n->sent_crc = s_mine_crc;
        snap = s_mine;
    }
    uint32_t crc = s_mine_crc;
    xSemaphoreGive(s_lock);
    if (send) {
        ESP_LOGI(TAG, "node %02x%02x%02x has %08lx: sending state %08lx (%d doses)", h->id[3], h->id[4], h->id[5],
                 (unsigned long)h->crc, (unsigned long)crc, snap.count);
        send_to(from, MSG_STATE, crc, &snap);
    }
}

static void main_tick(TickType_t *last_beacon)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_mine_changed;
    s_mine_changed = false;
    uint32_t crc = s_mine_crc;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used && older_than(s_nodes[i].heard, FORGET_S)) {
            ESP_LOGI(TAG, "forgetting node %02x%02x%02x", s_nodes[i].id[3], s_nodes[i].id[4], s_nodes[i].id[5]);
            s_nodes[i].used = false;
        }
    }
    xSemaphoreGive(s_lock);
    if (changed || *last_beacon == 0 || older_than(*last_beacon, BEACON_S)) {
        struct sockaddr_in6 g = group_addr();
        send_to(&g, MSG_BEACON, crc, NULL);
        *last_beacon = xTaskGetTickCount();
    }
}

#else
/* ---- Node ---- */

static void node_rx(const hdr_t *h, int len, const struct sockaddr_in6 *from)
{
    if (h->type == MSG_BEACON) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_main_known) {
            ESP_LOGI(TAG, "main unit found");
        }
        s_main = *from;
        s_main_known = true;
        s_main_heard = xTaskGetTickCount();
        uint32_t crc = s_mine_crc;
        xSemaphoreGive(s_lock);
        send_to(from, MSG_HELLO, crc, NULL);
        return;
    }
    if (h->type != MSG_STATE) {
        return;
    }
    const doses_snapshot_t *snap = (const doses_snapshot_t *)(s_buf + sizeof(hdr_t));
    int body = len - (int)sizeof(hdr_t);
    if (body < (int)offsetof(doses_snapshot_t, defs) || snap->count > DOSE_MAX ||
        body != (int)DOSES_SNAPSHOT_LEN(snap->count) || snap_crc(snap) != h->crc) {
        ESP_LOGW(TAG, "bad state (%d bytes)", len);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_rx, 0, sizeof(s_rx));
    memcpy(&s_rx, snap, body);
    s_rx_new = true;
    s_take = 0; /* a new state: the main unit handled the request (or it will come with the next) */
    s_main = *from;
    s_main_known = true;
    s_main_heard = xTaskGetTickCount();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "state %08lx from the main unit (%d doses)", (unsigned long)h->crc, snap->count);
}

static void node_tick(TickType_t *last_hello)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_mine_changed;
    s_mine_changed = false;
    uint32_t crc = s_mine_crc;
    bool known = s_main_known && !older_than(s_main_heard, CONNECTED_S);
    struct sockaddr_in6 to = s_main;
    uint32_t take = 0;
    if (s_take && known && (s_take_tries == 0 || older_than(s_take_sent, TAKE_RETRY_S))) {
        take = s_take;
        s_take_sent = xTaskGetTickCount();
        if (++s_take_tries >= TAKE_TRIES) {
            s_take = 0;
        }
    }
    xSemaphoreGive(s_lock);
    if (take & TAKE_SNOOZE) {
        ESP_LOGI(TAG, "asking the main unit to snooze");
        send_to(&to, MSG_TAKE, take, NULL);
    } else if (take) {
        ESP_LOGI(TAG, "asking the main unit to %s dose %lu", take & TAKE_UNDO ? "undo" : "take",
                 (unsigned long)(take & 0xff));
        send_to(&to, MSG_TAKE, take, NULL);
    }
    /* Report a new state right away (the ack after a STATE); look for the main unit while it's quiet */
    if (changed && known) {
        send_to(&to, MSG_HELLO, crc, NULL);
    } else if (!known && (*last_hello == 0 || older_than(*last_hello, BEACON_S))) {
        struct sockaddr_in6 g = group_addr();
        send_to(&g, MSG_HELLO, crc, NULL);
        *last_hello = xTaskGetTickCount();
    }
}
#endif

static void task(void *arg)
{
    (void)arg;
    while (!open_socket()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    TickType_t last = 0;
    while (true) {
        struct sockaddr_in6 from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, s_buf, sizeof(s_buf), 0, (struct sockaddr *)&from, &from_len);
        if (len >= (int)sizeof(hdr_t)) {
            const hdr_t *h = (const hdr_t *)s_buf;
            if (h->magic[0] == MAGIC0 && h->magic[1] == MAGIC1 && h->proto == PROTO &&
                memcmp(h->id, s_id, sizeof(s_id)) != 0) {
#if CONFIG_MEDREMINDER_ROLE_NODE
                node_rx(h, len, &from);
#else
                main_rx(h, &from);
#endif
            }
        }
#if CONFIG_MEDREMINDER_ROLE_NODE
        node_tick(&last);
#else
        main_tick(&last);
#endif
    }
}

void node_sync_start(void)
{
    esp_read_mac(s_id, ESP_MAC_BASE);
    s_lock = xSemaphoreCreateMutex();
    if (xTaskCreatePinnedToCoreWithCaps(task, "node_sync", 4096, NULL, 3, NULL, tskNO_AFFINITY, MEM_CAPS_BULK) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
