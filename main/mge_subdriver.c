/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * MGE / Eaton and OEM partners
 *
 * Many Eaton/MGE UPS units are sold under several brands (Dell, HP,
 * IBM, Powerware, AEG, Phoenixtec/Liebert) but all implement the same
 * standard HID Power Device class with the MGE mapping.
 *
 * PID 0xffff in NUT means "match any device from this vendor" —
 * the ESP32 engine treats it the same way (wildcard PID).
 *
 * References:
 *   NUT drivers/mge-hid.c
 *   NUT drivers/liebert-hid.c  (Phoenixtec 0x06da handled here)
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t mge_devices[] = {
    /* MGE / Eaton */
    { 0x0463, 0x0001, "Eaton",      "UPS"              },
    { 0x0463, 0xffff, "Eaton",      "UPS"              },
    /* Dell (OEM Eaton) */
    { 0x047c, 0xffff, "Dell",       "UPS"              },
    /* Powerware */
    { 0x0592, 0x0004, "Powerware",  "PW 9140"          },
    /* HP R/T3000 (MGE OEM) */
    { 0x03f0, 0x1fe5, "HP",         "R/T3000 G2"       },
    { 0x03f0, 0x1fe6, "HP",         "R/T3000 G2"       },
    { 0x03f0, 0x1fe7, "HP",         "R/T3000 G3"       },
    { 0x03f0, 0x1fe8, "HP",         "R/T3000 G3"       },
    /* AEG (Eaton brand in DE) */
    { 0x2b2d, 0xffff, "AEG",        "UPS"              },
    /* Phoenixtec / Liebert (GXT2, GXT3, Nfinity …) */
    { 0x06da, 0xffff, "Liebert",    "UPS"              },
    /* IBM (Eaton OEM) */
    { 0x04b3, 0x0001, "IBM",        "UPS"              },
    /* KSTAR */
    { 0x09d6, 0x0001, "KSTAR",      "UPS"              },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t mge_subdriver = {
    .name    = "mge",
    .devices = mge_devices,
    .var_map = hid_var_map_standard,
};
