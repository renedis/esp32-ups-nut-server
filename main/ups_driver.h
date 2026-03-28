/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hid_parser.h"
#include "ups_subdriver.h"

/* Maximum number of NUT variables held in the variable store. */
#define UPS_VAR_COUNT 64

/* -----------------------------------------------------------------------
 * Snapshot struct — filled by ups_driver_get_data().
 * Used by web_server and mqtt_pub for JSON / MQTT publishing.
 * ----------------------------------------------------------------------- */
typedef struct {
    /* identity */
    char    status[32];
    char    mfr[32];
    char    model[64];
    char    firmware[32];
    char    serial[32];
    /* battery */
    char    battery_type[32];
    char    battery_mfr_date[16];
    float   battery_charge;
    float   battery_charge_low;
    float   battery_charge_warning;
    float   battery_runtime;
    float   battery_voltage;
    float   battery_voltage_nominal;
    float   battery_temperature;
    int32_t battery_cycle_count;
    /* input */
    float   input_voltage;
    float   input_voltage_nominal;
    float   input_frequency;
    float   input_transfer_low;
    float   input_transfer_high;
    /* output */
    float   output_voltage;
    float   output_current;
    float   output_frequency;
    /* UPS */
    float   ups_load;
    float   ups_temperature;
    float   ups_realpower_nominal;
    float   ups_realpower;
    int32_t ups_delay_start;
    int32_t ups_delay_shutdown;
    char    ups_beeper_status[16];
    char    ups_test_result[32];
} ups_data_t;

/* -----------------------------------------------------------------------
 * Standard scale functions.
 * Declared here so subdriver mapping tables (in *_subdriver.c) can
 * reference them without duplicating the implementations.
 * ----------------------------------------------------------------------- */
/* physical = output of hid_scale_value() — SI units, properly scaled */
void ups_scale_voltage    (char *buf, size_t len, double physical, const hid_field_t *f);
void ups_scale_current    (char *buf, size_t len, double physical, const hid_field_t *f);
void ups_scale_temperature(char *buf, size_t len, double physical, const hid_field_t *f);
void ups_scale_frequency  (char *buf, size_t len, double physical, const hid_field_t *f);
void ups_scale_mfr_date   (char *buf, size_t len, double physical, const hid_field_t *f);

/* -----------------------------------------------------------------------
 * Driver public API
 * ----------------------------------------------------------------------- */

/* Install the HID host driver and set initial state.  Call once. */
esp_err_t   ups_driver_init(void);

/* FreeRTOS task: poll all HID variables periodically. */
void        ups_driver_poll_task(void *arg);

/* Thread-safe variable store accessors. */
const char *ups_driver_get_var(const char *name);
void        ups_driver_set_var(const char *name, const char *value);
void        ups_driver_list_vars(char *buf, size_t buf_len);
void        ups_driver_list_vars_cb(void (*cb)(const char *name, const char *value, void *ctx), void *ctx);

/* Populate a ups_data_t snapshot (used by web/MQTT). */
void        ups_driver_get_data(ups_data_t *out);

/* Convenience queries. */
bool        ups_driver_is_connected(void);
const char *ups_driver_get_ups_name(void);  /* Returns model string for NUT LIST UPS */
void        ups_driver_get_last_usb(uint16_t *vid, uint16_t *pid, bool *ever_seen);
const hid_report_map_t *ups_driver_get_report_map(void);
