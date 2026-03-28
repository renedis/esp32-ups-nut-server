/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * iDowell / Goldenmate (0x075d)
 * Reference: NUT drivers/idowell-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t idowell_devices[] = {
    { 0x075d, 0x0300, "iDowell", "UPS" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t idowell_subdriver = {
    .name    = "idowell",
    .devices = idowell_devices,
    .var_map = hid_var_map_standard,
};
