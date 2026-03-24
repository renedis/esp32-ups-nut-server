#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * CyberPower Systems (CPS) + Cyber Energy (ST Microelectronics OEM)
 * Reference: NUT drivers/cps-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t cps_devices[] = {
    /* CyberPower */
    { 0x0764, 0x0005, "CyberPower", "900AVR / BC900D"                    },
    { 0x0764, 0x0501, "CyberPower", "CP1200AVR / CP825AVR / CP1500C etc" },
    { 0x0764, 0x0601, "CyberPower", "OR2200LCDRM2U / OR700LCDRM1U etc"   },
    /* Cyber Energy — ST Micro OEM */
    { 0x0483, 0xa430, "CyberPower", "Cyber Energy UPS"                   },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t cps_subdriver = {
    .name    = "cps",
    .devices = cps_devices,
    .var_map = hid_var_map_standard,
};
