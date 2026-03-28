#include "hid_var_map.h"
#include "ups_driver.h"
#include "hid_parser.h"

/* -----------------------------------------------------------------------
 * ups.status bit lookup tables
 * ----------------------------------------------------------------------- */
static const ups_lkp_t lkp_ac_present[] = {
    { 1, "OL"  },   /* On line  */
    { 0, "OB"  },   /* On battery */
    { 0, NULL  }
};
static const ups_lkp_t lkp_charging[] = {
    { 1, "CHRG" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_discharging[] = {
    { 1, "DISCHRG" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_low_battery[] = {
    { 1, "LB" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_replace_battery[] = {
    { 1, "RB" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_shutdown_imminent[] = {
    { 1, "FSD" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_overload[] = {
    { 1, "OVER" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_fully_charged[] = {
    { 1, "" }, { 0, "" }, { 0, NULL }  /* no status token for fully charged */
};
static const ups_lkp_t lkp_boost[] = {
    { 1, "BOOST" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_buck[] = {
    { 1, "TRIM" }, { 0, "" }, { 0, NULL }
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
    { 1, "Done and passed"   },
    { 2, "Done and warning"  },
    { 3, "Done and error"    },
    { 4, "Aborted"           },
    { 5, "In progress"       },
    { 6, "No test initiated" },
    { 0, NULL                }
};

/* -----------------------------------------------------------------------
 * Standard HID UPS variable map
 *
 * Column order:  nut_name, usage, parent_coll, item_type, flags, lkp, scale_fn
 * ----------------------------------------------------------------------- */
const ups_var_map_t hid_var_map_standard[] = {

    /* --- ups.status bits ------------------------------------------ */
    { "ups.status", HID_USAGE_BS_AC_PRESENT,           0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_ac_present,        NULL },
    { "ups.status", HID_USAGE_BS_CHARGING,             0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_charging,          NULL },
    { "ups.status", HID_USAGE_BS_DISCHARGING,          0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_discharging,       NULL },
    { "ups.status", HID_USAGE_BS_BELOW_REMAINING_CAP,  0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_low_battery,       NULL },
    { "ups.status", HID_USAGE_BS_NEED_REPLACEMENT,     0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_replace_battery,   NULL },
    { "ups.status", HID_USAGE_PD_SHUTDOWN_IMMINENT,    0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_shutdown_imminent, NULL },

    /* --- battery -------------------------------------------------- */
    { "battery.charge",
      HID_USAGE_BS_REMAINING_CAPACITY,  0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.charge.low",
      HID_USAGE_BS_REMAINING_CAP_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.charge.warning",
      HID_USAGE_BS_WARNING_CAP_LIMIT,   0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.runtime",
      HID_USAGE_BS_RUNTIME_TO_EMPTY,    0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.voltage",
      HID_USAGE_BS_VOLTAGE,             0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "battery.temperature",
      HID_USAGE_BS_TEMPERATURE,         0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
    { "battery.cycle.count",
      HID_USAGE_BS_CYCLE_COUNT,         0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.mfr.date",
      HID_USAGE_BS_MANUFACTURER_DATE,   0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATIC, NULL, ups_scale_mfr_date },

    /* --- input ---------------------------------------------------- */
    /* Most UPS (APC, CPS, etc.) put input under Flow (0x84001A) */
    { "input.voltage",
      HID_USAGE_PD_VOLTAGE,        HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "input.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.frequency",
      HID_USAGE_PD_FREQUENCY,      HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_frequency },
    { "input.transfer.low",
      HID_USAGE_PD_LOW_VOLTAGE_TRANSFER,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.transfer.high",
      HID_USAGE_PD_HIGH_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- output --------------------------------------------------- */
    /* CPS uses Output (0x84001C); APC has its own map with AC_OUTPUT (0x840016) */
    { "output.voltage",
      HID_USAGE_PD_VOLTAGE,   HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "output.current",
      HID_USAGE_PD_CURRENT,   HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_current },
    { "output.frequency",
      HID_USAGE_PD_FREQUENCY, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_frequency },

    /* --- UPS ------------------------------------------------------ */
    { "ups.load",
      HID_USAGE_PD_PERCENT_LOAD,          0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "ups.temperature",
      HID_USAGE_PD_TEMPERATURE,           0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
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
      HID_USAGE_PD_AUDIBLE_ALARM,         0, HID_ITEM_TYPE_FEATURE,
      0, lkp_beeper, NULL },
    { "ups.test.result",
      HID_USAGE_PD_TEST,                  0, HID_ITEM_TYPE_FEATURE,
      0, lkp_test_result, NULL },

    /* --- additional status bits ---------------------------------- */
    { "ups.status", HID_USAGE_BS_OVERLOAD,             0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_overload,       NULL },
    { "ups.status", HID_USAGE_BS_FULLY_CHARGED,        0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_fully_charged,  NULL },
    { "ups.status", HID_USAGE_PD_BOOST,                0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_boost,          NULL },
    { "ups.status", HID_USAGE_PD_BUCK,                 0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_buck,           NULL },

    /* --- additional battery -------------------------------------- */
    { "battery.runtime.low",
      HID_USAGE_BS_REMAINING_TIME_LIMIT,  0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE,        HID_USAGE_POWER_SUMMARY, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- additional output --------------------------------------- */
    { "output.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE,        HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- power --------------------------------------------------- */
    { "ups.power",
      HID_USAGE_PD_APPARENT_POWER,        0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "ups.power.nominal",
      HID_USAGE_PD_CONFIG_APPARENT_POWER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    /* sentinel */
    { NULL, 0, 0, HID_ITEM_TYPE_FEATURE, 0, NULL, NULL }
};
