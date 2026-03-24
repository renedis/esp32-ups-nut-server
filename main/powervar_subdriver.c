#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Powervar (0x4234)
 * Reference: NUT drivers/powervar-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t powervar_devices[] = {
    { 0x4234, 0x0002, "Powervar", "UPS" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t powervar_subdriver = {
    .name    = "powervar",
    .devices = powervar_devices,
    .var_map = hid_var_map_standard,
};
