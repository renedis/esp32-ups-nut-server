/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * EcoFlow portable power stations (0x3746)
 * Reference: NUT drivers/ecoflow-hid.c
 *
 * PID 0xffff is a vendor wildcard — matches any EcoFlow device.
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t ecoflow_devices[] = {
    { 0x3746, 0xffff, "EcoFlow", "Power Station" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t ecoflow_subdriver = {
    .name    = "ecoflow",
    .devices = ecoflow_devices,
    .var_map = hid_var_map_standard,
};
