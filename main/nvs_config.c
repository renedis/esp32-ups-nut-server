#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_config.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_cfg";
#define NVS_NS "nut_cfg"

static nvs_cfg_t s_cfg;

static void overrides_load(void);

/* ── Variable overrides ─────────────────────────────────────────────── */
typedef struct { char name[40]; char value[64]; } override_entry_t;
static override_entry_t s_overrides[MAX_OVERRIDES];
static int              s_override_count = 0;
static char             s_overrides_json[4000] = "{}";

static const nvs_cfg_t s_defaults = {
    .ups_name      = "apc",
    .ups_desc      = "APC Back-UPS BE850G2",
    .nut_user      = CONFIG_APC_NUT_USERNAME,
    .nut_pass      = CONFIG_APC_NUT_PASSWORD,
    .nut_port      = CONFIG_APC_NUT_PORT,
    .poll_ms       = CONFIG_APC_UPS_POLL_INTERVAL_MS,
    .mqtt_en       = false,
    .mqtt_uri      = "",
    .mqtt_user     = "",
    .mqtt_pass     = "",
    .mqtt_prefix   = "homeassistant/sensor/ups",
    .mqtt_interval = 30,
    .mqtt_qos      = 1,
    .mqtt_ha       = 1,
};

#define LOAD_STR(h, k, dst) do { \
    size_t _l = sizeof(dst); \
    if (nvs_get_str(h, #k, dst, &_l) != ESP_OK) \
        strlcpy(dst, s_defaults.k, sizeof(dst)); \
} while(0)

#define LOAD_U16(h, k, dst) do { \
    if (nvs_get_u16(h, #k, &(dst)) != ESP_OK) (dst) = s_defaults.k; \
} while(0)

#define LOAD_U32(h, k, dst) do { \
    if (nvs_get_u32(h, #k, &(dst)) != ESP_OK) (dst) = s_defaults.k; \
} while(0)

#define LOAD_U8(h, k, dst) do { \
    if (nvs_get_u8(h, #k, &(dst)) != ESP_OK) (dst) = s_defaults.k; \
} while(0)

esp_err_t nvs_config_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        memcpy(&s_cfg, &s_defaults, sizeof(s_cfg));
        overrides_load();
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    LOAD_STR(h, ups_name,      s_cfg.ups_name);
    LOAD_STR(h, ups_desc,      s_cfg.ups_desc);
    LOAD_STR(h, nut_user,      s_cfg.nut_user);
    LOAD_STR(h, nut_pass,      s_cfg.nut_pass);
    LOAD_U16(h, nut_port,      s_cfg.nut_port);
    LOAD_U32(h, poll_ms,       s_cfg.poll_ms);
    LOAD_STR(h, mqtt_uri,      s_cfg.mqtt_uri);
    LOAD_STR(h, mqtt_user,     s_cfg.mqtt_user);
    LOAD_STR(h, mqtt_pass,     s_cfg.mqtt_pass);
    LOAD_STR(h, mqtt_prefix,   s_cfg.mqtt_prefix);
    LOAD_U32(h, mqtt_interval, s_cfg.mqtt_interval);
    LOAD_U8 (h, mqtt_qos,      s_cfg.mqtt_qos);
    LOAD_U8 (h, mqtt_ha,       s_cfg.mqtt_ha);

    uint8_t en = 0;
    if (nvs_get_u8(h, "mqtt_en", &en) != ESP_OK) en = 0;
    s_cfg.mqtt_en = (bool)en;

    nvs_close(h);
    overrides_load();
    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

const nvs_cfg_t *nvs_config_get(void)
{
    return &s_cfg;
}

esp_err_t nvs_config_save(const nvs_cfg_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    nvs_set_str(h, "ups_name",      cfg->ups_name);
    nvs_set_str(h, "ups_desc",      cfg->ups_desc);
    nvs_set_str(h, "nut_user",      cfg->nut_user);
    nvs_set_str(h, "nut_pass",      cfg->nut_pass);
    nvs_set_u16(h, "nut_port",      cfg->nut_port);
    nvs_set_u32(h, "poll_ms",       cfg->poll_ms);
    nvs_set_u8 (h, "mqtt_en",       (uint8_t)cfg->mqtt_en);
    nvs_set_str(h, "mqtt_uri",      cfg->mqtt_uri);
    nvs_set_str(h, "mqtt_user",     cfg->mqtt_user);
    nvs_set_str(h, "mqtt_pass",     cfg->mqtt_pass);
    nvs_set_str(h, "mqtt_prefix",   cfg->mqtt_prefix);
    nvs_set_u32(h, "mqtt_interval", cfg->mqtt_interval);
    nvs_set_u8 (h, "mqtt_qos",      cfg->mqtt_qos);
    nvs_set_u8 (h, "mqtt_ha",       cfg->mqtt_ha);

    esp_err_t err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        memcpy(&s_cfg, cfg, sizeof(s_cfg));
        ESP_LOGI(TAG, "Config saved to NVS");
    }
    return err;
}

/* ── Variable overrides ─────────────────────────────────────────────── */
static void parse_overrides_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    s_override_count = 0;
    if (!root) return;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (s_override_count >= MAX_OVERRIDES) break;
        if (!cJSON_IsString(item) || !item->valuestring[0]) continue;
        strlcpy(s_overrides[s_override_count].name, item->string, sizeof(s_overrides[0].name));
        strlcpy(s_overrides[s_override_count].value, item->valuestring, sizeof(s_overrides[0].value));
        s_override_count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d variable overrides", s_override_count);
}

static void overrides_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_overrides_json);
    if (nvs_get_str(h, "overrides", s_overrides_json, &len) != ESP_OK)
        strlcpy(s_overrides_json, "{}", sizeof(s_overrides_json));
    nvs_close(h);
    parse_overrides_json(s_overrides_json);
}

const char *nvs_override_get(const char *var_name)
{
    for (int i = 0; i < s_override_count; i++) {
        if (strcmp(s_overrides[i].name, var_name) == 0)
            return s_overrides[i].value;
    }
    return NULL;
}

const char *nvs_overrides_get_json(void)
{
    return s_overrides_json;
}

esp_err_t nvs_overrides_save(const char *json)
{
    if (strlen(json) >= sizeof(s_overrides_json) - 1) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "overrides", json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        strlcpy(s_overrides_json, json, sizeof(s_overrides_json));
        parse_overrides_json(json);
    }
    return err;
}
