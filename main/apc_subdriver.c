#include "apc_subdriver.h"
#include "ups_driver.h"
#include "hid_parser.h"

/* -----------------------------------------------------------------------
 * VID:PID table — APC (American Power Conversion)
 * 051d:0002  Back-UPS (USB HID, most common)
 * 051d:0003  Smart-UPS / network-capable
 * 051d:0004  UPS (generic variant)
 * 051d:0005  UPS (generic variant)
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t apc_devices[] = {
    { 0x051d, 0x0002, "APC", "Back-UPS" },
    { 0x051d, 0x0003, "APC", "Smart-UPS" },
    { 0x051d, 0x0004, "APC", "UPS" },
    { 0x051d, 0x0005, "APC", "UPS" },
    { 0, 0, NULL, NULL }   /* sentinel */
};

/* -----------------------------------------------------------------------
 * ups.status bit lookup tables
 *
 * Each entry maps a HID boolean value to a NUT status token.
 * Empty string "" means "contribute nothing for this state".
 * Sentinel: { anything, NULL }.
 * ----------------------------------------------------------------------- */
static const ups_lkp_t lkp_ac_present[] = {
    { 1, "OL"  },   /* On line (mains present)  */
    { 0, "OB"  },   /* On battery               */
    { 0, NULL  }
};

static const ups_lkp_t lkp_charging[] = {
    { 1, "CHRG" },
    { 0, ""     },
    { 0, NULL   }
};

static const ups_lkp_t lkp_discharging[] = {
    { 1, "DISCHRG" },
    { 0, ""        },
    { 0, NULL      }
};

static const ups_lkp_t lkp_low_battery[] = {
    { 1, "LB" },
    { 0, ""   },
    { 0, NULL }
};

static const ups_lkp_t lkp_replace_battery[] = {
    { 1, "RB" },
    { 0, ""   },
    { 0, NULL }
};

static const ups_lkp_t lkp_shutdown_imminent[] = {
    { 1, "FSD" },
    { 0, ""    },
    { 0, NULL  }
};

/* -----------------------------------------------------------------------
 * Enum lookup tables
 * ----------------------------------------------------------------------- */
static const ups_lkp_t lkp_beeper[] = {
    { 1, "enabled"  },
    { 2, "disabled" },
    { 3, "muted"    },
    { 0, NULL       }
};

static const ups_lkp_t lkp_test_result[] = {
    { 1, "Done and passed"    },
    { 2, "Done and warning"   },
    { 3, "Done and error"     },
    { 4, "Aborted"            },
    { 5, "In progress"        },
    { 6, "No test initiated"  },
    { 0, NULL                 }
};

/* -----------------------------------------------------------------------
 * Mapping table
 *
 * Column order: nut_name, usage, parent_coll, item_type, flags, lkp, scale_fn
 *
 * parent_coll = 0   → search anywhere in descriptor
 * parent_coll = HID_USAGE_PD_INPUT  → prefer field inside Input collection
 * parent_coll = HID_USAGE_PD_OUTPUT → prefer field inside Output collection
 *
 * This is what distinguishes input.voltage from output.voltage — both
 * share usage HID_USAGE_PD_VOLTAGE (0x84/0x30) but live in different
 * parent collections.
 * ----------------------------------------------------------------------- */
static const ups_var_map_t apc_var_map[] = {

    /* ------------------------------------------------------------------ */
    /* ups.status bits — each boolean field contributes a token            */
    /* ------------------------------------------------------------------ */
    { "ups.status", HID_USAGE_BS_AC_PRESENT,          0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_ac_present,        NULL },
    { "ups.status", HID_USAGE_BS_CHARGING,             0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_charging,           NULL },
    { "ups.status", HID_USAGE_BS_DISCHARGING,          0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_discharging,        NULL },
    { "ups.status", HID_USAGE_BS_BELOW_REMAINING_CAP,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_low_battery,        NULL },
    { "ups.status", HID_USAGE_BS_NEED_REPLACEMENT,     0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_replace_battery,    NULL },
    { "ups.status", HID_USAGE_PD_SHUTDOWN_IMMINENT,    0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATUS_BIT, lkp_shutdown_imminent,  NULL },

    /* ------------------------------------------------------------------ */
    /* battery                                                             */
    /* ------------------------------------------------------------------ */
    { "battery.charge",
      HID_USAGE_BS_REMAINING_CAPACITY,   0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, NULL },

    { "battery.charge.low",
      HID_USAGE_BS_REMAINING_CAP_LIMIT,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    { "battery.charge.warning",
      HID_USAGE_BS_WARNING_CAP_LIMIT,    0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    { "battery.runtime",
      HID_USAGE_BS_RUNTIME_TO_EMPTY,     0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, NULL },

    { "battery.voltage",
      HID_USAGE_BS_VOLTAGE,              0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_voltage },

    { "battery.temperature",
      HID_USAGE_BS_TEMPERATURE,          0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_temperature },

    { "battery.cycle.count",
      HID_USAGE_BS_CYCLE_COUNT,          0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, NULL },

    /* Read once at connect time — packed DOS date → "YYYY/MM/DD" */
    { "battery.mfr.date",
      HID_USAGE_BS_MANUFACTURER_DATE,    0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATIC, NULL, ups_scale_mfr_date },

    /* ------------------------------------------------------------------ */
    /* input                                                               */
    /* ------------------------------------------------------------------ */
    { "input.voltage",
      HID_USAGE_PD_VOLTAGE,     HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_voltage },

    /* Nominal input voltage: ConfigVoltage in the Input collection */
    { "input.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    { "input.frequency",
      HID_USAGE_PD_FREQUENCY,   HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_frequency },

    { "input.transfer.low",
      HID_USAGE_PD_LOW_VOLTAGE_TRANSFER,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    { "input.transfer.high",
      HID_USAGE_PD_HIGH_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* ------------------------------------------------------------------ */
    /* output                                                              */
    /* ------------------------------------------------------------------ */
    { "output.voltage",
      HID_USAGE_PD_VOLTAGE,     HID_USAGE_PD_OUTPUT, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_voltage },

    { "output.current",
      HID_USAGE_PD_CURRENT,     HID_USAGE_PD_OUTPUT, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_current },

    { "output.frequency",
      HID_USAGE_PD_FREQUENCY,   HID_USAGE_PD_OUTPUT, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_frequency },

    /* ------------------------------------------------------------------ */
    /* UPS                                                                 */
    /* ------------------------------------------------------------------ */
    { "ups.load",
      HID_USAGE_PD_PERCENT_LOAD,          0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, NULL },

    { "ups.temperature",
      HID_USAGE_PD_TEMPERATURE,           0, HID_ITEM_TYPE_FEATURE, 0,
      NULL, ups_scale_temperature },

    /* Nominal real power (W) — configuration value, re-read each cycle */
    { "ups.realpower.nominal",
      HID_USAGE_PD_CONFIG_ACTIVE_POWER,   0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    { "ups.delay.start",
      HID_USAGE_PD_DELAY_BEFORE_STARTUP,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    { "ups.delay.shutdown",
      HID_USAGE_PD_DELAY_BEFORE_SHUTDOWN, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    { "ups.beeper.status",
      HID_USAGE_PD_AUDIBLE_ALARM,         0, HID_ITEM_TYPE_FEATURE, 0,
      lkp_beeper, NULL },

    { "ups.test.result",
      HID_USAGE_PD_TEST,                  0, HID_ITEM_TYPE_FEATURE, 0,
      lkp_test_result, NULL },

    /* sentinel */
    { NULL, 0, 0, HID_ITEM_TYPE_FEATURE, 0, NULL, NULL }
};

/* -----------------------------------------------------------------------
 * Subdriver descriptor
 * ----------------------------------------------------------------------- */
const ups_subdriver_t apc_subdriver = {
    .name    = "apc",
    .devices = apc_devices,
    .var_map = apc_var_map,
};
