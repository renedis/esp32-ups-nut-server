/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * PowerCOM (0x0d9f)
 * Reference: NUT drivers/powercom-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t powercom_devices[] = {
    { 0x0d9f, 0x00a2, "PowerCOM", "IMP / IMPERIAL Series"   },
    { 0x0d9f, 0x00a3, "PowerCOM", "SKP Smart KING Pro"      },
    { 0x0d9f, 0x00a4, "PowerCOM", "WOW"                     },
    { 0x0d9f, 0x00a5, "PowerCOM", "VGD Vanguard"            },
    { 0x0d9f, 0x00a6, "PowerCOM", "BNT Black Knight Pro"    },
    { 0x0d9f, 0x0004, "PowerCOM", "Vanguard / BNT-xxxAP"    },
    { 0x0d9f, 0x0001, "PowerCOM", "UPS"                     },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t powercom_subdriver = {
    .name    = "powercom",
    .devices = powercom_devices,
    .var_map = hid_var_map_standard,
};
