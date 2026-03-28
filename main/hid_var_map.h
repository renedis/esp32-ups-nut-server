/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include "ups_subdriver.h"

/* -----------------------------------------------------------------------
 * Standard HID Power Device (page 0x84) + Battery System (page 0x85)
 * variable map.  Covers all variables defined by the USB HID Power Device
 * Class specification without any vendor extensions.
 *
 * Reference: https://github.com/networkupstools/nut/tree/master/drivers
 *
 * Any subdriver whose device implements standard HID UPS can point its
 * ups_subdriver_t.var_map here instead of defining its own identical table.
 * The engine silently skips entries whose HID field is absent on the
 * connected device, so unused entries are harmless.
 * ----------------------------------------------------------------------- */
extern const ups_var_map_t hid_var_map_standard[];
