#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Minibox openUPS — open-source intelligent UPS (0x04d8)
 * Reference: NUT drivers/openups-hid.c
 *
 * NUT applies a per-firmware voltage multiplier (read from a string
 * descriptor).  The ESP32 engine auto-detects voltage scaling via
 * logical_max heuristics in ups_scale_voltage(), which covers the
 * common firmware versions without firmware interrogation.
 *
 * Requires openUPS firmware 1.4 or newer.
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t openups_devices[] = {
    { 0x04d8, 0xd004, "Minibox", "openUPS" },
    { 0x04d8, 0xd005, "Minibox", "openUPS" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t openups_subdriver = {
    .name    = "openups",
    .devices = openups_devices,
    .var_map = hid_var_map_standard,
};
