#include "ups_subdriver.h"
#include "hid_var_map.h"

/* -----------------------------------------------------------------------
 * Arduino Leonardo running NUT USB HID UPS firmware
 * Reference: NUT drivers/arduino-hid.c
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t arduino_devices[] = {
    { 0x2341, 0x0036, "Arduino", "Leonardo UPS" },
    { 0x2341, 0x8036, "Arduino", "Leonardo UPS" },
    { 0x2a03, 0x0036, "Arduino", "UPS"          },
    { 0x2a03, 0x8036, "Arduino", "UPS"          },
    { 0x2a03, 0x0040, "Arduino", "UPS"          },
    { 0x2a03, 0x8040, "Arduino", "UPS"          },
    { 0, 0, NULL, NULL }
};

const ups_subdriver_t arduino_subdriver = {
    .name    = "arduino",
    .devices = arduino_devices,
    .var_map = hid_var_map_standard,
};
