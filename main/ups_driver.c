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
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "driver/temperature_sensor.h"
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

/* Queued USB connect event: posted from HID callback, consumed by poll task */
typedef struct {
    hid_host_device_handle_t dev;
    const ups_subdriver_t   *sd;
    uint16_t                 vid;
    uint16_t                 pid;
    char                     firmware[64];
    char                     serial[32];
} connect_event_t;

static nut_var_t             s_vars[UPS_VAR_COUNT];
static SemaphoreHandle_t     s_vars_mutex;
static QueueHandle_t         s_connect_queue   = NULL;
static hid_host_device_handle_t s_hid_device   = NULL;
static hid_report_map_t      s_report_map;
static bool                  s_usb_connected   = false;
static bool                  s_desc_parsed     = false;
static const ups_subdriver_t *s_subdriver       = NULL;
static uint16_t              s_last_vid         = 0;
static uint16_t              s_last_pid         = 0;
static bool                  s_usb_ever_seen    = false;
static temperature_sensor_handle_t s_temp_sensor = NULL;

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
void ups_driver_set_var(const char *name, const char *value)
{
    set_var(name, value);
}

const char *ups_driver_get_var(const char *name)
{
    /* Check overrides first */
    const char *ovr = nvs_override_get(name);
    if (ovr) return ovr;

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
    const char *uname = nvs_config_get()->ups_name;
    size_t off = 0;
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT && off < buf_len - 1; i++) {
        if (!s_vars[i].valid) continue;
        int n = snprintf(buf + off, buf_len - off,
                         "VAR %s %s \"%s\"\n",
                         uname, s_vars[i].name, s_vars[i].value);
        if (n > 0) off += (size_t)n;
    }
    xSemaphoreGive(s_vars_mutex);
}

void ups_driver_list_vars_cb(void (*cb)(const char *name, const char *value, void *ctx), void *ctx)
{
    xSemaphoreTake(s_vars_mutex, portMAX_DELAY);
    for (int i = 0; i < UPS_VAR_COUNT; i++) {
        if (!s_vars[i].valid) continue;
        const char *ovr = nvs_override_get(s_vars[i].name);
        cb(s_vars[i].name, ovr ? ovr : s_vars[i].value, ctx);
    }
    xSemaphoreGive(s_vars_mutex);
}

bool        ups_driver_is_connected(void)  { return s_usb_connected; }
void        ups_driver_get_last_usb(uint16_t *vid, uint16_t *pid, bool *ever_seen)
{
    *vid        = s_last_vid;
    *pid        = s_last_pid;
    *ever_seen  = s_usb_ever_seen;
}
const hid_report_map_t *ups_driver_get_report_map(void) { return &s_report_map; }

const char *ups_driver_get_ups_name(void)
{
    const char *m = ups_driver_get_var("device.model");
    return m ? m : "UPS";
}

/* -----------------------------------------------------------------------
 * Standard scale functions (shared with subdrivers via ups_driver.h)
 *
 * All receive `physical` = output of hid_scale_value() — a double already
 * in SI units.  hid_scale_value() has applied:
 *   1. Logical→Physical linear scaling
 *   2. Unit exponent adjustment (NUT canonical exponents)
 * So voltage arrives in volts, time in seconds, etc.
 * ----------------------------------------------------------------------- */
void ups_scale_voltage(char *buf, size_t len, double physical, const hid_field_t *f)
{
    (void)f;
    snprintf(buf, len, "%.1f", physical);
}

void ups_scale_current(char *buf, size_t len, double physical, const hid_field_t *f)
{
    (void)f;
    snprintf(buf, len, "%.2f", physical);
}

void ups_scale_temperature(char *buf, size_t len, double physical, const hid_field_t *f)
{
    (void)f;
    /*
     * NUT Kelvin detection: if the physical value is in the range 273-373 K
     * (0-100°C), assume the device is reporting in Kelvin and subtract 273.15.
     * Some buggy devices report in Celsius directly — those values would be
     * outside the 273-373 window and are used as-is.
     */
    if (physical >= 273.0 && physical <= 373.0)
        snprintf(buf, len, "%.1f", physical - 273.15);
    else
        snprintf(buf, len, "%.1f", physical);
}

void ups_scale_frequency(char *buf, size_t len, double physical, const hid_field_t *f)
{
    (void)f;
    snprintf(buf, len, "%.1f", physical);
}

void ups_scale_mfr_date(char *buf, size_t len, double physical, const hid_field_t *f)
{
    (void)f;
    /*
     * APC date encoding: hex-as-decimal BCD packed as
     *   bits[7:0]   = year (BCD, 2 digits)  — add 1900 if >=70, else 2000
     *   bits[15:8]  = day  (BCD, 2 digits)
     *   bits[23:16] = month (BCD, 2 digits)
     * Example: 0x102202 = month 10, day 22, year 02 → 2002/10/22
     *
     * Falls back to standard DOS/FAT packed date if the APC format gives
     * an obviously invalid month (0 or >12).
     */
    int32_t val = (int32_t)physical;
    if (val == 0) { snprintf(buf, len, "not set"); return; }

    int year  = ((val & 0x0F) + 10 * ((val >> 4) & 0x0F));
    int month = (((val >> 16) & 0x0F) + 10 * ((val >> 20) & 0x0F));
    int day   = (((val >> 8) & 0x0F) + 10 * ((val >> 12) & 0x0F));

    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        /* APC BCD format valid */
        year += (year >= 70) ? 1900 : 2000;
        snprintf(buf, len, "%04d/%02d/%02d", year, month, day);
    } else {
        /* Fall back: DOS/FAT packed date bits[15:9]=year-1980, [8:5]=month, [4:0]=day */
        snprintf(buf, len, "%04d/%02d/%02d",
                 (int)(((val >> 9) & 0x7F) + 1980),
                 (int)((val >> 5) & 0x0F),
                 (int)(val        & 0x1F));
    }
}

/* -----------------------------------------------------------------------
 * HID report I/O — with per-poll-cycle report cache
 *
 * The APC BE850G2 has 168 HID fields across ~25 unique report IDs.
 * Without caching, poll_all_vars() would issue 168 GET_REPORT control
 * transfers × ~50 ms each ≈ 8+ s — exceeding the task watchdog timeout.
 *
 * With caching: each unique report ID is fetched exactly once per poll
 * cycle.  Subsequent fields in the same report are extracted from the
 * cached 65-byte buffer.  The WDT is reset on every cache miss (i.e.,
 * every real USB transfer) so the watchdog never fires mid-poll.
 * ----------------------------------------------------------------------- */
#define REPORT_CACHE_MAX 64     /* max unique report IDs per poll cycle   */
#define REPORT_BUF_SIZE  65     /* 1 report-ID byte + 64 data bytes       */

typedef struct {
    uint8_t  id;
    uint8_t  data[REPORT_BUF_SIZE];
    bool     valid;
} report_cache_entry_t;

static report_cache_entry_t s_report_cache[REPORT_CACHE_MAX];
static uint8_t              s_report_cache_count = 0;

static void report_cache_clear(void)
{
    s_report_cache_count = 0;
    for (int i = 0; i < REPORT_CACHE_MAX; i++) s_report_cache[i].valid = false;
}

static esp_err_t get_feature_report(uint8_t report_id,
                                    uint8_t *buf, size_t buf_len)
{
    /* buf_len must cover the full report so we don't trip the DWC assert */
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

/* Returns a pointer to a 65-byte buffer for report_id.
 * On the first call for a given ID in a poll cycle the report is fetched
 * from the device (USB transfer + WDT reset).  Subsequent calls return the
 * cached copy.  Returns NULL on USB failure. */
static const uint8_t *get_cached_report(uint8_t report_id)
{
    /* Lookup in cache */
    for (int i = 0; i < s_report_cache_count; i++) {
        if (s_report_cache[i].id == report_id)
            return s_report_cache[i].data;
    }

    /* Cache miss — fetch from device */
    if (s_report_cache_count >= REPORT_CACHE_MAX) {
        ESP_LOGW(TAG, "report cache full, fetching rid=0x%02x uncached", report_id);
        /* Return temporary buffer (won't be cached) */
        static uint8_t tmp[REPORT_BUF_SIZE];
        return (get_feature_report(report_id, tmp, sizeof(tmp)) == ESP_OK) ? tmp : NULL;
    }

    esp_task_wdt_reset();   /* keep watchdog happy while doing USB I/O */

    report_cache_entry_t *entry = &s_report_cache[s_report_cache_count];
    if (get_feature_report(report_id, entry->data, REPORT_BUF_SIZE) != ESP_OK)
        return NULL;

    entry->id    = report_id;
    entry->valid = true;
    s_report_cache_count++;
    return entry->data;
}

static const hid_field_t *fetch_field(const hid_field_t *f, double *out)
{
    /*
     * Always request the full buffer (65 bytes = 1 report-ID byte + 64 data).
     *
     * Root cause of the _buffer_parse_ctrl assertion crash:
     * The APC BE850G2 (and many HID UPS devices) ignores wLength in
     * GET_REPORT and sends a full USB packet — at least EP0 MPS (8 bytes for
     * full-speed).  When the DWC descriptor is programmed with xfer_size < 8,
     * receiving an 8-byte packet wraps the 17-bit counter:
     *   xfer_size = 2 - 8 = -6  →  131066 in 17-bit unsigned
     * _buffer_parse_ctrl then sees rem_len=131066 > num_bytes-8=2 and asserts.
     *
     * Requesting 65 bytes guarantees xfer_size (65) >= EP0 MPS (8), so the
     * counter never underflows regardless of what the device sends.
     */
    const uint8_t *report = get_cached_report(f->report_id);
    if (!report) return NULL;
    int32_t logical = hid_extract_field_value(report + 1, REPORT_BUF_SIZE - 1, f);
    *out = hid_scale_value(logical, f);
    return f;
}

static const hid_field_t *read_field_val(uint32_t usage, double *out)
{
    const hid_field_t *f = hid_find_field(&s_report_map, usage, HID_ITEM_TYPE_FEATURE);
    return f ? fetch_field(f, out) : NULL;
}

static const hid_field_t *read_field_coll_val(uint32_t usage, uint32_t coll, double *out)
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
    report_cache_clear();   /* fresh cache for static-var scan */

    char tmp[16];

    snprintf(tmp, sizeof(tmp), "%04x", vid);
    set_var("ups.vendorid", tmp);
    snprintf(tmp, sizeof(tmp), "%04x", pid);
    set_var("ups.productid", tmp);

    set_var("driver.name",    "esp32-hid-ups");
    set_var("driver.version", "2.0.0");
    set_var("device.type",    "ups");

    const ups_vid_pid_t *dev = find_device_entry(vid, pid);
    if (dev && dev->mfr)   set_var("device.mfr",   dev->mfr);
    if (dev && dev->model) set_var("device.model",  dev->model);

    if (!s_subdriver) return;

    /* APC Back-UPS uses sealed lead-acid (SLA/PbAc) — the iChemistry HID
     * field is a string descriptor index we can't safely fetch at runtime. */
    if (strcmp(s_subdriver->name, "apc") == 0)
        set_var("battery.type", "PbAc");

    /* Read STATIC mapping entries from the HID descriptor now. */
    char buf[64];
    double val;
    for (const ups_var_map_t *e = s_subdriver->var_map; e->nut_name; e++) {
        if (!(e->flags & UPS_MAP_STATIC)) continue;
        if (e->flags & UPS_MAP_STATUS_BIT) continue;

        const hid_field_t *f;
        if (e->parent_coll) {
            f = read_field_coll_val(e->usage, e->parent_coll, &val);
        } else {
            f = read_field_val(e->usage, &val);
        }
        if (!f) continue;

        if (e->lkp) {
            const char *s = lkp_lookup(e->lkp, (int32_t)val);
            if (s) set_var(e->nut_name, s);
        } else if (e->scale_fn) {
            e->scale_fn(buf, sizeof(buf), val, f);
            set_var(e->nut_name, buf);
        } else {
            snprintf(buf, sizeof(buf), "%.4g", val);
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

    report_cache_clear();   /* one USB transfer per report ID, not per field */

    char   buf[64];
    double val;
    char   status_buf[128] = "";
    bool   got_status      = false;

    for (const ups_var_map_t *e = s_subdriver->var_map; e->nut_name; e++) {
        /* Static vars are set at connect time; skip if already populated. */
        if ((e->flags & UPS_MAP_STATIC) && is_var_set(e->nut_name)) continue;

        /* Fetch the HID field value (already scaled via hid_scale_value). */
        const hid_field_t *f;
        if (e->parent_coll) {
            f = read_field_coll_val(e->usage, e->parent_coll, &val);
        } else {
            f = read_field_val(e->usage, &val);
        }
        if (!f) continue;

        /* Status bit: accumulate token into status_buf. */
        if (e->flags & UPS_MAP_STATUS_BIT) {
            const char *token = lkp_lookup(e->lkp, (int32_t)val);
            if (token && *token) {
                if (*status_buf) strlcat(status_buf, " ", sizeof(status_buf));
                strlcat(status_buf, token, sizeof(status_buf));
            }
            got_status = true;
            continue;
        }

        /* Regular variable: format and store. */
        if (e->lkp) {
            const char *s = lkp_lookup(e->lkp, (int32_t)val);
            strlcpy(buf, s ? s : "", sizeof(buf));
        } else if (e->scale_fn) {
            e->scale_fn(buf, sizeof(buf), val, f);
        } else {
            /* Auto-format: use %.4g for compact representation (no trailing zeros) */
            snprintf(buf, sizeof(buf), "%.4g", val);
        }

        if (*buf) set_var(e->nut_name, buf);
    }

    if (got_status)
        set_var("ups.status", *status_buf ? status_buf : "OFF");

    /* Derive ups.realpower = ups.load% × ups.realpower.nominal / 100 */
    {
        const char *load_s    = ups_driver_get_var("ups.load");
        const char *nominal_s = ups_driver_get_var("ups.realpower.nominal");
        if (load_s && nominal_s) {
            float load_pct = strtof(load_s, NULL);
            float nominal  = strtof(nominal_s, NULL);
            char  pbuf[16];
            snprintf(pbuf, sizeof(pbuf), "%.0f", load_pct * nominal / 100.0f);
            set_var("ups.realpower", pbuf);
        }
    }

    /* Fallback: if UPS doesn't report temperatures, use ESP32 internal sensor */
    if (s_temp_sensor &&
        (!is_var_set("battery.temperature") || !is_var_set("ups.temperature"))) {
        float tsens;
        if (temperature_sensor_get_celsius(s_temp_sensor, &tsens) == ESP_OK) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.1f", tsens);
            if (!is_var_set("battery.temperature"))
                set_var("battery.temperature", tbuf);
            if (!is_var_set("ups.temperature"))
                set_var("ups.temperature", tbuf);
        }
    }
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
    s_last_vid      = info.VID;
    s_last_pid      = info.PID;
    s_usb_ever_seen = true;
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

    /*
     * Do NOT call hid_host_get_report_descriptor here — it issues a
     * synchronous control transfer that blocks on ctrl_xfer_done semaphore.
     * The completion callback fires from usb_host_client_handle_events,
     * which is the SAME task calling this callback → deadlock / 5 s timeout.
     *
     * Instead, post to s_connect_queue and let ups_driver_poll_task (a
     * separate task) fetch the descriptor safely.
     */
    connect_event_t evt = { .dev = dev, .sd = sd, .vid = info.VID, .pid = info.PID };

    /* Convert wchar_t string descriptors to plain ASCII (low byte of each wchar)
     * and strip trailing whitespace (APC pads with spaces). */
    for (int i = 0; i < (int)(sizeof(evt.firmware) - 1); i++) {
        if (!info.iProduct[i]) break;
        evt.firmware[i] = (char)(info.iProduct[i] & 0xFF);
    }
    for (int i = strlen(evt.firmware) - 1; i >= 0 && evt.firmware[i] == ' '; i--)
        evt.firmware[i] = '\0';
    for (int i = 0; i < (int)(sizeof(evt.serial) - 1); i++) {
        if (!info.iSerialNumber[i]) break;
        evt.serial[i] = (char)(info.iSerialNumber[i] & 0xFF);
    }
    for (int i = strlen(evt.serial) - 1; i >= 0 && evt.serial[i] == ' '; i--)
        evt.serial[i] = '\0';
    if (xQueueSend(s_connect_queue, &evt, 0) != pdTRUE)
        ESP_LOGE(TAG, "connect_queue full — device ignored");
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Process a queued USB connect event (called from poll task, NOT from HID cb)
 * Safe to call hid_host_get_report_descriptor here because this runs in a
 * different task than the HID background event_handler_task.
 * ----------------------------------------------------------------------- */
static void handle_connect_event(const connect_event_t *evt)
{
    size_t   desc_len = 0;
    uint8_t *desc_ptr = hid_host_get_report_descriptor(evt->dev, &desc_len);
    if (!desc_ptr || desc_len == 0) {
        ESP_LOGE(TAG, "Failed to get HID report descriptor");
        hid_host_device_close(evt->dev);
        return;
    }

    hid_parse_report_descriptor(desc_ptr, desc_len, &s_report_map);
#if CONFIG_APC_HID_DEBUG
    hid_dump_report_map(&s_report_map);
#endif

    s_hid_device    = evt->dev;
    s_subdriver     = evt->sd;
    s_desc_parsed   = true;
    s_usb_connected = true;

    set_static_vars(evt->vid, evt->pid);

    /* String descriptors captured in hid_device_event_cb (safe HID task context). */
    if (evt->firmware[0]) set_var("ups.firmware", evt->firmware);
    if (evt->serial[0])   set_var("device.serial", evt->serial);

    /*
     * Do NOT call hid_host_device_start() here.
     * We only use FEATURE reports via EP0 control transfers (hid_class_request_get_report).
     * Starting the interrupt IN endpoint causes the APC BE850G2 to send INPUT reports
     * concurrently with our EP0 control transfers, crashing the DWC controller assertion
     * in _buffer_parse_ctrl (rem_len vs num_bytes mismatch) on the first GET_REPORT.
     */
    ESP_LOGI(TAG, "UPS ready — %s %s",
             ups_driver_get_var("device.mfr")   ? ups_driver_get_var("device.mfr")   : "?",
             ups_driver_get_var("device.model") ? ups_driver_get_var("device.model") : "?");
}

esp_err_t ups_driver_init(void)
{
    s_vars_mutex = xSemaphoreCreateMutex();
    if (!s_vars_mutex) return ESP_ERR_NO_MEM;

    s_connect_queue = xQueueCreate(4, sizeof(connect_event_t));
    if (!s_connect_queue) return ESP_ERR_NO_MEM;

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

    /* Internal temperature sensor — used as fallback when UPS has no battery temp */
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tsens_cfg, &s_temp_sensor) == ESP_OK)
        temperature_sensor_enable(s_temp_sensor);
    else
        s_temp_sensor = NULL;

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
            /* Block on the connect queue for up to 'chunk' ms.
             * This lets us react to a new USB device quickly while still
             * honoring the WDT reset cadence. */
            connect_event_t evt;
            if (xQueueReceive(s_connect_queue, &evt, pdMS_TO_TICKS(chunk)) == pdTRUE)
                handle_connect_event(&evt);
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
    GETF(ups_realpower,          "ups.realpower");
    GETI(ups_delay_start,        "ups.delay.start");
    GETI(ups_delay_shutdown,     "ups.delay.shutdown");

#undef GETF
#undef GETS
#undef GETI
}
