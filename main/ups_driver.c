#include "ups_driver.h"
#include "hid_parser.h"

/* Vendor subdrivers */
#include "apc_subdriver.h"
extern const ups_subdriver_t mge_subdriver;
extern const ups_subdriver_t belkin_subdriver;
extern const ups_subdriver_t cps_subdriver;
extern const ups_subdriver_t tripplite_subdriver;
extern const ups_subdriver_t powercom_subdriver;
extern const ups_subdriver_t powervar_subdriver;
extern const ups_subdriver_t salicru_subdriver;
extern const ups_subdriver_t delta_subdriver;
extern const ups_subdriver_t ever_subdriver;
extern const ups_subdriver_t arduino_subdriver;
extern const ups_subdriver_t idowell_subdriver;
extern const ups_subdriver_t legrand_subdriver;
extern const ups_subdriver_t openups_subdriver;
extern const ups_subdriver_t ecoflow_subdriver;
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

static const char *TAG = "ups_driver";

/* -----------------------------------------------------------------------
 * Registered subdrivers — add new vendors here.
 * The first subdriver whose VID:PID table matches the connected device
 * is selected; order matters only when two subdrivers claim the same ID.
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Registered subdrivers — first match wins.
 * Order matters only when two subdrivers claim the same VID:PID.
 * PIDs of 0xffff in a device table entry match any PID from that vendor.
 * ----------------------------------------------------------------------- */
static const ups_subdriver_t *s_subdriver_list[] = {
    &apc_subdriver,
    &mge_subdriver,
    &belkin_subdriver,
    &cps_subdriver,
    &tripplite_subdriver,
    &powercom_subdriver,
    &powervar_subdriver,
    &salicru_subdriver,
    &delta_subdriver,
    &ever_subdriver,
    &arduino_subdriver,
    &idowell_subdriver,
    &legrand_subdriver,
    &openups_subdriver,
    &ecoflow_subdriver,
    NULL   /* sentinel */
};

/* -----------------------------------------------------------------------
 * Runtime state
 * ----------------------------------------------------------------------- */
typedef struct {
    char name[32];
    char value[64];
    bool valid;
} nut_var_t;

static nut_var_t             s_vars[UPS_VAR_COUNT];
static SemaphoreHandle_t     s_vars_mutex;
static hid_host_device_handle_t s_hid_device   = NULL;
static hid_report_map_t      s_report_map;
static bool                  s_usb_connected   = false;
static bool                  s_desc_parsed     = false;
static const ups_subdriver_t *s_subdriver       = NULL;

/* -----------------------------------------------------------------------
 * Variable store (internal helpers)
 * ----------------------------------------------------------------------- */
static void set_var(const char *name, const char *value)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT; i++) {
        if (!s_vars[i].valid || strcmp(s_vars[i].name, name) == 0) {
            strlcpy(s_vars[i].name,  name,  sizeof(s_vars[i].name));
            strlcpy(s_vars[i].value, value, sizeof(s_vars[i].value));
            s_vars[i].valid = true;
            break;
        }
    }
    xSemaphoreGive(s_vars_mutex);
}

static bool is_var_set(const char *name)
{
    bool found = false;
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT; i++) {
        if (s_vars[i].valid && strcmp(s_vars[i].name, name) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_vars_mutex);
    return found;
}

static void clear_vars(void)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    memset(s_vars, 0, sizeof(s_vars));
    xSemaphoreGive(s_vars_mutex);
}

/* -----------------------------------------------------------------------
 * Variable store (public API)
 * ----------------------------------------------------------------------- */
const char *ups_driver_get_var(const char *name)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT; i++) {
        if (s_vars[i].valid && strcmp(s_vars[i].name, name) == 0) {
            xSemaphoreGive(s_vars_mutex);
            return s_vars[i].value;
        }
    }
    xSemaphoreGive(s_vars_mutex);
    return NULL;
}

void ups_driver_list_vars(char *buf, size_t buf_len)
{
    size_t off = 0;
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT && off < buf_len - 1; i++) {
        if (!s_vars[i].valid) continue;
        int n = snprintf(buf + off, buf_len - off,
                         "VAR ups %s \"%s\"\n",
                         s_vars[i].name, s_vars[i].value);
        if (n > 0) off += (size_t)n;
    }
    xSemaphoreGive(s_vars_mutex);
}

bool        ups_driver_is_connected(void)  { return s_usb_connected; }
const char *ups_driver_get_ups_name(void)
{
    const char *m = ups_driver_get_var("device.model");
    return m ? m : "UPS";
}

/* -----------------------------------------------------------------------
 * Standard scale functions (shared with subdrivers via ups_driver.h)
 * ----------------------------------------------------------------------- */
void ups_scale_voltage(char *buf, size_t len, int32_t val, const hid_field_t *f)
{
    if      (f->logical_max > 10000) snprintf(buf, len, "%.1f", val / 1000.0f);
    else if (f->logical_max > 300)   snprintf(buf, len, "%.1f", val / 10.0f);
    else                             snprintf(buf, len, "%"PRId32, val);
}

void ups_scale_current(char *buf, size_t len, int32_t val, const hid_field_t *f)
{
    if (f->logical_max > 100) snprintf(buf, len, "%.2f", val / 100.0f);
    else                      snprintf(buf, len, "%.2f", (float)val);
}

void ups_scale_temperature(char *buf, size_t len, int32_t val, const hid_field_t *f)
{
    /* HID reports temp in 0.1 K when logical_max > 1000, else direct Celsius */
    if (f->logical_max > 1000) snprintf(buf, len, "%.1f", val / 10.0f - 273.15f);
    else                       snprintf(buf, len, "%"PRId32, val);
}

void ups_scale_frequency(char *buf, size_t len, int32_t val, const hid_field_t *f)
{
    if (f->logical_max > 100) snprintf(buf, len, "%.1f", val / 10.0f);
    else                      snprintf(buf, len, "%"PRId32, val);
}

void ups_scale_mfr_date(char *buf, size_t len, int32_t val, const hid_field_t *f)
{
    (void)f;
    /* DOS/FAT packed date: bits[15:9]=year-1980, [8:5]=month, [4:0]=day */
    snprintf(buf, len, "%04d/%02d/%02d",
             ((val >> 9) & 0x7F) + 1980,
             (val >> 5) & 0x0F,
              val       & 0x1F);
}

/* -----------------------------------------------------------------------
 * HID report I/O
 * ----------------------------------------------------------------------- */
static esp_err_t get_feature_report(uint8_t report_id, uint8_t *buf, size_t buf_len)
{
    if (!s_hid_device) return ESP_ERR_INVALID_STATE;
    memset(buf, 0, buf_len);
    esp_err_t ret = hid_class_request_get_report(s_hid_device,
                                                  HID_REPORT_TYPE_FEATURE,
                                                  report_id, buf, &buf_len);
    if (ret != ESP_OK)
        ESP_LOGD(TAG, "GET_REPORT rid=0x%02x failed: %s",
                 report_id, esp_err_to_name(ret));
    return ret;
}

static const hid_field_t *fetch_field(const hid_field_t *f, int32_t *out)
{
    uint8_t buf[65];
    size_t  report_bytes = (f->bit_offset + f->bit_size + 7) / 8 + 1;
    if (report_bytes > sizeof(buf)) report_bytes = sizeof(buf);
    if (get_feature_report(f->report_id, buf, report_bytes) != ESP_OK) return NULL;
    *out = hid_extract_field_value(buf + 1, report_bytes - 1, f);
    return f;
}

static const hid_field_t *read_field_val(uint32_t usage, int32_t *out)
{
    const hid_field_t *f = hid_find_field(&s_report_map, usage, HID_ITEM_TYPE_FEATURE);
    return f ? fetch_field(f, out) : NULL;
}

static const hid_field_t *read_field_coll_val(uint32_t usage, uint32_t coll, int32_t *out)
{
    const hid_field_t *f = hid_find_field_in_collection(&s_report_map, usage,
                                                         HID_ITEM_TYPE_FEATURE, coll);
    return f ? fetch_field(f, out) : NULL;
}

/* -----------------------------------------------------------------------
 * Lookup table helper
 * ----------------------------------------------------------------------- */
static const char *lkp_lookup(const ups_lkp_t *lkp, int32_t val)
{
    if (!lkp) return NULL;
    for (; lkp->nut_val != NULL; lkp++)
        if (lkp->hid_val == val) return lkp->nut_val;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Subdriver / device detection
 * ----------------------------------------------------------------------- */
/* pid==0xffff in a device table entry is a vendor wildcard (any PID). */
static bool device_matches(const ups_vid_pid_t *d, uint16_t vid, uint16_t pid)
{
    return d->vid == vid && (d->pid == pid || d->pid == 0xffff);
}

static const ups_subdriver_t *find_subdriver(uint16_t vid, uint16_t pid)
{
    for (int i = 0; s_subdriver_list[i]; i++)
        for (const ups_vid_pid_t *d = s_subdriver_list[i]->devices; d->vid; d++)
            if (device_matches(d, vid, pid)) return s_subdriver_list[i];
    return NULL;
}

static const ups_vid_pid_t *find_device_entry(uint16_t vid, uint16_t pid)
{
    for (int i = 0; s_subdriver_list[i]; i++)
        for (const ups_vid_pid_t *d = s_subdriver_list[i]->devices; d->vid; d++)
            if (device_matches(d, vid, pid)) return d;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Static variables — set once when the device connects.
 * Covers identity fields and UPS_MAP_STATIC mapping entries.
 * ----------------------------------------------------------------------- */
static void set_static_vars(uint16_t vid, uint16_t pid)
{
    char tmp[16];

    snprintf(tmp, sizeof(tmp), "%04x", vid);
    set_var("ups.vendorid", tmp);
    snprintf(tmp, sizeof(tmp), "%04x", pid);
    set_var("ups.productid", tmp);

    set_var("driver.name",    "esp32-hid-ups");
    set_var("driver.version", "2.0.0");

    const ups_vid_pid_t *dev = find_device_entry(vid, pid);
    if (dev && dev->mfr)   set_var("device.mfr",   dev->mfr);
    if (dev && dev->model) set_var("device.model",  dev->model);

    if (!s_subdriver) return;

    /* Read STATIC mapping entries from the HID descriptor now. */
    char buf[64];
    int32_t val;
    for (const ups_var_map_t *e = s_subdriver->var_map; e->nut_name; e++) {
        if (!(e->flags & UPS_MAP_STATIC)) continue;
        if (e->flags & UPS_MAP_STATUS_BIT) continue;

        const hid_field_t *f;
        if (e->parent_coll) {
            f = read_field_coll_val(e->usage, e->parent_coll, &val);
            if (!f) f = read_field_val(e->usage, &val);
        } else {
            f = read_field_val(e->usage, &val);
        }
        if (!f) continue;

        if (e->lkp) {
            const char *s = lkp_lookup(e->lkp, val);
            if (s) set_var(e->nut_name, s);
        } else if (e->scale_fn) {
            e->scale_fn(buf, sizeof(buf), val, f);
            set_var(e->nut_name, buf);
        } else {
            snprintf(buf, sizeof(buf), "%"PRId32, val);
            set_var(e->nut_name, buf);
        }
    }
}

/* -----------------------------------------------------------------------
 * Table-driven poll: reads all non-STATIC mapping entries.
 * Status bits are accumulated and written to ups.status at the end.
 * ----------------------------------------------------------------------- */
static void poll_all_vars(void)
{
    if (!s_subdriver) return;

    char    buf[64];
    int32_t val;
    char    status_buf[128] = "";
    bool    got_status      = false;

    for (const ups_var_map_t *e = s_subdriver->var_map; e->nut_name; e++) {
        /* Static vars are set at connect time; skip if already populated. */
        if ((e->flags & UPS_MAP_STATIC) && is_var_set(e->nut_name)) continue;

        /* Fetch the HID field value. */
        const hid_field_t *f;
        if (e->parent_coll) {
            f = read_field_coll_val(e->usage, e->parent_coll, &val);
            if (!f) f = read_field_val(e->usage, &val);  /* automatic fallback */
        } else {
            f = read_field_val(e->usage, &val);
        }
        if (!f) continue;

        /* Status bit: accumulate token into status_buf. */
        if (e->flags & UPS_MAP_STATUS_BIT) {
            const char *token = lkp_lookup(e->lkp, val);
            if (token && *token) {
                if (*status_buf) strlcat(status_buf, " ", sizeof(status_buf));
                strlcat(status_buf, token, sizeof(status_buf));
            }
            got_status = true;
            continue;
        }

        /* Regular variable: format and store. */
        if (e->lkp) {
            const char *s = lkp_lookup(e->lkp, val);
            strlcpy(buf, s ? s : "", sizeof(buf));
        } else if (e->scale_fn) {
            e->scale_fn(buf, sizeof(buf), val, f);
        } else {
            snprintf(buf, sizeof(buf), "%"PRId32, val);
        }

        if (*buf) set_var(e->nut_name, buf);
    }

    if (got_status)
        set_var("ups.status", *status_buf ? status_buf : "OFF");
}

/* -----------------------------------------------------------------------
 * USB HID host callbacks
 * ----------------------------------------------------------------------- */
static void hid_interface_event_cb(hid_host_device_handle_t dev,
                                   const hid_host_interface_event_t event,
                                   void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (dev == s_hid_device) {
            ESP_LOGI(TAG, "UPS disconnected");
            hid_host_device_close(dev);
            s_hid_device    = NULL;
            s_usb_connected = false;
            s_desc_parsed   = false;
            s_subdriver     = NULL;
            memset(&s_report_map, 0, sizeof(s_report_map));
            clear_vars();
            set_var("ups.status", "OFF");
        }
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        break;
    }
}

static void hid_device_event_cb(hid_host_device_handle_t dev,
                                 const hid_host_driver_event_t event,
                                 void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_info_t info;
    hid_host_get_device_info(dev, &info);
    ESP_LOGI(TAG, "HID device: VID=0x%04x PID=0x%04x", info.VID, info.PID);

    const ups_subdriver_t *sd = find_subdriver(info.VID, info.PID);
    if (!sd) {
        ESP_LOGW(TAG, "No subdriver for %04x:%04x — ignoring", info.VID, info.PID);
        return;
    }
    ESP_LOGI(TAG, "Subdriver matched: %s", sd->name);

    hid_host_device_config_t dev_cfg = {
        .callback     = hid_interface_event_cb,
        .callback_arg = NULL,
    };
    hid_host_device_open(dev, &dev_cfg);
    s_hid_device = dev;

    size_t   desc_len = 0;
    uint8_t *desc_ptr = hid_host_get_report_descriptor(dev, &desc_len);
    if (!desc_ptr || desc_len == 0) {
        ESP_LOGE(TAG, "Failed to get HID report descriptor");
        s_hid_device = NULL;
        hid_host_device_close(dev);
        return;
    }

    hid_parse_report_descriptor(desc_ptr, desc_len, &s_report_map);
#if CONFIG_APC_HID_DEBUG
    hid_dump_report_map(&s_report_map);
#endif

    s_subdriver     = sd;
    s_desc_parsed   = true;
    s_usb_connected = true;

    set_static_vars(info.VID, info.PID);
    hid_host_device_start(dev);
    ESP_LOGI(TAG, "UPS ready — %s %s",
             ups_driver_get_var("device.mfr")   ? ups_driver_get_var("device.mfr")   : "?",
             ups_driver_get_var("device.model") ? ups_driver_get_var("device.model") : "?");
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
esp_err_t ups_driver_init(void)
{
    s_vars_mutex = xSemaphoreCreateMutex();
    if (!s_vars_mutex) return ESP_ERR_NO_MEM;

    memset(s_vars, 0, sizeof(s_vars));
    set_var("ups.status", "OFF");

    hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = hid_device_event_cb,
        .callback_arg           = NULL,
    };

    esp_err_t ret = hid_host_install(&hid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Count registered subdrivers for the log line. */
    int n = 0;
    while (s_subdriver_list[n]) n++;
    ESP_LOGI(TAG, "UPS driver ready (%d subdriver(s) registered)", n);
    return ESP_OK;
}

void ups_driver_poll_task(void *arg)
{
    esp_task_wdt_add(NULL);
    while (1) {
        uint32_t remaining_ms = CONFIG_APC_UPS_POLL_INTERVAL_MS;
        while (remaining_ms > 0) {
            uint32_t chunk = remaining_ms > 5000 ? 5000 : remaining_ms;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            esp_task_wdt_reset();
            remaining_ms -= chunk;
        }
        if (s_usb_connected && s_desc_parsed)
            poll_all_vars();
    }
}

void ups_driver_get_data(ups_data_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *v;

#define GETF(field, key) do { \
    v = ups_driver_get_var(key); if (v) out->field = strtof(v, NULL); } while(0)
#define GETS(field, key) do { \
    v = ups_driver_get_var(key); if (v) strlcpy(out->field, v, sizeof(out->field)); } while(0)
#define GETI(field, key) do { \
    v = ups_driver_get_var(key); if (v) out->field = (int32_t)strtol(v, NULL, 10); } while(0)

    GETS(status,                 "ups.status");
    GETS(mfr,                    "device.mfr");
    GETS(model,                  "device.model");
    GETS(firmware,               "ups.firmware");
    GETS(serial,                 "device.serial");
    GETS(battery_type,           "battery.type");
    GETS(battery_mfr_date,       "battery.mfr.date");
    GETS(ups_beeper_status,      "ups.beeper.status");
    GETS(ups_test_result,        "ups.test.result");
    GETF(battery_charge,         "battery.charge");
    GETF(battery_charge_low,     "battery.charge.low");
    GETF(battery_charge_warning, "battery.charge.warning");
    GETF(battery_runtime,        "battery.runtime");
    GETF(battery_voltage,        "battery.voltage");
    GETF(battery_voltage_nominal,"battery.voltage.nominal");
    GETF(battery_temperature,    "battery.temperature");
    GETI(battery_cycle_count,    "battery.cycle.count");
    GETF(input_voltage,          "input.voltage");
    GETF(input_voltage_nominal,  "input.voltage.nominal");
    GETF(input_frequency,        "input.frequency");
    GETF(input_transfer_low,     "input.transfer.low");
    GETF(input_transfer_high,    "input.transfer.high");
    GETF(output_voltage,         "output.voltage");
    GETF(output_current,         "output.current");
    GETF(output_frequency,       "output.frequency");
    GETF(ups_load,               "ups.load");
    GETF(ups_temperature,        "ups.temperature");
    GETF(ups_realpower_nominal,  "ups.realpower.nominal");
    GETI(ups_delay_start,        "ups.delay.start");
    GETI(ups_delay_shutdown,     "ups.delay.shutdown");

#undef GETF
#undef GETS
#undef GETI
}
