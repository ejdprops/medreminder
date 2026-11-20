/* The device as Apple Home / Matter sees it: a bridge (aggregator endpoint) with bridged devices
 *   "Dose overdue": Contact Sensor, closed normally, open while any dose is overdue, so Home can
 *     notify (turn on notifications for the sensor).
 *   One On/Off Plug-in Unit per dose, labelled "<name> <time>" ("Karen 0930", + " TUE" for a
 *     weekly dose): on = taken today. Turning it on
 *     in Home takes the dose, off undoes it. Doses not due today (weekly) stay off; writes to them
 *     are refused (set back to off).
 * Nodes (CONFIG_MEDREMINDER_ROLE_NODE) keep the empty aggregator and add no endpoints: the main
 * unit is the one with dose accessories in Home.
 * Each dose's endpoint belongs to the dose's uid, saved here as uid -> endpoint id, so Home rooms
 * and automations survive edits, re-sorting and reboots (endpoints are resumed with their ids).
 * Adding or deleting a dose adds or removes its endpoint while running; renaming updates its
 * NodeLabel.
 *
 * Threads: the UI task publishes snapshots (matter_dev_publish), this module's task turns them into
 * endpoint and attribute changes. esp-matter takes the CHIP stack lock itself in enable(),
 * destroy() and attribute::update(), so this task never holds it; the CHIP task only queues
 * requests. s_mutex never wraps a Matter call (the CHIP task takes it inside the stack lock). */
#include "matter_dev.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <platform/CHIPDeviceConfig.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <nvs.h>

#include <app/clusters/boolean-state-server/boolean-state-cluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

#include "audio.h"
#include "mem_caps.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "matter_dev";

#define NVS_NAMESPACE "matter_dev"
#define NVS_KEY_MAP "eps" /* blob: ep_map_t[], one per bridged endpoint */
#define OVERDUE_UID 0     /* dose uids are 1-255 */
#define MAX_BRIDGED (DOSE_MAX + 1)
#define OVERDUE_LABEL "Dose overdue"
#define OVERDUE_SERIAL "OVERDUE"

typedef struct {
    uint8_t uid;
    uint16_t endpoint_id;
} ep_map_t;

typedef struct {
    bool used;
    uint8_t uid;
    endpoint_t *ep;
    bool on;                                  /* last state sent */
    char label[MATTER_DOSE_LABEL_MAX + 1];    /* last NodeLabel sent */
    char serial[MATTER_DOSE_SERIAL_MAX + 1];  /* last SerialNumber sent */
} bridged_t;

static node_t *s_node;
static endpoint_t *s_aggregator;
static TaskHandle_t s_task;
static QueueHandle_t s_requests; /* request_t from Home, for the UI task */
static SemaphoreHandle_t s_mutex;

typedef struct {
    uint8_t uid;
    bool on;
} request_t;

/* Buffers in PSRAM if there is any (internal RAM is short): allocated in matter_dev_start */
typedef struct {
    matter_dose_t pub[DOSE_MAX];  /* published by the UI task (under s_mutex) */
    matter_dose_t sync[DOSE_MAX]; /* this module's task: copy being synced */
    bridged_t bridged[MAX_BRIDGED]; /* this module's task (.uid/.used read by find_bridged) */
} bufs_t;
static bufs_t *s_bufs;
#define s_pub (s_bufs->pub)
#define s_bridged (s_bufs->bridged)

static int s_pub_count = -1; /* -1: nothing published yet; don't drop endpoints before the first */
static bool s_pub_overdue;
/* Send every state again on the next sync: after a write from Home (a refused one must be set
 * back) and at boot (resumed endpoints may come back with persisted, stale values) */
static bool s_resync = true;

static bool s_overdue_sent; /* this module's task */

static bridged_t *find_bridged(uint8_t uid)
{
    for (int i = 0; i < MAX_BRIDGED; i++) {
        if (s_bridged[i].used && s_bridged[i].uid == uid) {
            return &s_bridged[i];
        }
    }
    return NULL;
}

static void save_map(void)
{
    ep_map_t map[MAX_BRIDGED];
    int n = 0;
    for (int i = 0; i < MAX_BRIDGED; i++) {
        if (s_bridged[i].used) {
            map[n].uid = s_bridged[i].uid;
            map[n].endpoint_id = endpoint::get_id(s_bridged[i].ep);
            n++;
        }
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    if (n > 0) {
        nvs_set_blob(nvs, NVS_KEY_MAP, map, n * sizeof(ep_map_t));
    } else {
        nvs_erase_key(nvs, NVS_KEY_MAP);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Create (resume_id invalid) or resume a bridged endpoint for uid. Not yet enabled. No stack lock,
 * as in esp_matter_bridge: the endpoint is invisible until enable(), and several of these calls
 * take the lock themselves (it is not recursive). */
static endpoint_t *make_endpoint(uint8_t uid, const char *label, const char *serial, bool on, uint16_t resume_id)
{
    bridged_node::config_t bridged_config;
    void *priv = (void *)(intptr_t)uid;
    endpoint_t *ep = resume_id != chip::kInvalidEndpointId
                         ? bridged_node::resume(s_node, &bridged_config, ENDPOINT_FLAG_BRIDGE, resume_id, priv)
                         : bridged_node::create(s_node, &bridged_config, ENDPOINT_FLAG_BRIDGE, priv);
    if (!ep) {
        return NULL;
    }
    if (uid == OVERDUE_UID) {
        contact_sensor::config_t config;
        config.boolean_state.state_value = true; /* closed: nothing overdue */
        contact_sensor::add(ep, &config);
    } else {
        on_off_plug_in_unit::config_t config;
        config.on_off.on_off = on;
        on_off_plug_in_unit::add(ep, &config);
    }
    set_parent_endpoint(ep, s_aggregator);

    /* NodeLabel: the name Home offers; Manufacturer/Model: the device's own (Basic Information);
     * SerialNumber per dose; UniqueID: stable across reboots (cluster creates it empty) */
    namespace info_attr = cluster::bridged_device_basic_information::attribute;
    cluster_t *info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    info_attr::create_node_label(info, (char *)label, strlen(label));
    info_attr::create_vendor_name(info, (char *)CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME,
                                  strlen(CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME));
    info_attr::create_product_name(info, (char *)CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME,
                                   strlen(CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME));
    info_attr::create_serial_number(info, (char *)serial, strlen(serial));
    char unique_id[16];
    snprintf(unique_id, sizeof(unique_id), "dose-%u", uid);
    esp_matter_attr_val_t val = esp_matter_char_str(unique_id, strlen(unique_id));
    attribute::set_val(attribute::get(info, BridgedDeviceBasicInformation::Attributes::UniqueID::Id), &val, false);

    /* Cluster plugin init for clusters the node didn't have at start (as esp_matter_bridge does) */
    for (cluster_t *c = cluster::get_first(ep); c; c = cluster::get_next(c)) {
        cluster::plugin_server_init_callback_t init = cluster::get_plugin_server_init_callback(c);
        if (init) {
            init();
        }
    }
    return ep;
}

static bridged_t *add_bridged(uint8_t uid, const char *label, const char *serial, bool on, uint16_t resume_id)
{
    bridged_t *b = NULL;
    for (int i = 0; i < MAX_BRIDGED && !b; i++) {
        if (!s_bridged[i].used) {
            b = &s_bridged[i];
        }
    }
    if (!b) {
        ESP_LOGE(TAG, "no room for uid %u", uid);
        return NULL;
    }
    endpoint_t *ep = make_endpoint(uid, label, serial, on, resume_id);
    if (!ep && resume_id != chip::kInvalidEndpointId) {
        /* e.g. after a Matter factory reset forgot the endpoint ids: start a new one */
        ESP_LOGW(TAG, "uid %u: endpoint %u can't be resumed, creating a new one", uid, resume_id);
        ep = make_endpoint(uid, label, serial, on, chip::kInvalidEndpointId);
    }
    if (!ep) {
        ESP_LOGE(TAG, "uid %u: endpoint failed", uid);
        return NULL;
    }
    endpoint::enable(ep);
    b->used = true;
    b->uid = uid;
    b->ep = ep;
    b->on = on;
    strlcpy(b->label, label, sizeof(b->label));
    strlcpy(b->serial, serial, sizeof(b->serial));
    ESP_LOGI(TAG, "%s endpoint %u: uid %u \"%s\"", endpoint::get_id(ep) == resume_id ? "resumed" : "new",
             endpoint::get_id(ep), uid, label);
    return b;
}

static void remove_bridged(bridged_t *b)
{
    ESP_LOGI(TAG, "removed endpoint %u: \"%s\"", endpoint::get_id(b->ep), b->label);
    endpoint::destroy(s_node, b->ep);
    b->used = false;
}

/* Bring the endpoints saved last time back with their ids, so Home sees the same devices */
static void restore(void)
{
    ep_map_t map[MAX_BRIDGED];
    size_t len = sizeof(map);
    int n = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, NVS_KEY_MAP, map, &len) == ESP_OK) {
            n = len / sizeof(ep_map_t);
        }
        nvs_close(nvs);
    }
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (find_bridged(map[i].uid)) {
            changed = true; /* duplicate: drop it */
            continue;
        }
        /* Labels and states are filled in by the first sync */
        bool sensor = map[i].uid == OVERDUE_UID;
        bridged_t *b = add_bridged(map[i].uid, sensor ? OVERDUE_LABEL : "", sensor ? OVERDUE_SERIAL : "", false,
                                   map[i].endpoint_id);
        changed |= !b || endpoint::get_id(b->ep) != map[i].endpoint_id;
    }
    if (!find_bridged(OVERDUE_UID)) {
        changed |= add_bridged(OVERDUE_UID, OVERDUE_LABEL, OVERDUE_SERIAL, false, chip::kInvalidEndpointId) != NULL;
    }
    if (changed) {
        save_map();
    }
}

/* Contact sensor StateValue: true = contact (closed); open = overdue. The Boolean State cluster
 * is code-driven (attribute::update can't set it); its SetStateValue also sends the StateChange
 * event Home notifies on. */
static void set_contact(uint16_t endpoint_id, bool closed)
{
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *cluster = static_cast<BooleanStateCluster *>(
        data_model::provider::get_instance().registry().Get({endpoint_id, BooleanState::Id}));
    if (!cluster) {
        ESP_LOGE(TAG, "no Boolean State cluster on endpoint %u", endpoint_id);
        return;
    }
    cluster->SetStateValue(closed);
}

static void update_on_off(bridged_t *b, bool on)
{
    esp_matter_attr_val_t val = esp_matter_bool(on);
    b->on = on;
    attribute::update(endpoint::get_id(b->ep), OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

static void sync_endpoints(void)
{
    matter_dose_t *doses = s_bufs->sync;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_pub_count;
    bool overdue = s_pub_overdue;
    bool resync = s_resync;
    s_resync = false;
    if (count > 0) {
        memcpy(doses, s_pub, count * sizeof(matter_dose_t));
    }
    xSemaphoreGive(s_mutex);
    if (count < 0) {
        return;
    }

    bool map_changed = false;
    bridged_t *sensor = find_bridged(OVERDUE_UID);
    if (sensor && (overdue != s_overdue_sent || resync)) {
        set_contact(endpoint::get_id(sensor->ep), !overdue);
        s_overdue_sent = overdue;
        ESP_LOGI(TAG, "overdue sensor: %s", overdue ? "open (overdue)" : "closed");
    }

    /* Deleted doses */
    for (int i = 0; i < MAX_BRIDGED; i++) {
        bridged_t *b = &s_bridged[i];
        if (!b->used || b->uid == OVERDUE_UID) {
            continue;
        }
        bool found = false;
        for (int j = 0; j < count && !found; j++) {
            found = doses[j].uid == b->uid;
        }
        if (!found) {
            remove_bridged(b);
            map_changed = true;
        }
    }

    /* New, renamed and changed doses */
    for (int j = 0; j < count; j++) {
        bridged_t *b = find_bridged(doses[j].uid);
        if (!b) {
            map_changed |= add_bridged(doses[j].uid, doses[j].label, doses[j].serial, doses[j].on,
                                       chip::kInvalidEndpointId) != NULL;
            continue;
        }
        if (strcmp(b->label, doses[j].label) != 0) {
            esp_matter_attr_val_t val = esp_matter_char_str(doses[j].label, strlen(doses[j].label));
            attribute::update(endpoint::get_id(b->ep), BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &val);
            ESP_LOGI(TAG, "endpoint %u: \"%s\" -> \"%s\"", endpoint::get_id(b->ep), b->label, doses[j].label);
            strlcpy(b->label, doses[j].label, sizeof(b->label));
        }
        if (strcmp(b->serial, doses[j].serial) != 0) {
            esp_matter_attr_val_t val = esp_matter_char_str(doses[j].serial, strlen(doses[j].serial));
            attribute::update(endpoint::get_id(b->ep), BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::SerialNumber::Id, &val);
            strlcpy(b->serial, doses[j].serial, sizeof(b->serial));
        }
        if (b->on != doses[j].on || resync) {
            update_on_off(b, doses[j].on);
        }
    }
    if (map_changed) {
        save_map();
    }
}

static void task(void *arg)
{
    /* Before restore: this task runs before xTaskCreate returns the handle, and the attribute
     * callback tells our own writes from Home's by it */
    s_task = xTaskGetCurrentTaskHandle();
    restore();
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sync_endpoints();
    }
}

void matter_dev_create(node_t *node)
{
    s_node = node;
    aggregator::config_t config;
    s_aggregator = aggregator::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    if (!s_aggregator) {
        ESP_LOGE(TAG, "aggregator endpoint failed");
        return;
    }
    ESP_LOGI(TAG, "bridge: aggregator endpoint %u", endpoint::get_id(s_aggregator));
}

void matter_dev_start(void)
{
#if CONFIG_MEDREMINDER_ROLE_NODE
    ESP_LOGI(TAG, "node: no dose accessories");
    return;
#endif
    if (!s_aggregator) {
        return;
    }
    s_bufs = (bufs_t *)heap_caps_calloc(1, sizeof(bufs_t), MEM_CAPS_BULK);
    if (!s_bufs) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_requests = xQueueCreate(8, sizeof(request_t));
    /* Stack in PSRAM like the UI's (if there is any): XIP from PSRAM keeps it usable during flash
     * (NVS) writes */
    if (xTaskCreatePinnedToCoreWithCaps(task, "matter_dev", 6144, NULL, 4, &s_task, tskNO_AFFINITY,
                                        MEM_CAPS_BULK) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

void matter_dev_publish(const matter_dose_t *doses, int count, bool overdue)
{
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_resync || count != s_pub_count || overdue != s_pub_overdue ||
                   memcmp(doses, s_pub, count * sizeof(matter_dose_t)) != 0;
    if (changed) {
        memcpy(s_pub, doses, count * sizeof(matter_dose_t));
        s_pub_count = count;
        s_pub_overdue = overdue;
    }
    xSemaphoreGive(s_mutex);
    if (changed) {
        xTaskNotifyGive(s_task);
    }
}

bool matter_dev_next_request(uint8_t *uid, bool *on)
{
    request_t r;
    if (!s_requests || xQueueReceive(s_requests, &r, 0) != pdTRUE) {
        return false;
    }
    *uid = r.uid;
    *on = r.on;
    return true;
}

esp_err_t matter_dev_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                      esp_matter_attr_val_t *val, void *priv_data)
{
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id || !s_requests) {
        return ESP_OK;
    }
    /* Our own updates (this module's task) come through here too; only others are requests */
    request_t r = {(uint8_t)(intptr_t)priv_data, val->val.b};
    if (r.uid == OVERDUE_UID || xTaskGetCurrentTaskHandle() == s_task) {
        return ESP_OK;
    }
    /* The UI task answers with a publish; s_resync makes it resend every state, so a refused
     * write is set back */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_resync = true;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Home: dose uid %u %s", r.uid, r.on ? "taken" : "undo");
    xQueueSend(s_requests, &r, 0);
    return ESP_OK;
}

void matter_dev_identify(uint16_t endpoint_id)
{
    ESP_LOGI(TAG, "identify endpoint %u", endpoint_id);
    audio_play(AUDIO_ALERT);
}

/* Level Control (needed by the On/Off server) calls into Color Control, which is disabled in
 * sdkconfig.defaults to save RAM: no color temperature here to couple */
void emberAfPluginLevelControlCoupledColorTempChangeCallback(chip::EndpointId endpoint) {}
