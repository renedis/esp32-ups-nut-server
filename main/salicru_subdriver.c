/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Salicru (0x2e66)
 * Reference: NUT drivers/salicru-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t salicru_devices[] = {
    { 0x2e66, 0x0101, "Salicru", "SPS 3000 ADV RT2"    },
    { 0x2e66, 0x0201, "Salicru", "TWINPRO3 / TWINRT3"  },
    { 0x2e66, 0x0202, "Salicru", "UPS"                 },
    { 0x2e66, 0x0203, "Salicru", "UPS"                 },
    { 0x2e66, 0x0300, "Salicru", "SPS 850 HOME"        },
    { 0x2e66, 0x0302, "Salicru", "SPS 850 ADV T"       },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t salicru_subdriver = {
    .name    = "salicru",
    .devices = salicru_devices,
    .var_map = hid_var_map_standard,
};
