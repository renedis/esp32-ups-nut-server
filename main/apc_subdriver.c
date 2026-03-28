#include "apc_subdriver.h"
#include "hid_var_map.h"
#include "hid_parser.h"
#include "ups_driver.h"
#include "ups_subdriver.h"

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

/* -----------------------------------------------------------------------
 * APC-specific status bit lookup tables
 * ----------------------------------------------------------------------- */
static const ups_lkp_t lkp_ac_present[] = {
    { 1, "OL" }, { 0, "OB" }, { 0, NULL }
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
static const ups_lkp_t lkp_boost[] = {
    { 1, "BOOST" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_buck[] = {
    { 1, "TRIM" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_beeper[] = {
    { 1, "enabled" }, { 2, "disabled" }, { 3, "muted" }, { 0, NULL }
};
static const ups_lkp_t lkp_test_result[] = {
    { 1, "Done and passed" }, { 2, "Done and warning" },
    { 3, "Done and error"  }, { 4, "Aborted"          },
    { 5, "In progress"     }, { 6, "No test initiated" },
    { 0, NULL }
};

/* -----------------------------------------------------------------------
 * APC HID descriptor collection layout (discovered via /api/hid dump):
 *
 *  UPS (0x840004)
 *  ├─ 0x840024  — PowerConverter/top-level battery block
 *  │    battery chemistry, status bits, DC voltage (uexp=5), date
 *  ├─ 0x840012  — Battery subsystem
 *  │    charge %, runtime, DC voltage (uexp=5), capacity, date
 *  ├─ 0x0084001A  — AC Input (APC "Flow" collection, usage=0x1A)
 *  │    ConfigVoltage nominal (uexp=7), actual Voltage (uexp=7),
 *  │    LowVoltageTransfer, HighVoltageTransfer
 *  ├─ 0x840016  — AC Output
 *  │    PercentLoad, NominalActivePower
 *  └─ 0x00840002  — PresentStatus (flat bitfield report)
 *       AC_PRESENT, Charging, Discharging, BelowRemainingCapacity,
 *       ShutdownImminent, NeedReplacement, …
 *
 * Note: APC uses HID_USAGE_PD_FLOW (0x84001A) as the AC input
 * subcollection instead of the standard HID_USAGE_PD_INPUT (0x84001C).
 * Voltage fields in 0x84001A have uexp=7; the canonical exponent for
 * voltage (0x00F0D121) is also 7, so hid_scale_value() returns logical
 * directly in Volts (adjusted_exp = 7-7 = 0).
 *
 * Battery voltage (0x840030) appears in both 0x840024 and 0x840012 with
 * uexp=5. hid_scale_value() applies adjusted_exp = 5-7 = -2, converting
 * the raw 10 mV units to Volts (e.g. 1350 → 13.50 V).
 * ----------------------------------------------------------------------- */
static const ups_var_map_t apc_var_map[] = {

    /* --- ups.status bits (from PresentStatus flat report 0x16) --- */
    { "ups.status", HID_USAGE_BS_AC_PRESENT,          HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_ac_present,        NULL },
    { "ups.status", HID_USAGE_BS_CHARGING,             HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_charging,          NULL },
    { "ups.status", HID_USAGE_BS_DISCHARGING,          HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_discharging,       NULL },
    { "ups.status", HID_USAGE_BS_BELOW_REMAINING_CAP,  HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_low_battery,       NULL },
    { "ups.status", HID_USAGE_BS_NEED_REPLACEMENT,     HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_replace_battery,   NULL },
    { "ups.status", HID_USAGE_PD_SHUTDOWN_IMMINENT,    HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_shutdown_imminent, NULL },

    /* --- battery -------------------------------------------------- */
    { "battery.charge",
      HID_USAGE_BS_REMAINING_CAPACITY, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.charge.low",
      HID_USAGE_BS_REMAINING_CAP_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.charge.warning",
      HID_USAGE_BS_WARNING_CAP_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.runtime",
      HID_USAGE_BS_RUNTIME_TO_EMPTY, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    /* battery DC voltage — lives in APC's Battery subcollection (0x840012),
     * usage 0x840030 with uexp=5; hid_scale_value applies adj_exp=-2 → Volts */
    { "battery.voltage",
      HID_USAGE_PD_VOLTAGE, HID_USAGE_PD_BATTERY, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "battery.temperature",
      HID_USAGE_BS_TEMPERATURE, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
    { "battery.cycle.count",
      HID_USAGE_BS_CYCLE_COUNT, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.mfr.date",
      HID_USAGE_BS_MANUFACTURER_DATE, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATIC, NULL, ups_scale_mfr_date },

    /* --- input (APC uses 0x84001A "Flow" as AC input subcollection) */
    { "input.voltage",
      HID_USAGE_PD_VOLTAGE, HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "input.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.frequency",
      HID_USAGE_PD_FREQUENCY, HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_frequency },
    { "input.transfer.low",
      HID_USAGE_PD_LOW_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.transfer.high",
      HID_USAGE_PD_HIGH_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- output (APC uses 0x840016 as AC output subcollection) ---- */
    { "output.voltage",
      HID_USAGE_PD_VOLTAGE, HID_USAGE_PD_AC_OUTPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "output.current",
      HID_USAGE_PD_CURRENT, HID_USAGE_PD_AC_OUTPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_current },
    { "output.frequency",
      HID_USAGE_PD_FREQUENCY, HID_USAGE_PD_AC_OUTPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_frequency },

    /* --- UPS ------------------------------------------------------ */
    { "ups.load",
      HID_USAGE_PD_PERCENT_LOAD, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "ups.temperature",
      HID_USAGE_PD_TEMPERATURE, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
    { "ups.realpower.nominal",
      HID_USAGE_PD_CONFIG_ACTIVE_POWER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "ups.delay.start",
      HID_USAGE_PD_DELAY_BEFORE_STARTUP, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "ups.delay.shutdown",
      HID_USAGE_PD_DELAY_BEFORE_SHUTDOWN, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "ups.beeper.status",
      HID_USAGE_PD_AUDIBLE_ALARM, 0, HID_ITEM_TYPE_FEATURE,
      0, lkp_beeper, NULL },
    { "ups.test.result",
      HID_USAGE_PD_TEST, 0, HID_ITEM_TYPE_FEATURE,
      0, lkp_test_result, NULL },

    /* --- additional status bits ---------------------------------- */
    { "ups.status", HID_USAGE_BS_OVERLOAD,             HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_overload, NULL },
    { "ups.status", HID_USAGE_PD_BOOST,                HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_boost,    NULL },
    { "ups.status", HID_USAGE_PD_BUCK,                 HID_USAGE_PRESENT_STATUS,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_buck,     NULL },

    /* --- additional battery -------------------------------------- */
    { "battery.runtime.low",
      HID_USAGE_BS_REMAINING_TIME_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_POWER_SUMMARY, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- additional output --------------------------------------- */
    { "output.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_AC_OUTPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- power --------------------------------------------------- */
    { "ups.power",
      HID_USAGE_PD_APPARENT_POWER, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "ups.power.nominal",
      HID_USAGE_PD_CONFIG_APPARENT_POWER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },

    /* sentinel */
    { NULL, 0, 0, HID_ITEM_TYPE_FEATURE, 0, NULL, NULL }
};

const ups_subdriver_t apc_subdriver = {
    .name    = "apc",
    .devices = apc_devices,
    .var_map = apc_var_map,
};
