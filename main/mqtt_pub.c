#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "ups_driver.h"
#include "nvs_config.h"
#include "mqtt_pub.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client    = NULL;
static SemaphoreHandle_t        s_mutex     = NULL;
static bool                     s_connected = false;

/* ── Power sensor discovery ─────────────────────────────────────────── */
static power_sensor_t s_power_sensors[MAX_POWER_SENSORS];
static int            s_power_sensor_count = 0;
static char           s_subscribed_topic[128] = "";
static char           s_subscribed_json_key[32] = "";

/* ── HA discovery definitions ────────────────────────────────────────── */
typedef struct {
    const char *key;
    const char *name;
    const char *unit;
    const char *device_class;
    const char *state_class;
} ha_sensor_t;

static const ha_sensor_t s_sensors[] = {
    { "battery/charge",         "Battery Charge",       "%",   "battery",     "measurement" },
    { "battery/runtime",        "Battery Runtime",      "s",   "duration",    "measurement" },
    { "battery/voltage",        "Battery Voltage",      "V",   "voltage",     "measurement" },
    { "input/voltage",          "Input Voltage",        "V",   "voltage",     "measurement" },
    { "ups/load",               "UPS Load",             "%",   NULL,          "measurement" },
    { "ups/temperature",        "UPS Temperature",      "°C",  "temperature", "measurement" },
    { "ups/status",             "UPS Status",           NULL,  NULL,          NULL          },
    { "ups/realpower/nominal",  "Nominal Power",        "W",   "power",       NULL          },
};

static void publish_ha_discovery(const char *prefix, const char *ups_name,
                                  uint8_t qos)
{
    char dev_id[40];
    snprintf(dev_id, sizeof(dev_id), "esp32_nut_%s", ups_name);

    for (int i = 0; i < (int)(sizeof(s_sensors)/sizeof(s_sensors[0])); i++) {
        const ha_sensor_t *s = &s_sensors[i];

        char slug[64];
        strlcpy(slug, s->key, sizeof(slug));
        for (char *p = slug; *p; p++) if (*p == '/') *p = '_';

        char disc_topic[192];
        snprintf(disc_topic, sizeof(disc_topic),
                 "homeassistant/sensor/%s_%s/config", dev_id, slug);

        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/%s", prefix, s->key);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name",        s->name);
        cJSON_AddStringToObject(root, "unique_id",   slug);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        if (s->unit)         cJSON_AddStringToObject(root, "unit_of_measurement", s->unit);
        if (s->device_class) cJSON_AddStringToObject(root, "device_class",        s->device_class);
        if (s->state_class)  cJSON_AddStringToObject(root, "state_class",         s->state_class);

        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "identifiers",   dev_id);
        cJSON_AddStringToObject(dev, "name",          "APC UPS");
        cJSON_AddStringToObject(dev, "manufacturer",  "APC");
        cJSON_AddStringToObject(dev, "model",         ups_name);
        cJSON_AddStringToObject(dev, "sw_version",    "esp32-nut-1.0");
        cJSON_AddItemToObject(root, "device", dev);

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        esp_mqtt_client_publish(s_client, disc_topic, payload, 0, qos, 1);
        free(payload);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "HA discovery published (%d sensors)", (int)(sizeof(s_sensors)/sizeof(s_sensors[0])));
}

/* ── publish one cycle of UPS data ───────────────────────────────────── */
static void publish_data(const char *prefix, uint8_t qos)
{
    ups_data_t d;
    ups_driver_get_data(&d);

    char topic[128], val[32];

#define PUB(subtopic, fmt, field) do { \
    snprintf(topic, sizeof(topic), "%s/%s", prefix, subtopic); \
    snprintf(val,   sizeof(val),   fmt,     field); \
    esp_mqtt_client_publish(s_client, topic, val, 0, qos, 0); \
} while(0)

    PUB("battery/charge",        "%.1f", d.battery_charge);
    PUB("battery/runtime",       "%.0f", d.battery_runtime);
    PUB("battery/voltage",       "%.2f", d.battery_voltage);
    PUB("input/voltage",         "%.1f", d.input_voltage);
    PUB("ups/load",              "%.1f", d.ups_load);
    PUB("ups/temperature",       "%.1f", d.ups_temperature);
    PUB("ups/realpower/nominal", "%.0f", d.ups_realpower_nominal);

    snprintf(topic, sizeof(topic), "%s/ups/status", prefix);
    esp_mqtt_client_publish(s_client, topic, d.status, 0, qos, 0);
#undef PUB
}

/* ── HA discovery: parse a sensor config payload ────────────────────── */
static void handle_discovery(const char *data, int data_len)
{
    char *buf = malloc(data_len + 1);
    if (!buf) return;
    memcpy(buf, data, data_len);
    buf[data_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    /* Only keep sensors with device_class "power" or unit "W"/"kW" */
    const cJSON *dc   = cJSON_GetObjectItem(root, "device_class");
    const cJSON *unit  = cJSON_GetObjectItem(root, "unit_of_measurement");
    const cJSON *topic = cJSON_GetObjectItem(root, "state_topic");

    bool is_power = false;
    if (dc && cJSON_IsString(dc) && strcmp(dc->valuestring, "power") == 0)
        is_power = true;
    if (!is_power && unit && cJSON_IsString(unit) &&
        (strcmp(unit->valuestring, "W") == 0 || strcmp(unit->valuestring, "kW") == 0))
        is_power = true;

    if (!is_power || !topic || !cJSON_IsString(topic)) {
        cJSON_Delete(root);
        return;
    }

    /* Extract name, unique_id, json_key from value_template */
    const char *name_str = "Unknown";
    const cJSON *name_j = cJSON_GetObjectItem(root, "name");
    if (name_j && cJSON_IsString(name_j)) name_str = name_j->valuestring;

    const char *uid_str = "";
    const cJSON *uid_j = cJSON_GetObjectItem(root, "unique_id");
    if (uid_j && cJSON_IsString(uid_j)) uid_str = uid_j->valuestring;

    /* Parse value_template like "{{ value_json.power }}" → "power" */
    char json_key[32] = "";
    const cJSON *vt = cJSON_GetObjectItem(root, "value_template");
    if (vt && cJSON_IsString(vt)) {
        const char *s = strstr(vt->valuestring, "value_json.");
        if (s) {
            s += 11; /* skip "value_json." */
            int i = 0;
            while (*s && *s != ' ' && *s != '}' && *s != '|' && i < 31)
                json_key[i++] = *s++;
            json_key[i] = '\0';
        }
    }

    /* Check for duplicate by topic+json_key */
    for (int i = 0; i < s_power_sensor_count; i++) {
        if (strcmp(s_power_sensors[i].topic, topic->valuestring) == 0 &&
            strcmp(s_power_sensors[i].json_key, json_key) == 0) {
            /* Update name in case it changed */
            strlcpy(s_power_sensors[i].name, name_str, sizeof(s_power_sensors[0].name));
            cJSON_Delete(root);
            return;
        }
    }

    if (s_power_sensor_count < MAX_POWER_SENSORS) {
        power_sensor_t *ps = &s_power_sensors[s_power_sensor_count++];
        strlcpy(ps->name,     name_str,              sizeof(ps->name));
        strlcpy(ps->topic,    topic->valuestring,    sizeof(ps->topic));
        strlcpy(ps->json_key, json_key,              sizeof(ps->json_key));
        strlcpy(ps->unique_id,uid_str,               sizeof(ps->unique_id));
        ESP_LOGI(TAG, "Discovered power sensor: \"%s\" topic=%s key=%s",
                 ps->name, ps->topic, json_key[0] ? json_key : "(raw)");
    }

    cJSON_Delete(root);
}

/* ── Handle incoming data on the subscribed power sensor topic ──────── */
static void handle_power_data(const char *data, int data_len)
{
    char val[32] = "";

    if (s_subscribed_json_key[0]) {
        /* Parse JSON and extract key */
        char *buf = malloc(data_len + 1);
        if (!buf) return;
        memcpy(buf, data, data_len);
        buf[data_len] = '\0';
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (root) {
            cJSON *v = cJSON_GetObjectItem(root, s_subscribed_json_key);
            if (v) {
                if (cJSON_IsNumber(v))
                    snprintf(val, sizeof(val), "%.0f", v->valuedouble);
                else if (cJSON_IsString(v))
                    strlcpy(val, v->valuestring, sizeof(val));
            }
            cJSON_Delete(root);
        }
    } else {
        /* Raw value */
        int len = data_len < 31 ? data_len : 31;
        memcpy(val, data, len);
        val[len] = '\0';
    }

    if (val[0]) {
        ups_driver_set_var("ups.realpower", val);
    }
}

/* ── Subscribe to the configured power sensor topic ─────────────────── */
void mqtt_subscribe_power_topic(const char *topic, const char *json_key)
{
    if (!s_client || !s_connected) return;

    /* Unsubscribe from old topic */
    if (s_subscribed_topic[0]) {
        esp_mqtt_client_unsubscribe(s_client, s_subscribed_topic);
        s_subscribed_topic[0] = '\0';
        s_subscribed_json_key[0] = '\0';
    }

    if (topic && topic[0]) {
        strlcpy(s_subscribed_topic, topic, sizeof(s_subscribed_topic));
        strlcpy(s_subscribed_json_key, json_key ? json_key : "",
                sizeof(s_subscribed_json_key));
        esp_mqtt_client_subscribe(s_client, topic, 0);
        ESP_LOGI(TAG, "Subscribed to power sensor: %s (key=%s)",
                 topic, json_key && json_key[0] ? json_key : "raw");
    }
}

/* ── MQTT event handler ──────────────────────────────────────────────── */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to broker");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = true;
        xSemaphoreGive(s_mutex);
        /* Subscribe to HA sensor discovery topics to find power meters */
        esp_mqtt_client_subscribe(s_client, "homeassistant/sensor/+/config", 0);
        esp_mqtt_client_subscribe(s_client, "homeassistant/sensor/+/+/config", 0);
        /* Re-subscribe to configured power sensor */
        const nvs_cfg_t *cfg = nvs_config_get();
        if (cfg->power_topic[0]) {
            mqtt_subscribe_power_topic(cfg->power_topic, cfg->power_json_key);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = false;
        xSemaphoreGive(s_mutex);
        break;
    case MQTT_EVENT_DATA: {
        if (!ev->topic || ev->topic_len == 0) break;
        /* Check if this is a HA discovery message */
        if (ev->topic_len > 25 &&
            memcmp(ev->topic, "homeassistant/sensor/", 20) == 0 &&
            memcmp(ev->topic + ev->topic_len - 7, "/config", 7) == 0) {
            handle_discovery(ev->data, ev->data_len);
        }
        /* Check if this is our subscribed power sensor */
        else if (s_subscribed_topic[0] &&
                 ev->topic_len == (int)strlen(s_subscribed_topic) &&
                 memcmp(ev->topic, s_subscribed_topic, ev->topic_len) == 0) {
            handle_power_data(ev->data, ev->data_len);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

/* ── public API ──────────────────────────────────────────────────────── */
esp_err_t mqtt_pub_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return ESP_OK;
}

void mqtt_pub_stop(void)
{
    if (s_client) {
        esp_mqtt_client_disconnect(s_client);
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client    = NULL;
        s_connected = false;
        ESP_LOGI(TAG, "MQTT client stopped");
    }
}

void mqtt_pub_reconfigure(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client    = NULL;
        s_connected = false;
    }

    const nvs_cfg_t *cfg = nvs_config_get();
    if (!cfg->mqtt_en || cfg->mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled");
        return;
    }

    /* Auto-prepend mqtt:// if no scheme is given */
    char uri_buf[160];
    const char *uri = cfg->mqtt_uri;
    if (strncmp(uri, "mqtt", 4) != 0 && strncmp(uri, "ws", 2) != 0) {
        snprintf(uri_buf, sizeof(uri_buf), "mqtt://%s", uri);
        uri = uri_buf;
    }

    esp_mqtt_client_config_t mc = {
        .broker.address.uri          = uri,
        .credentials.username        = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .session.keepalive           = 60,
    };

    s_client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client started → %s", cfg->mqtt_uri);
}

bool mqtt_pub_is_connected(void)
{
    return s_connected && s_client != NULL;
}

int mqtt_get_power_sensors(const power_sensor_t **out)
{
    *out = s_power_sensors;
    return s_power_sensor_count;
}

void mqtt_pub_task(void *arg)
{
    esp_task_wdt_add(NULL);
    bool ha_sent = false;

    while (1) {
        const nvs_cfg_t *cfg = nvs_config_get();

        uint32_t interval_ms = cfg->mqtt_interval * 1000;
        if (interval_ms < 5000) interval_ms = 5000;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool connected = s_connected;
        xSemaphoreGive(s_mutex);

        if (connected && cfg->mqtt_en && s_client) {
            if (!ha_sent && cfg->mqtt_ha) {
                publish_ha_discovery(cfg->mqtt_prefix, cfg->ups_name, cfg->mqtt_qos);
                ha_sent = true;
            }
            publish_data(cfg->mqtt_prefix, cfg->mqtt_qos);
        } else {
            ha_sent = false;
        }

        /* Break the publish interval into WDT-friendly chunks */
        uint32_t remaining_ms = interval_ms;
        while (remaining_ms > 0) {
            uint32_t chunk = remaining_ms > 5000 ? 5000 : remaining_ms;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            esp_task_wdt_reset();
            remaining_ms -= chunk;
        }
    }
}
