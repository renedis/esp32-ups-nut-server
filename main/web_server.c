#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "ups_driver.h"
#include "nvs_config.h"
#include "mqtt_pub.h"
#include "web_server.h"

static const char *TAG = "web";

extern const char index_html_start[]  asm("_binary_index_html_start");
extern const char index_html_end[]    asm("_binary_index_html_end");
extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[]   asm("_binary_config_html_end");

static httpd_handle_t s_server = NULL;

/* ── Log ring buffer for serial console ─────────────────────────────── */
#define LOG_RING_SIZE  (16 * 1024)
static char   s_log_ring[LOG_RING_SIZE];
static size_t s_log_head = 0;   /* next write position */
static size_t s_log_len  = 0;   /* bytes stored        */
static vprintf_like_t s_orig_vprintf = NULL;
static volatile bool  s_log_capture  = false;

/* Minimal log hook: capture into ring buffer, then forward to UART.
 * Uses a static buffer (not reentrant, but ESP_LOG serializes via lock). */
static int log_vprintf(const char *fmt, va_list args)
{
    static char tmp[256];  /* static — no stack cost */
    if (s_log_capture) {
        va_list args_copy;
        va_copy(args_copy, args);
        int n = vsnprintf(tmp, sizeof(tmp), fmt, args_copy);
        va_end(args_copy);
        if (n > 0) {
            if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
            for (int i = 0; i < n; i++) {
                s_log_ring[s_log_head] = tmp[i];
                s_log_head = (s_log_head + 1) % LOG_RING_SIZE;
            }
            s_log_len += n;
            if (s_log_len > LOG_RING_SIZE) s_log_len = LOG_RING_SIZE;
        }
    }
    return s_orig_vprintf(fmt, args);
}

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
    /* Send in chunks to avoid large stack allocation in httpd_resp_send */
    const char *p = config_html_start;
    size_t remaining = config_html_end - config_html_start - 1;
    while (remaining > 0) {
        size_t chunk = remaining > 4096 ? 4096 : remaining;
        if (httpd_resp_send_chunk(req, p, chunk) != ESP_OK) return ESP_FAIL;
        p += chunk;
        remaining -= chunk;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /api/status ─────────────────────────────────────────────────── */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    ups_data_t d;
    ups_driver_get_data(&d);
    const nvs_cfg_t *cfg = nvs_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "_ups_name",              cfg->ups_name);
    cJSON_AddStringToObject(root, "ups.status",             d.status);
    cJSON_AddStringToObject(root, "ups.mfr",                d.mfr);
    cJSON_AddStringToObject(root, "ups.model",              d.model);
    cJSON_AddStringToObject(root, "ups.firmware",           d.firmware);
    cJSON_AddStringToObject(root, "device.serial",          d.serial);
    cJSON_AddStringToObject(root, "ups.test.result",        d.ups_test_result);

    char buf[32];
#define ADDF(key, fmt, val) do { snprintf(buf, sizeof(buf), fmt, val); \
    cJSON_AddStringToObject(root, key, buf); } while(0)

    ADDF("battery.charge",          "%.2f", d.battery_charge);
    ADDF("battery.runtime",         "%.0f", d.battery_runtime);
    ADDF("battery.voltage",         "%.2f", d.battery_voltage);
    ADDF("battery.voltage.nominal", "%.2f", d.battery_voltage_nominal);
    ADDF("battery.charge.low",      "%.2f", d.battery_charge_low);
    ADDF("battery.charge.warning",  "%.2f", d.battery_charge_warning);
    ADDF("battery.temperature",     "%.2f", d.battery_temperature);
    ADDF("input.voltage",           "%.2f", d.input_voltage);
    ADDF("input.voltage.nominal",   "%.2f", d.input_voltage_nominal);
    ADDF("input.frequency",         "%.2f", d.input_frequency);
    ADDF("input.transfer.low",      "%.2f", d.input_transfer_low);
    ADDF("input.transfer.high",     "%.2f", d.input_transfer_high);
    ADDF("output.voltage",          "%.2f", d.output_voltage);
    ADDF("output.current",          "%.2f", d.output_current);
    ADDF("output.frequency",        "%.2f", d.output_frequency);
    ADDF("ups.load",                "%.2f", d.ups_load);
    ADDF("ups.temperature",         "%.2f", d.ups_temperature);
    ADDF("ups.realpower.nominal",   "%.0f", d.ups_realpower_nominal);
    ADDF("ups.realpower",           "%.2f", d.ups_realpower);
    cJSON_AddStringToObject(root, "ups.beeper.status", d.ups_beeper_status);
#undef ADDF

    /* USB debug fields */
    {
        uint16_t vid, pid; bool ever_seen;
        ups_driver_get_last_usb(&vid, &pid, &ever_seen);
        cJSON_AddBoolToObject(root, "_usb_connected", ups_driver_is_connected());
        cJSON_AddBoolToObject(root, "_usb_ever_seen", ever_seen);
        if (ever_seen) {
            char vidpid[12];
            snprintf(vidpid, sizeof(vidpid), "%04x:%04x", vid, pid);
            cJSON_AddStringToObject(root, "_usb_vidpid", vidpid);
        }
    }

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

/* ── GET /api/log — return log ring buffer as plain text ─────────────── */
static esp_err_t handle_api_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (s_log_len == 0) {
        httpd_resp_sendstr(req, "(no log output yet)\n");
        return ESP_OK;
    }

    size_t start;
    size_t len = s_log_len;
    if (len >= LOG_RING_SIZE) {
        len = LOG_RING_SIZE;
        start = s_log_head;  /* oldest byte */
    } else {
        start = (s_log_head + LOG_RING_SIZE - len) % LOG_RING_SIZE;
    }

    /* Send in up to 2 chunks (ring may wrap) */
    if (start + len <= LOG_RING_SIZE) {
        httpd_resp_send(req, &s_log_ring[start], len);
    } else {
        size_t first = LOG_RING_SIZE - start;
        httpd_resp_send_chunk(req, &s_log_ring[start], first);
        httpd_resp_send_chunk(req, &s_log_ring[0], len - first);
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}

/* ── GET /api/overrides ──────────────────────────────────────────────── */
static esp_err_t handle_api_overrides_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, nvs_overrides_get_json());
    return ESP_OK;
}

/* ── POST /api/overrides ─��──────��────────────────────────────────────── */
static esp_err_t handle_api_overrides_post(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 3900) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
        return ESP_FAIL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int total = 0, r;
    while (total < content_len) {
        r = httpd_req_recv(req, buf + total, content_len - total);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error"); return ESP_FAIL; }
        total += r;
    }
    buf[total] = '\0';

    esp_err_t err = nvs_overrides_save(buf);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /api/power_sensors ──────────────────────────────────────────── */
static esp_err_t handle_api_power_sensors(httpd_req_t *req)
{
    const nvs_cfg_t *cfg = nvs_config_get();
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg->mqtt_en && cfg->mqtt_uri[0]);
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_pub_is_connected());
    cJSON_AddStringToObject(root, "selected_topic", cfg->power_topic);
    cJSON_AddStringToObject(root, "selected_json_key", cfg->power_json_key);

    const power_sensor_t *sensors;
    int count = mqtt_get_power_sensors(&sensors);
    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; i < count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name",     sensors[i].name);
        cJSON_AddStringToObject(s, "topic",    sensors[i].topic);
        cJSON_AddStringToObject(s, "json_key", sensors[i].json_key);
        cJSON_AddStringToObject(s, "uid",      sensors[i].unique_id);
        cJSON_AddItemToArray(arr, s);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── POST /api/power_sensor — select a power sensor ─────────────────── */
static esp_err_t handle_api_power_sensor_post(httpd_req_t *req)
{
    char buf[256];
    int total = 0, r;
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
    memcpy(&c, nvs_config_get(), sizeof(c));

    cJSON *v;
    v = cJSON_GetObjectItem(root, "topic");
    if (v && cJSON_IsString(v))
        strlcpy(c.power_topic, v->valuestring, sizeof(c.power_topic));
    else
        c.power_topic[0] = '\0';

    v = cJSON_GetObjectItem(root, "json_key");
    if (v && cJSON_IsString(v))
        strlcpy(c.power_json_key, v->valuestring, sizeof(c.power_json_key));
    else
        c.power_json_key[0] = '\0';

    cJSON_Delete(root);

    if (nvs_config_save(&c) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    /* Subscribe/unsubscribe immediately */
    mqtt_subscribe_power_topic(c.power_topic, c.power_json_key);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Deferred restart task — clean shutdown outside httpd context ──── */
static void ota_restart_task(void *arg)
{
    /* Give httpd time to finish sending the "OK" response */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGW(TAG, "OTA: stopping all services before restart...");

    /* Disable log capture first so shutdown logs don't contend */
    s_log_capture = false;
    if (s_orig_vprintf) {
        esp_log_set_vprintf(s_orig_vprintf);
        s_orig_vprintf = NULL;
    }

    /* Stop httpd — this closes the listening socket AND the internal
     * UDP control socket, preventing stale socket state after reboot */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* Stop other network services */
    extern void nut_server_stop(void);
    extern void mqtt_pub_stop(void);
    nut_server_stop();
    mqtt_pub_stop();

    /* Let lwIP finish closing all sockets */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGW(TAG, "OTA: restarting now");
    esp_restart();
    vTaskDelete(NULL);  /* never reached */
}

/* ── POST /api/ota ───────────────────────────────────────────────────── */
static esp_err_t handle_ota(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);

    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    int content_len = req->content_len;
    ESP_LOGI(TAG, "OTA: content_len=%d → %s (0x%lx)",
             content_len, update_part->label, (unsigned long)update_part->address);
    if (content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content length");
        return ESP_FAIL;
    }

    esp_task_wdt_reset();

    char *buf = malloc(8192);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(update_part, content_len, &ota_handle);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    int remaining = content_len;
    int total_written = 0;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining < 8192 ? remaining : 8192;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA recv error at %d/%d (ret=%d)", total_written, content_len, received);
            ok = false;
            break;
        }
        if (esp_ota_write(ota_handle, buf, received) != ESP_OK) {
            ESP_LOGE(TAG, "OTA write error at %d", total_written);
            ok = false;
            break;
        }
        total_written += received;
        remaining -= received;
    }
    free(buf);

    if (!ok) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: wrote %d bytes, validating...", total_written);
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA validation failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success — restarting via clean shutdown task");
    httpd_resp_sendstr(req, "OK");

    /* Spawn a separate task to do the restart — NOT from httpd handler context.
     * This ensures httpd can finish sending the response and we can cleanly
     * tear down all sockets before esp_restart(). */
    xTaskCreate(ota_restart_task, "ota_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

/* ── GET /api/hid — dump parsed HID report map as plain text ─────────── */
static esp_err_t handle_api_hid(httpd_req_t *req)
{
    const hid_report_map_t *map = ups_driver_get_report_map();
    static const char *type_name[] = { "INPUT", "OUTPUT", "FEATURE" };

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char line[256];
    snprintf(line, sizeof(line), "HID Report Map: %u fields\n\n", map->count);
    httpd_resp_sendstr_chunk(req, line);

    for (uint8_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        int n = snprintf(line, sizeof(line),
            "[%3u] %-7s rid=0x%02x usage=0x%08lx bit_off=%3u bit_sz=%u "
            "lmin=%ld lmax=%ld pmin=%ld pmax=%ld unit=0x%08lx uexp=%d coll=%u[",
            i, type_name[f->item_type], f->report_id,
            (unsigned long)f->usage, f->bit_offset, f->bit_size,
            (long)f->logical_min, (long)f->logical_max,
            (long)f->phy_min, (long)f->phy_max,
            (unsigned long)f->unit, (int)f->unit_exp,
            f->collection_depth);
        for (uint8_t d = 0; d < f->collection_depth && n < (int)sizeof(line) - 16; d++)
            n += snprintf(line + n, sizeof(line) - n, "%s0x%08lx",
                          d ? "," : "", (unsigned long)f->collection_path[d]);
        n += snprintf(line + n, sizeof(line) - n, "]\n");
        (void)n;
        httpd_resp_sendstr_chunk(req, line);
    }

    httpd_resp_sendstr_chunk(req, NULL);  /* end chunked response */
    return ESP_OK;
}

/* ── router ──────────────────────────────────────────────────────────── */
static const httpd_uri_t s_routes[] = {
    { .uri="/",           .method=HTTP_GET,  .handler=handle_index           },
    { .uri="/config",     .method=HTTP_GET,  .handler=handle_config_page     },
    { .uri="/api/status", .method=HTTP_GET,  .handler=handle_api_status      },
    { .uri="/api/config", .method=HTTP_GET,  .handler=handle_api_config_get  },
    { .uri="/api/config", .method=HTTP_POST, .handler=handle_api_config_post },
    { .uri="/api/hid",    .method=HTTP_GET,  .handler=handle_api_hid        },
    { .uri="/api/log",    .method=HTTP_GET,  .handler=handle_api_log        },
    { .uri="/api/power_sensors",  .method=HTTP_GET,  .handler=handle_api_power_sensors },
    { .uri="/api/power_sensor",   .method=HTTP_POST, .handler=handle_api_power_sensor_post },
    { .uri="/api/overrides", .method=HTTP_GET,  .handler=handle_api_overrides_get },
    { .uri="/api/overrides", .method=HTTP_POST, .handler=handle_api_overrides_post },
    { .uri="/api/ota",    .method=HTTP_POST, .handler=handle_ota,
      .user_ctx=NULL },
};

esp_err_t web_server_start(void)
{
    esp_log_level_set("httpd", ESP_LOG_DEBUG);
    esp_log_level_set("httpd_txrx", ESP_LOG_DEBUG);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 16;
    cfg.stack_size       = 16384;
    cfg.lru_purge_enable = true;
    cfg.ctrl_port        = 32769;  /* avoid potential bind conflict after OTA reboot */

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    for (int i = 0; i < (int)(sizeof(s_routes)/sizeof(s_routes[0])); i++)
        httpd_register_uri_handler(s_server, &s_routes[i]);

    /* Hook ESP_LOG into ring buffer AFTER server is fully started */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    s_log_capture = true;

    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
