#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hid_parser.h"

#define APC_VID          0x051d
#define APC_PID_BACKUPS  0x0002

#define NUT_VAR_COUNT    48

typedef struct {
    char name[32];
    char value[64];
    bool valid;
} nut_var_t;

typedef struct {
    nut_var_t    vars[NUT_VAR_COUNT];
    bool         usb_connected;
    bool         desc_parsed;
    hid_report_map_t report_map;
    uint8_t      dev_addr;
    uint8_t      iface_num;
} apc_ups_state_t;

typedef struct {
    /* identity */
    char   status[32];
    char   mfr[32];
    char   model[64];
    char   firmware[32];
    char   serial[32];
    /* battery */
    char   battery_type[32];
    char   battery_mfr_date[16];
    float  battery_charge;
    float  battery_charge_low;
    float  battery_charge_warning;
    float  battery_runtime;
    float  battery_voltage;
    float  battery_voltage_nominal;
    float  battery_temperature;
    int32_t battery_cycle_count;
    /* input */
    float  input_voltage;
    float  input_voltage_nominal;
    float  input_frequency;
    float  input_transfer_low;
    float  input_transfer_high;
    /* output */
    float  output_voltage;
    float  output_current;
    float  output_frequency;
    /* UPS */
    float  ups_load;
    float  ups_temperature;
    float  ups_realpower_nominal;
    int32_t ups_delay_start;
    int32_t ups_delay_shutdown;
    char   ups_beeper_status[16];
    char   ups_test_result[32];
} ups_data_t;

extern apc_ups_state_t g_ups;

esp_err_t   apc_ups_init(void);
void        apc_ups_poll_task(void *arg);
void        apc_ups_get_data(ups_data_t *out);

const char *apc_ups_get_var(const char *name);
void        apc_ups_list_vars(char *buf, size_t buf_len);
