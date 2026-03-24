#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "apc_ups.h"
#include "nvs_config.h"
#include "mqtt_pub.h"
#include "web_server.h"

static const char *TAG = "web";

extern const char index_html_start[]  asm("_binary_index_html_start");
extern const char index_html_end[]    asm("_binary_index_html_end");
extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[]   asm("_binary_config_html_end");

static httpd_handle_t s_server = NULL;

static void field_str(cJSON *obj, const char *key, char *dst, size_t maxlen)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (v && cJSON_IsString(v) && v->valuestring)
        strlcpy(dst, v->valuestring, maxlen);
}

/* ── GET / ───────────────────────────────────────────────────────────── */
static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start - 1));
    return ESP_OK;
}

/* ── GET /config ─────────────────────────────────────────────────────── */
static esp_err_t handle_config_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, config_html_start,
                    (ssize_t)(config_html_end - config_html_start - 1));
    return ESP_OK;
}

/* ── GET /api/status ─────────────────────────────────────────────────── */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    ups_data_t d;
    apc_ups_get_data(&d);
    const nvs_cfg_t *cfg = nvs_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "_ups_name",              cfg->ups_name);
    cJSON_AddStringToObject(root, "ups.status",             d.status);
    cJSON_AddStringToObject(root, "ups.mfr",                d.mfr);
    cJSON_AddStringToObject(root, "ups.model",              d.model);
    cJSON_AddStringToObject(root, "ups.firmware",           d.firmware);
    cJSON_AddStringToObject(root, "device.serial",          d.serial);
    cJSON_AddStringToObject(root, "ups.test.result",        d.test_result);

    char buf[32];
#define ADDF(key, fmt, val) do { snprintf(buf, sizeof(buf), fmt, val); \
    cJSON_AddStringToObject(root, key, buf); } while(0)

    ADDF("battery.charge",          "%.1f", d.battery_charge);
    ADDF("battery.runtime",         "%.0f", d.battery_runtime);
    ADDF("battery.voltage",         "%.2f", d.battery_voltage);
    ADDF("battery.charge.low",      "%.1f", d.battery_charge_low);
    ADDF("battery.charge.warning",  "%.1f", d.battery_charge_warning);
    ADDF("input.voltage",           "%.1f", d.input_voltage);
    ADDF("ups.load",                "%.1f", d.ups_load);
    ADDF("ups.temperature",         "%.1f", d.ups_temperature);
    ADDF("ups.realpower.nominal",   "%.0f", d.ups_realpower_nominal);
#undef ADDF

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── GET /api/config ─────────────────────────────────────────────────── */
static esp_err_t handle_api_config_get(httpd_req_t *req)
{
    const nvs_cfg_t *c = nvs_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ups_name",      c->ups_name);
    cJSON_AddStringToObject(root, "ups_desc",      c->ups_desc);
    cJSON_AddStringToObject(root, "nut_user",      c->nut_user);
    cJSON_AddStringToObject(root, "nut_pass",      c->nut_pass);
    cJSON_AddNumberToObject(root, "nut_port",      c->nut_port);
    cJSON_AddNumberToObject(root, "poll_ms",       c->poll_ms);
    cJSON_AddBoolToObject  (root, "mqtt_en",       c->mqtt_en);
    cJSON_AddStringToObject(root, "mqtt_uri",      c->mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_user",     c->mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_pass",     c->mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_prefix",   c->mqtt_prefix);
    cJSON_AddNumberToObject(root, "mqtt_interval", c->mqtt_interval);
    cJSON_AddNumberToObject(root, "mqtt_qos",      c->mqtt_qos);
    cJSON_AddNumberToObject(root, "mqtt_ha",       c->mqtt_ha);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── POST /api/config ────────────────────────────────────────────────── */
static esp_err_t handle_api_config_post(httpd_req_t *req)
{
    char buf[1024];
    int  total = 0, r;
    while (total < (int)sizeof(buf) - 1) {
        r = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    nvs_cfg_t c;
    memcpy(&c, nvs_config_get(), sizeof(nvs_cfg_t));

    field_str(root, "ups_name",    c.ups_name,    sizeof(c.ups_name));
    field_str(root, "ups_desc",    c.ups_desc,    sizeof(c.ups_desc));
    field_str(root, "nut_user",    c.nut_user,    sizeof(c.nut_user));
    field_str(root, "nut_pass",    c.nut_pass,    sizeof(c.nut_pass));
    field_str(root, "mqtt_uri",    c.mqtt_uri,    sizeof(c.mqtt_uri));
    field_str(root, "mqtt_user",   c.mqtt_user,   sizeof(c.mqtt_user));
    field_str(root, "mqtt_pass",   c.mqtt_pass,   sizeof(c.mqtt_pass));
    field_str(root, "mqtt_prefix", c.mqtt_prefix, sizeof(c.mqtt_prefix));

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "nut_port")))      c.nut_port      = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "poll_ms")))       c.poll_ms       = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "mqtt_en")))       c.mqtt_en       = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "mqtt_interval"))) c.mqtt_interval = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "mqtt_qos")))      c.mqtt_qos      = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "mqtt_ha")))       c.mqtt_ha       = (uint8_t)v->valuedouble;

    cJSON_Delete(root);

    if (nvs_config_save(&c) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    mqtt_pub_reconfigure();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── router ──────────────────────────────────────────────────────────── */
static const httpd_uri_t s_routes[] = {
    { .uri="/",           .method=HTTP_GET,  .handler=handle_index           },
    { .uri="/config",     .method=HTTP_GET,  .handler=handle_config_page     },
    { .uri="/api/status", .method=HTTP_GET,  .handler=handle_api_status      },
    { .uri="/api/config", .method=HTTP_GET,  .handler=handle_api_config_get  },
    { .uri="/api/config", .method=HTTP_POST, .handler=handle_api_config_post },
};

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    for (int i = 0; i < (int)(sizeof(s_routes)/sizeof(s_routes[0])); i++)
        httpd_register_uri_handler(s_server, &s_routes[i]);

    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
