/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Delta Electronics RT / Amplon R Series
 * Reference: NUT drivers/delta_ups-hid.c
 *
 * Note: PIDs 0xa011 and 0xa0a0 (Minuteman OEM) are handled by
 * tripplite_subdriver since NUT places them in tripplite-hid.c.
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t delta_devices[] = {
    { 0x05dd, 0x041b, "Delta", "RT / Amplon R Series 1-3 kVA" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t delta_subdriver = {
    .name    = "delta",
    .devices = delta_devices,
    .var_map = hid_var_map_standard,
};
