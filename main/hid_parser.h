#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define HID_MAX_FIELDS     192
#define HID_MAX_DEPTH      8

#define USAGE(page, id)    (((uint32_t)(page) << 16) | ((uint32_t)(id)))

/* USB HID Power Device page (0x84) */
#define HID_USAGE_UPS                   USAGE(0x84, 0x04)
#define HID_USAGE_POWER_SUMMARY         USAGE(0x84, 0x05)
#define HID_USAGE_PRESENT_STATUS        USAGE(0x84, 0x02)
#define HID_USAGE_PD_VOLTAGE            USAGE(0x84, 0x30)
#define HID_USAGE_PD_CURRENT            USAGE(0x84, 0x31)
#define HID_USAGE_PD_FREQUENCY          USAGE(0x84, 0x32)
#define HID_USAGE_PD_APPARENT_POWER     USAGE(0x84, 0x33)
#define HID_USAGE_PD_ACTIVE_POWER       USAGE(0x84, 0x34)
#define HID_USAGE_PD_PERCENT_LOAD       USAGE(0x84, 0x35)
#define HID_USAGE_PD_TEMPERATURE        USAGE(0x84, 0x36)
#define HID_USAGE_PD_SHUTDOWN_IMMINENT  USAGE(0x84, 0x69)
#define HID_USAGE_PD_DELAY_BEFORE_SHUTDOWN USAGE(0x84, 0x57)
#define HID_USAGE_PD_DELAY_BEFORE_STARTUP  USAGE(0x84, 0x56)
#define HID_USAGE_PD_AUDIBLE_ALARM      USAGE(0x84, 0x5A)
#define HID_USAGE_PD_IPRODUCT           USAGE(0x84, 0x61)
#define HID_USAGE_PD_IMANUFACTURER      USAGE(0x84, 0x60)
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
    hid_item_type_t item_type;
} hid_field_t;

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    uint8_t     count;
} hid_report_map_t;

esp_err_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                      hid_report_map_t *map);

const hid_field_t *hid_find_field(const hid_report_map_t *map,
                                  uint32_t usage,
                                  hid_item_type_t type);

int32_t hid_extract_field_value(const uint8_t *report, size_t report_len,
                                const hid_field_t *field);

void hid_dump_report_map(const hid_report_map_t *map);
