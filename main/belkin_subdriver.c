#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Belkin UPS models + Liebert (10af) consumer/prosumer units.
 * Reference: NUT drivers/belkin-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t belkin_devices[] = {
    /* Belkin */
    { 0x050d, 0x0980, "Belkin", "F6C800-UNV"        },
    { 0x050d, 0x0900, "Belkin", "F6C900-UNV"        },
    { 0x050d, 0x0910, "Belkin", "F6C100-UNV"        },
    { 0x050d, 0x0912, "Belkin", "F6C120-UNV"        },
    { 0x050d, 0x0551, "Belkin", "F6C550-AVR"        },
    { 0x050d, 0x0750, "Belkin", "F6C1250-TW-RK"     },
    { 0x050d, 0x0751, "Belkin", "F6C1500-TW-RK"     },
    { 0x050d, 0x0375, "Belkin", "F6H375-USB"        },
    { 0x050d, 0x0f51, "Belkin", "Regulator PRO-USB" },
    { 0x050d, 0x1100, "Belkin", "F6C1100/1200-UNV"  },
    /* Liebert */
    { 0x10af, 0x0000, "Liebert", "GXT4"             },
    { 0x10af, 0x0001, "Liebert", "PowerSure PSA"    },
    { 0x10af, 0x0002, "Liebert", "PowerSure PST"    },
    { 0x10af, 0x0004, "Liebert", "PowerSure PSI 1440" },
    { 0x10af, 0x0008, "Liebert", "GXT3"             },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t belkin_subdriver = {
    .name    = "belkin",
    .devices = belkin_devices,
    .var_map = hid_var_map_standard,
};
