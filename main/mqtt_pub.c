#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "apc_ups.h"
#include "nvs_config.h"
#include "mqtt_pub.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client    = NULL;
static SemaphoreHandle_t        s_mutex     = NULL;
static bool                     s_connected = false;

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
    apc_ups_get_data(&d);

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

/* ── MQTT event handler ──────────────────────────────────────────────── */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    (void)ev;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = true;
        xSemaphoreGive(s_mutex);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = false;
        xSemaphoreGive(s_mutex);
        break;
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

    esp_mqtt_client_config_t mc = {
        .broker.address.uri          = cfg->mqtt_uri,
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
