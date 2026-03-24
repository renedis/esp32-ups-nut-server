#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Legrand (0x1cb0) — Keor series
 * Reference: NUT drivers/legrand-hid.c
 *
 * NUT disables the interrupt pipe for these devices; on the ESP32 we
 * use feature reports only, so no special handling is needed.
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t legrand_devices[] = {
    { 0x1cb0, 0x0038, "Legrand", "Keor PDU"              },   /* 800 VA */
    { 0x1cb0, 0x0032, "Legrand", "Keor SP"               },   /* 600-2000 VA */
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t legrand_subdriver = {
    .name    = "legrand",
    .devices = legrand_devices,
    .var_map = hid_var_map_standard,
};
