#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char     ups_name[32];
    char     ups_desc[64];
    char     nut_user[32];
    char     nut_pass[64];
    uint16_t nut_port;
    uint32_t poll_ms;
    bool     mqtt_en;
    char     mqtt_uri[128];
    char     mqtt_user[64];
    char     mqtt_pass[64];
    char     mqtt_prefix[64];
    uint32_t mqtt_interval;
    uint8_t  mqtt_qos;
    uint8_t  mqtt_ha;
} nvs_cfg_t;

esp_err_t        nvs_config_init(void);
const nvs_cfg_t *nvs_config_get(void);
esp_err_t        nvs_config_save(const nvs_cfg_t *cfg);

/* Variable overrides — stored as JSON in NVS */
#define MAX_OVERRIDES 32
const char      *nvs_override_get(const char *var_name);
const char      *nvs_overrides_get_json(void);
esp_err_t        nvs_overrides_save(const char *json);
