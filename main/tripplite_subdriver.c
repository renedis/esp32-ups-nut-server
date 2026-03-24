#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Tripp Lite + HP T-series (OEM) + Delta/Minuteman (OEM)
 * Reference: NUT drivers/tripplite-hid.c
 *
 * NUT uses battery-voltage scale factors per PID (0.1 V or 1.0 V).
 * The ESP32 engine auto-detects scaling via logical_max heuristics in
 * ups_scale_voltage(), so no per-PID scale callback is needed.
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t tripplite_devices[] = {
    /* Tripp Lite — 0x1xxx / 0x2xxx: AVR, OMNI, Smart series */
    { 0x09ae, 0x1003, "Tripp Lite", "UPS" },
    { 0x09ae, 0x1007, "Tripp Lite", "UPS" },
    { 0x09ae, 0x1008, "Tripp Lite", "UPS" },
    { 0x09ae, 0x1009, "Tripp Lite", "UPS" },
    { 0x09ae, 0x1010, "Tripp Lite", "UPS" },
    { 0x09ae, 0x1330, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2005, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2007, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2008, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2009, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2010, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2011, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2012, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2013, "Tripp Lite", "UPS" },
    { 0x09ae, 0x2014, "Tripp Lite", "UPS" },
    /* Tripp Lite — 0x3xxx: SmartOnline / Smart1500LCD */
    { 0x09ae, 0x3008, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3009, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3010, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3011, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3012, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3013, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3014, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3015, "Tripp Lite", "SmartOnline" },
    { 0x09ae, 0x3016, "Tripp Lite", "Smart1500LCDT" },
    { 0x09ae, 0x3024, "Tripp Lite", "Smart1500LCDT" },
    /* Tripp Lite — 0x4xxx: ECO / SU series */
    { 0x09ae, 0x4001, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4002, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4003, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4004, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4005, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4006, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4007, "Tripp Lite", "UPS" },
    { 0x09ae, 0x4008, "Tripp Lite", "UPS" },
    /* HP T-series (Tripp Lite OEM) */
    { 0x03f0, 0x0001, "HP",         "T1500 G3 UPS"   },
    { 0x03f0, 0x1fe0, "HP",         "T1500 G3 UPS"   },
    { 0x03f0, 0x1fe1, "HP",         "T1500 G3 UPS"   },
    { 0x03f0, 0x1fe2, "HP",         "T2200 G3 UPS"   },
    { 0x03f0, 0x1fe3, "HP",         "T2200 G3 UPS"   },
    { 0x03f0, 0x1f06, "HP",         "UPS"            },
    { 0x03f0, 0x1f08, "HP",         "UPS"            },
    { 0x03f0, 0x1f09, "HP",         "UPS"            },
    { 0x03f0, 0x1f0a, "HP",         "UPS"            },
    /* Delta / Minuteman (Tripp Lite OEM) */
    { 0x05dd, 0xa011, "Minuteman",  "UPS"            },
    { 0x05dd, 0xa0a0, "Minuteman",  "UPS"            },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t tripplite_subdriver = {
    .name    = "tripplite",
    .devices = tripplite_devices,
    .var_map = hid_var_map_standard,
};
