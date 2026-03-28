/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#include "ups_subdriver.h"
#include "hid_var_map.h"
#include "hid_parser.h"
#include "ups_driver.h"
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * CyberPower Systems (CPS) + Cyber Energy (ST Microelectronics OEM)
 * Reference: NUT drivers/cps-hid.c
 *
 * CPS quirks handled here:
 *   - Battery charge can exceed 100% — clamp to 100
 *   - Battery voltage may be 1.5x too high (raw vs nominal)
 *   - Frequency may be 10x too high on some models
 *   - Input lives under PD_FLOW (0x84001A), output under PD_INPUT (0x84001C)
 *     (CPS uses 0x1C for output despite it being "Input" in the HID spec)
 * ----------------------------------------------------------------------- */
static const ups_vid_pid_t cps_devices[] = {
    /* CyberPower */
    { 0x0764, 0x0005, "CyberPower", "900AVR / BC900D"                    },
    { 0x0764, 0x0501, "CyberPower", "CP1200AVR / CP825AVR / CP1500C etc" },
    { 0x0764, 0x0601, "CyberPower", "OR2200LCDRM2U / OR700LCDRM1U etc"   },
    /* Cyber Energy -- ST Micro OEM */
    { 0x0483, 0xa430, "CyberPower", "Cyber Energy UPS"                   },
    { 0, 0, NULL, NULL }
};

/* -----------------------------------------------------------------------
 * CPS-specific scale functions
 *
 * These receive the already-scaled physical value from hid_scale_value()
 * and apply CPS-specific corrections before formatting into the NUT buffer.
 * ----------------------------------------------------------------------- */

/* One-time detection flags (persist across polls for this device) */
static bool s_batt_scale_detected = false;
static bool s_batt_scale_needed   = false;  /* true = divide by 1.5 */
static bool s_freq_scale_detected = false;
static bool s_freq_scale_needed   = false;  /* true = divide by 10 */

/* Clamp charge to 100% */
static void cps_scale_charge(char *buf, size_t len, double physical,
                             const hid_field_t *f)
{
    (void)f;
    if (physical > 100.0) physical = 100.0;
    if (physical < 0.0)   physical = 0.0;
    snprintf(buf, len, "%.0f", physical);
}

/* Battery voltage: detect if raw is >1.4x nominal and scale by 2/3.
 * NUT cps-hid.c: "if value > 1.4 * nominal, divide by 1.5" */
static void cps_scale_battery_voltage(char *buf, size_t len, double physical,
                                      const hid_field_t *f)
{
    (void)f;
    if (!s_batt_scale_detected) {
        /* First reading: check against a reasonable nominal (12V or 24V).
         * If physical > 16.8 (1.4 * 12) for a 12V battery, scale needed.
         * We use a heuristic: if voltage > 18V for what should be 12V,
         * or > 36V for what should be 24V, the raw value is likely 1.5x. */
        if (physical > 18.0) {
            s_batt_scale_needed = true;
        }
        s_batt_scale_detected = true;
    }
    if (s_batt_scale_needed) {
        physical = physical * 2.0 / 3.0;
    }
    snprintf(buf, len, "%.1f", physical);
}

/* Frequency: detect 10x and correct.
 * NUT cps-hid.c: "some models report Hz*10" */
static void cps_scale_frequency(char *buf, size_t len, double physical,
                                const hid_field_t *f)
{
    (void)f;
    if (!s_freq_scale_detected) {
        /* If frequency > 100, it's clearly 10x too high (50Hz or 60Hz grid) */
        if (physical > 100.0) {
            s_freq_scale_needed = true;
        }
        s_freq_scale_detected = true;
    }
    if (s_freq_scale_needed) {
        physical /= 10.0;
    }
    snprintf(buf, len, "%.1f", physical);
}

/* -----------------------------------------------------------------------
 * CPS-specific status bit lookup tables
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
static const ups_lkp_t lkp_fully_charged[] = {
    { 1, "" }, { 0, "" }, { 0, NULL }
};
static const ups_lkp_t lkp_boost[] = {
    { 1, "BOOST" }, { 0, "" }, { 0, NULL }
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
 * CPS HID descriptor layout (typical CyberPower OR/CP series):
 *
 *  UPS (0x840004)
 *  +-- PowerSummary (0x840005)
 *  |     battery.charge, battery.runtime, battery.voltage, temperature
 *  |     status bits (ACPresent, Charging, Discharging, etc.)
 *  +-- Flow (0x84001A) -- AC Input
 *  |     input.voltage, input.frequency, ConfigVoltage
 *  +-- Output (0x84001C) -- note: HID spec calls this "Input" but CPS
 *  |     uses it for output: output.voltage, output.frequency, PercentLoad,
 *  |     ConfigActivePower, ConfigApparentPower
 *  +-- PresentStatus (0x840002)
 *        flat bitfield of status flags
 * ----------------------------------------------------------------------- */
static const ups_var_map_t cps_var_map[] = {

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
    { "ups.status", HID_USAGE_BS_OVERLOAD,             0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_overload,          NULL },
    { "ups.status", HID_USAGE_BS_FULLY_CHARGED,        0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_fully_charged,     NULL },
    { "ups.status", HID_USAGE_PD_BOOST,                0,
      HID_ITEM_TYPE_FEATURE, UPS_MAP_STATUS_BIT, lkp_boost,             NULL },

    /* --- battery -------------------------------------------------- */
    { "battery.charge",
      HID_USAGE_BS_REMAINING_CAPACITY,  0, HID_ITEM_TYPE_FEATURE,
      0, NULL, cps_scale_charge },
    { "battery.charge.low",
      HID_USAGE_BS_REMAINING_CAP_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.charge.warning",
      HID_USAGE_BS_WARNING_CAP_LIMIT,   0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.runtime",
      HID_USAGE_BS_RUNTIME_TO_EMPTY,    0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.runtime.low",
      HID_USAGE_BS_REMAINING_TIME_LIMIT, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "battery.voltage",
      HID_USAGE_PD_VOLTAGE, HID_USAGE_POWER_SUMMARY, HID_ITEM_TYPE_FEATURE,
      0, NULL, cps_scale_battery_voltage },
    { "battery.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_POWER_SUMMARY, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "battery.temperature",
      HID_USAGE_BS_TEMPERATURE, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
    { "battery.cycle.count",
      HID_USAGE_BS_CYCLE_COUNT, 0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "battery.mfr.date",
      HID_USAGE_BS_MANUFACTURER_DATE, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_STATIC, NULL, ups_scale_mfr_date },

    /* --- input (CPS uses Flow 0x84001A as AC input) -------------- */
    { "input.voltage",
      HID_USAGE_PD_VOLTAGE,        HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "input.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.frequency",
      HID_USAGE_PD_FREQUENCY,      HID_USAGE_PD_FLOW, HID_ITEM_TYPE_FEATURE,
      0, NULL, cps_scale_frequency },
    { "input.transfer.low",
      HID_USAGE_PD_LOW_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "input.transfer.high",
      HID_USAGE_PD_HIGH_VOLTAGE_TRANSFER, 0, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },

    /* --- output (CPS uses 0x84001C for output) ------------------- */
    { "output.voltage",
      HID_USAGE_PD_VOLTAGE,   HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_voltage },
    { "output.voltage.nominal",
      HID_USAGE_PD_CONFIG_VOLTAGE, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, ups_scale_voltage },
    { "output.current",
      HID_USAGE_PD_CURRENT,   HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_current },
    { "output.frequency",
      HID_USAGE_PD_FREQUENCY, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, cps_scale_frequency },

    /* --- UPS ------------------------------------------------------ */
    { "ups.load",
      HID_USAGE_PD_PERCENT_LOAD, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
    { "ups.temperature",
      HID_USAGE_PD_TEMPERATURE,           0, HID_ITEM_TYPE_FEATURE,
      0, NULL, ups_scale_temperature },
    { "ups.realpower.nominal",
      HID_USAGE_PD_CONFIG_ACTIVE_POWER, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "ups.power.nominal",
      HID_USAGE_PD_CONFIG_APPARENT_POWER, HID_USAGE_PD_INPUT, HID_ITEM_TYPE_FEATURE,
      UPS_MAP_SEMI_STATIC, NULL, NULL },
    { "ups.power",
      HID_USAGE_PD_APPARENT_POWER,        0, HID_ITEM_TYPE_FEATURE,
      0, NULL, NULL },
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

    /* sentinel */
    { NULL, 0, 0, HID_ITEM_TYPE_FEATURE, 0, NULL, NULL }
};

const ups_subdriver_t cps_subdriver = {
    .name    = "cps",
    .devices = cps_devices,
    .var_map = cps_var_map,
};
