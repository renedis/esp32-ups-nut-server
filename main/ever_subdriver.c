#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Ever (2e51) + ST Microelectronics OEM (0483:a113 — Ever-branded)
 * Reference: NUT drivers/ever-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t ever_devices[] = {
    { 0x0483, 0xa113, "Ever",   "UPS"          },
    { 0x2e51, 0xffff, "Ever",   "UPS"          },
    { 0x2e51, 0x0000, "Ever",   "UPS"          },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t ever_subdriver = {
    .name    = "ever",
    .devices = ever_devices,
    .var_map = hid_var_map_standard,
};
