#include "apc_ups.h"
#include "hid_parser.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "sdkconfig.h"

static const char *TAG = "apc_ups";

apc_ups_state_t g_ups = {0};
static SemaphoreHandle_t s_vars_mutex;
static hid_host_device_handle_t s_hid_device = NULL;

static void set_var(const char *name, const char *value)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < NUT_VAR_COUNT; i++) {
        if (!g_ups.vars[i].valid || strcmp(g_ups.vars[i].name, name) == 0) {
            strlcpy(g_ups.vars[i].name,  name,  sizeof(g_ups.vars[i].name));
            strlcpy(g_ups.vars[i].value, value, sizeof(g_ups.vars[i].value));
            g_ups.vars[i].valid = true;
            break;
        }
    }
    xSemaphoreGive(s_vars_mutex);
}

const char *apc_ups_get_var(const char *name)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < NUT_VAR_COUNT; i++) {
        if (g_ups.vars[i].valid && strcmp(g_ups.vars[i].name, name) == 0) {
            xSemaphoreGive(s_vars_mutex);
            return g_ups.vars[i].value;
        }
    }
    xSemaphoreGive(s_vars_mutex);
    return NULL;
}

void apc_ups_list_vars(char *buf, size_t buf_len)
{
    size_t off = 0;
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < NUT_VAR_COUNT && off < buf_len - 1; i++) {
        if (g_ups.vars[i].valid) {
            int n = snprintf(buf + off, buf_len - off,
                             "VAR ups %s \"%s\"\n",
                             g_ups.vars[i].name,
                             g_ups.vars[i].value);
            if (n > 0) off += n;
        }
    }
    xSemaphoreGive(s_vars_mutex);
}

static void set_static_vars(void)
{
    set_var("device.mfr",    "APC");
    set_var("device.model",  "Back-UPS BE850G2");
    set_var("driver.name",   "esp32-apc-hid");
    set_var("driver.version","1.0.0");
    set_var("ups.vendorid",  "051d");
    set_var("ups.productid", "0002");
}

static esp_err_t get_feature_report(uint8_t report_id,
                                    uint8_t *report_buf, size_t buf_len)
{
    if (!s_hid_device) return ESP_ERR_INVALID_STATE;
    memset(report_buf, 0, buf_len);
    esp_err_t ret = hid_class_request_get_report(s_hid_device,
                                                  HID_REPORT_TYPE_FEATURE,
                                                  report_id,
                                                  report_buf,
                                                  &buf_len);
    if (ret != ESP_OK)
        ESP_LOGD(TAG, "GET_REPORT rid=0x%02x failed: %s", report_id, esp_err_to_name(ret));
    return ret;
}

static bool read_field(uint32_t usage, int32_t *out)
{
    const hid_field_t *f = hid_find_field(&g_ups.report_map, usage,
                                           HID_ITEM_TYPE_FEATURE);
    if (!f) return false;

    uint8_t buf[65];
    size_t  report_bytes = (f->bit_offset + f->bit_size + 7) / 8 + 1;
    if (report_bytes > sizeof(buf)) report_bytes = sizeof(buf);

    if (get_feature_report(f->report_id, buf, report_bytes) != ESP_OK)
        return false;

    *out = hid_extract_field_value(buf + 1, report_bytes - 1, f);
    return true;
}

static void update_ups_status(void)
{
    int32_t ac_present = 0, charging = 0, discharging = 0;
    int32_t shutdown_im = 0, low_batt = 0, need_repl = 0;

    bool got_ac   = read_field(HID_USAGE_BS_AC_PRESENT,         &ac_present);
    bool got_chrg = read_field(HID_USAGE_BS_CHARGING,           &charging);
                    read_field(HID_USAGE_BS_DISCHARGING,        &discharging);
                    read_field(HID_USAGE_PD_SHUTDOWN_IMMINENT,  &shutdown_im);
                    read_field(HID_USAGE_BS_BELOW_REMAINING_CAP,&low_batt);
                    read_field(HID_USAGE_BS_NEED_REPLACEMENT,   &need_repl);

    char status[64] = "";

    if (!got_ac && !got_chrg) {
        strlcpy(status, "OFF", sizeof(status));
    } else {
        strlcat(status, ac_present ? "OL" : "OB", sizeof(status));
        if (charging)    strlcat(status, " CHRG",   sizeof(status));
        if (discharging) strlcat(status, " DISCHRG",sizeof(status));
        if (low_batt)    strlcat(status, " LB",     sizeof(status));
        if (shutdown_im) strlcat(status, " SD",     sizeof(status));
        if (need_repl)   strlcat(status, " RB",     sizeof(status));
    }

    set_var("ups.status", status);
}

static void poll_all_vars(void)
{
    char buf[32];
    int32_t val;

    if (read_field(HID_USAGE_BS_REMAINING_CAPACITY, &val)) {
        snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("battery.charge", buf);
    }

    if (read_field(HID_USAGE_BS_RUNTIME_TO_EMPTY, &val)) {
        snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("battery.runtime", buf);
    }

    if (read_field(HID_USAGE_BS_VOLTAGE, &val)) {
        const hid_field_t *f = hid_find_field(&g_ups.report_map,
                                               HID_USAGE_BS_VOLTAGE,
                                               HID_ITEM_TYPE_FEATURE);
        if (f && f->logical_max > 100)
            snprintf(buf, sizeof(buf), "%.1f", val / 1000.0f);
        else
            snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("battery.voltage", buf);
    }

    if (read_field(HID_USAGE_PD_VOLTAGE, &val)) {
        const hid_field_t *f = hid_find_field(&g_ups.report_map,
                                               HID_USAGE_PD_VOLTAGE,
                                               HID_ITEM_TYPE_FEATURE);
        if (f && f->logical_max > 300)
            snprintf(buf, sizeof(buf), "%.1f", val / 10.0f);
        else
            snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("input.voltage", buf);
    }

    if (read_field(HID_USAGE_PD_PERCENT_LOAD, &val)) {
        snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("ups.load", buf);
    }

    if (read_field(HID_USAGE_BS_WARNING_CAP_LIMIT, &val)) {
        snprintf(buf, sizeof(buf), "%"PRId32, val);
        set_var("battery.charge.warning", buf);
    }

    update_ups_status();
}

static void hid_interface_event_cb(hid_host_device_handle_t hid_device_handle,
                                   const hid_host_interface_event_t event,
                                   void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID device disconnected");
        if (hid_device_handle == s_hid_device) {
            hid_host_device_close(hid_device_handle);
            s_hid_device        = NULL;
            g_ups.usb_connected = false;
            g_ups.desc_parsed   = false;
            memset(&g_ups.report_map, 0, sizeof(g_ups.report_map));
            set_var("ups.status", "OFF");
        }
        break;
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        break;
    }
}

static void hid_device_event_cb(hid_host_device_handle_t hid_device_handle,
                                const hid_host_driver_event_t event,
                                void *arg)
{
    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED: {
        hid_host_dev_info_t info;
        hid_host_get_device_info(hid_device_handle, &info);
        ESP_LOGI(TAG, "HID device connected: VID=0x%04x PID=0x%04x",
                 info.VID, info.PID);

        if (info.VID != APC_VID || info.PID != APC_PID_BACKUPS) {
            ESP_LOGW(TAG, "Not an APC Back-UPS (051d:0002) — ignoring");
            break;
        }

        hid_host_device_config_t dev_cfg = {
            .callback     = hid_interface_event_cb,
            .callback_arg = NULL,
        };
        hid_host_device_open(hid_device_handle, &dev_cfg);
        s_hid_device = hid_device_handle;

        {
            size_t   desc_len = 0;
            uint8_t *desc_ptr = hid_host_get_report_descriptor(hid_device_handle,
                                                                &desc_len);
            if (desc_ptr && desc_len > 0) {
                hid_parse_report_descriptor(desc_ptr, desc_len, &g_ups.report_map);
#if CONFIG_APC_HID_DEBUG
                hid_dump_report_map(&g_ups.report_map);
#endif
                g_ups.desc_parsed   = true;
                g_ups.usb_connected = true;
                set_static_vars();
                hid_host_device_start(hid_device_handle);
                ESP_LOGI(TAG, "APC Back-UPS ready, starting polling");
            } else {
                ESP_LOGE(TAG, "Failed to get report descriptor");
                s_hid_device = NULL;
                hid_host_device_close(hid_device_handle);
            }
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t apc_ups_init(void)
{
    s_vars_mutex = xSemaphoreCreateMutex();
    if (!s_vars_mutex) return ESP_ERR_NO_MEM;

    memset(&g_ups, 0, sizeof(g_ups));
    set_var("ups.status", "OFF");

    hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = hid_device_event_cb,
        .callback_arg           = NULL,
    };

    esp_err_t ret = hid_host_install(&hid_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "APC UPS HID driver installed, waiting for device");
    return ESP_OK;
}

void apc_ups_poll_task(void *arg)
{
    esp_task_wdt_add(NULL);
    while (1) {
        /* Break poll interval into WDT-friendly chunks */
        uint32_t remaining_ms = CONFIG_APC_UPS_POLL_INTERVAL_MS;
        while (remaining_ms > 0) {
            uint32_t chunk = remaining_ms > 5000 ? 5000 : remaining_ms;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            esp_task_wdt_reset();
            remaining_ms -= chunk;
        }
        if (g_ups.usb_connected && g_ups.desc_parsed)
            poll_all_vars();
    }
}

void apc_ups_get_data(ups_data_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *v;
#define GETF(field, key) do { v = apc_ups_get_var(key); \
    if (v) out->field = strtof(v, NULL); } while(0)
#define GETS(field, key) do { v = apc_ups_get_var(key); \
    if (v) strlcpy(out->field, v, sizeof(out->field)); } while(0)

    GETS(status,                "ups.status");
    GETS(mfr,                   "device.mfr");
    GETS(model,                 "device.model");
    GETS(firmware,              "ups.firmware");
    GETS(serial,                "device.serial");
    GETS(test_result,           "ups.test.result");
    GETF(battery_charge,        "battery.charge");
    GETF(battery_runtime,       "battery.runtime");
    GETF(battery_voltage,       "battery.voltage");
    GETF(battery_charge_low,    "battery.charge.low");
    GETF(battery_charge_warning,"battery.charge.warning");
    GETF(input_voltage,         "input.voltage");
    GETF(ups_load,              "ups.load");
    GETF(ups_temperature,       "ups.temperature");
    GETF(ups_realpower_nominal, "ups.realpower.nominal");
#undef GETF
#undef GETS
}
