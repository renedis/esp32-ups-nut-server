#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hid_parser.h"

#define APC_VID          0x051d
#define APC_PID_BACKUPS  0x0002

#define NUT_VAR_COUNT    20

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
    char   status[32];
    char   mfr[32];
    char   model[64];
    char   firmware[32];
    char   serial[32];
    char   test_result[32];
    float  battery_charge;
    float  battery_runtime;
    float  battery_voltage;
    float  battery_charge_low;
    float  battery_charge_warning;
    float  input_voltage;
    float  ups_load;
    float  ups_temperature;
    float  ups_realpower_nominal;
} ups_data_t;

extern apc_ups_state_t g_ups;

esp_err_t   apc_ups_init(void);
void        apc_ups_poll_task(void *arg);
void        apc_ups_get_data(ups_data_t *out);

const char *apc_ups_get_var(const char *name);
void        apc_ups_list_vars(char *buf, size_t buf_len);
