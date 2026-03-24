#include "apc_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * VID:PID table — APC (American Power Conversion) / Schneider Electric
 *
 * 051d:0000  AP9584 Serial→USB kit
 * 051d:0002  Back-UPS (most consumer models)
 * 051d:0003  Smart-UPS / 5G / network-capable
 * 051d:0004  Smart-UPS 1000 (interrupt pipe disabled in NUT)
 * 051d:0005  Reserved / future
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t apc_devices[] = {
    { 0x051d, 0x0000, "APC", "UPS" },
    { 0x051d, 0x0002, "APC", "Back-UPS" },
    { 0x051d, 0x0003, "APC", "Smart-UPS" },
    { 0x051d, 0x0004, "APC", "Smart-UPS 1000" },
    { 0x051d, 0x0005, "APC", "UPS" },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t apc_subdriver = {
    .name    = "apc",
    .devices = apc_devices,
    .var_map = hid_var_map_standard,
};
