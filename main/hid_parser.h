#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define HID_MAX_FIELDS     192
#define HID_MAX_DEPTH      8

#define USAGE(page, id)    (((uint32_t)(page) << 16) | ((uint32_t)(id)))

/* USB HID Power Device page (0x84) — collections */
#define HID_USAGE_UPS                   USAGE(0x84, 0x04)
#define HID_USAGE_POWER_SUMMARY         USAGE(0x84, 0x05)
#define HID_USAGE_PRESENT_STATUS        USAGE(0x84, 0x02)
#define HID_USAGE_PD_INPUT              USAGE(0x84, 0x1C)  /* Standard UPS Input subcollection */
#define HID_USAGE_PD_OUTPUT             USAGE(0x84, 0x1D)  /* Standard UPS Output subcollection */
#define HID_USAGE_PD_FLOW               USAGE(0x84, 0x1A)  /* APC: AC input/flow subcollection */
#define HID_USAGE_PD_AC_OUTPUT          USAGE(0x84, 0x16)  /* APC: AC output subcollection */
#define HID_USAGE_PD_BATTERY            USAGE(0x84, 0x12)  /* APC: battery subcollection */
/* USB HID Power Device page (0x84) — measurements */
#define HID_USAGE_PD_VOLTAGE            USAGE(0x84, 0x30)
#define HID_USAGE_PD_CURRENT            USAGE(0x84, 0x31)
#define HID_USAGE_PD_FREQUENCY          USAGE(0x84, 0x32)
#define HID_USAGE_PD_APPARENT_POWER     USAGE(0x84, 0x33)
#define HID_USAGE_PD_ACTIVE_POWER       USAGE(0x84, 0x34)
#define HID_USAGE_PD_PERCENT_LOAD       USAGE(0x84, 0x35)
#define HID_USAGE_PD_TEMPERATURE        USAGE(0x84, 0x36)
/* USB HID Power Device page (0x84) — config */
#define HID_USAGE_PD_CONFIG_VOLTAGE         USAGE(0x84, 0x40)
#define HID_USAGE_PD_CONFIG_ACTIVE_POWER    USAGE(0x84, 0x44)
/* USB HID Power Device page (0x84) — controls */
#define HID_USAGE_PD_LOW_VOLTAGE_TRANSFER   USAGE(0x84, 0x53)
#define HID_USAGE_PD_HIGH_VOLTAGE_TRANSFER  USAGE(0x84, 0x54)
#define HID_USAGE_PD_DELAY_BEFORE_STARTUP   USAGE(0x84, 0x56)
#define HID_USAGE_PD_DELAY_BEFORE_SHUTDOWN  USAGE(0x84, 0x57)
#define HID_USAGE_PD_TEST               USAGE(0x84, 0x58)
#define HID_USAGE_PD_AUDIBLE_ALARM      USAGE(0x84, 0x5A)
#define HID_USAGE_PD_SHUTDOWN_IMMINENT  USAGE(0x84, 0x69)
/* USB HID Power Device page (0x84) — string indices */
#define HID_USAGE_PD_IMANUFACTURER      USAGE(0x84, 0x60)
#define HID_USAGE_PD_IPRODUCT           USAGE(0x84, 0x61)
#define HID_USAGE_PD_ISERIAL            USAGE(0x84, 0x62)

/* USB HID Battery System page (0x85) */
#define HID_USAGE_BS_REMAINING_CAPACITY     USAGE(0x85, 0x66)
#define HID_USAGE_BS_RUNTIME_TO_EMPTY       USAGE(0x85, 0x68)
#define HID_USAGE_BS_FULL_CHARGE_CAPACITY   USAGE(0x85, 0x67)
#define HID_USAGE_BS_DESIGN_CAPACITY        USAGE(0x85, 0x83)
#define HID_USAGE_BS_REMAINING_CAP_LIMIT    USAGE(0x85, 0x28)
#define HID_USAGE_BS_WARNING_CAP_LIMIT      USAGE(0x85, 0x29)
#define HID_USAGE_BS_BELOW_REMAINING_CAP    USAGE(0x85, 0x42)
#define HID_USAGE_BS_CHARGING               USAGE(0x85, 0x44)
#define HID_USAGE_BS_DISCHARGING            USAGE(0x85, 0x45)
#define HID_USAGE_BS_NEED_REPLACEMENT       USAGE(0x85, 0x4B)
#define HID_USAGE_BS_AC_PRESENT             USAGE(0x85, 0xD0)
#define HID_USAGE_BS_BATTERY_PRESENT        USAGE(0x85, 0xD1)
#define HID_USAGE_BS_VOLTAGE                USAGE(0x85, 0x30)
#define HID_USAGE_BS_TEMPERATURE            USAGE(0x85, 0x16)
#define HID_USAGE_BS_MANUFACTURER_DATE      USAGE(0x85, 0x85)
#define HID_USAGE_BS_ISERIAL                USAGE(0x85, 0x86)
#define HID_USAGE_BS_ICHEMISTRY             USAGE(0x85, 0x89)
#define HID_USAGE_BS_CYCLE_COUNT            USAGE(0x85, 0x6B)

typedef enum {
    HID_ITEM_TYPE_INPUT   = 0,
    HID_ITEM_TYPE_OUTPUT  = 1,
    HID_ITEM_TYPE_FEATURE = 2,
} hid_item_type_t;

typedef struct {
    uint32_t usage;
    uint32_t collection_path[HID_MAX_DEPTH];
    uint8_t  collection_depth;
    uint8_t  report_id;
    uint16_t bit_offset;
    uint8_t  bit_size;
    int32_t  logical_min;
    int32_t  logical_max;
    int32_t  phy_min;       /* Physical Minimum (0 if not set) */
    int32_t  phy_max;       /* Physical Maximum (0 if not set) */
    uint32_t unit;          /* HID Unit type (e.g. 0x00F0D121 = voltage) */
    int8_t   unit_exp;      /* HID Unit Exponent, sign-extended from 4 bits */
    hid_item_type_t item_type;
} hid_field_t;

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    uint8_t     count;
} hid_report_map_t;

esp_err_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                      hid_report_map_t *map);

/* Find first field matching usage + item type */
const hid_field_t *hid_find_field(const hid_report_map_t *map,
                                  uint32_t usage,
                                  hid_item_type_t type);

/* Find first field matching usage + item type whose collection path contains
 * parent_collection.  Use this to distinguish e.g. Input.Voltage from
 * Output.Voltage (same usage 0x84/0x30, different parent collection). */
const hid_field_t *hid_find_field_in_collection(const hid_report_map_t *map,
                                                 uint32_t usage,
                                                 hid_item_type_t type,
                                                 uint32_t parent_collection);

int32_t hid_extract_field_value(const uint8_t *report, size_t report_len,
                                const hid_field_t *field);

void hid_dump_report_map(const hid_report_map_t *map);

/*
 * Convert a raw logical value from the HID report into a physical value
 * ready to display, applying:
 *   1. Logical→Physical linear scaling (using phy_min/phy_max when set)
 *   2. Unit exponent adjustment relative to NUT's canonical exponents:
 *        Voltage  (0x00F0D121): canonical 7  → physical × 10^(unit_exp − 7)
 *        Power/VA (0x0000D121): canonical 7  → physical × 10^(unit_exp − 7)
 *        All others            : canonical 0  → physical × 10^unit_exp
 *
 * Clamps logical to [logical_min, logical_max] before scaling (matches NUT).
 */
double hid_scale_value(int32_t logical, const hid_field_t *f);
