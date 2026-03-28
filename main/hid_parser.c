#include "hid_parser.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "hid_parser";

#define HID_ITEM_TYPE_MAIN    0
#define HID_ITEM_TYPE_GLOBAL  1
#define HID_ITEM_TYPE_LOCAL   2

#define HID_MAIN_INPUT        8
#define HID_MAIN_OUTPUT       9
#define HID_MAIN_FEATURE      11
#define HID_MAIN_COLLECTION   10
#define HID_MAIN_END_COLL     12

#define HID_GLOBAL_USAGE_PAGE  0
#define HID_GLOBAL_LOG_MIN     1
#define HID_GLOBAL_LOG_MAX     2
#define HID_GLOBAL_PHY_MIN     3
#define HID_GLOBAL_PHY_MAX     4
#define HID_GLOBAL_UNIT_EXP    5
#define HID_GLOBAL_UNIT        6
#define HID_GLOBAL_REPORT_SIZE 7
#define HID_GLOBAL_REPORT_ID   8
#define HID_GLOBAL_REPORT_CNT  9
#define HID_GLOBAL_PUSH        10
#define HID_GLOBAL_POP         11

#define HID_LOCAL_USAGE        0
#define HID_LOCAL_USAGE_MIN    1
#define HID_LOCAL_USAGE_MAX    2

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    int32_t  phy_min;
    int32_t  phy_max;
    uint32_t unit;
    int8_t   unit_exp;
    uint8_t  report_size;
    uint8_t  report_count;
    uint8_t  report_id;
} hid_global_state_t;

static int32_t read_signed(const uint8_t *data, uint8_t size)
{
    switch (size) {
    case 1: return (int8_t)data[0];
    case 2: return (int16_t)(data[0] | ((uint16_t)data[1] << 8));
    case 4: return (int32_t)(data[0] | ((uint32_t)data[1] << 8) |
                             ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
    }
    return 0;
}

static uint32_t read_unsigned(const uint8_t *data, uint8_t size)
{
    switch (size) {
    case 1: return data[0];
    case 2: return data[0] | ((uint32_t)data[1] << 8);
    case 4: return data[0] | ((uint32_t)data[1] << 8) |
                   ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }
    return 0;
}

esp_err_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                      hid_report_map_t *map)
{
    memset(map, 0, sizeof(*map));

    hid_global_state_t global = {0};
    hid_global_state_t stack[4];
    uint8_t stack_depth = 0;

    uint32_t collection_path[HID_MAX_DEPTH] = {0};
    uint8_t  collection_depth = 0;

    uint32_t local_usages[32];
    uint8_t  local_usage_count = 0;
    uint32_t local_usage_min = 0;
    uint32_t local_usage_max = 0;
    bool     have_usage_range = false;

    /* Separate bit-offset counters per item type (INPUT=0, OUTPUT=1, FEATURE=2).
     * HID reports for different item types sharing the same report ID are
     * independent: each starts at bit 0 in its own report buffer.
     * Using a single counter would corrupt offsets on descriptors with
     * interleaved Input/Feature items for the same report ID (e.g. APC). */
    uint16_t bit_offsets[3][256] = {{0}};

    size_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];
        uint8_t item_size = prefix & 0x03;
        uint8_t item_type = (prefix >> 2) & 0x03;
        uint8_t item_tag  = (prefix >> 4) & 0x0F;

        if (item_size == 3) item_size = 4;

        if (i + item_size > len) {
            ESP_LOGE(TAG, "descriptor truncated at offset %zu", i - 1);
            return ESP_ERR_INVALID_SIZE;
        }

        const uint8_t *data = &desc[i];
        i += item_size;

        if (item_type == HID_ITEM_TYPE_GLOBAL) {
            switch (item_tag) {
            case HID_GLOBAL_USAGE_PAGE:
                global.usage_page = (uint16_t)read_unsigned(data, item_size);
                break;
            case HID_GLOBAL_LOG_MIN:
                global.logical_min = read_signed(data, item_size);
                break;
            case HID_GLOBAL_LOG_MAX:
                global.logical_max = read_signed(data, item_size);
                break;
            case HID_GLOBAL_PHY_MIN:
                global.phy_min = read_signed(data, item_size);
                break;
            case HID_GLOBAL_PHY_MAX:
                global.phy_max = read_signed(data, item_size);
                break;
            case HID_GLOBAL_UNIT_EXP: {
                /* 4-bit signed value: 0x0-0x7 = +0..+7, 0x8-0xF = -8..-1 */
                uint8_t raw_exp = (uint8_t)(read_unsigned(data, item_size) & 0x0F);
                global.unit_exp = (raw_exp > 7) ? (int8_t)(raw_exp | 0xF0) : (int8_t)raw_exp;
                break;
            }
            case HID_GLOBAL_UNIT:
                global.unit = read_unsigned(data, item_size);
                break;
            case HID_GLOBAL_REPORT_SIZE:
                global.report_size = (uint8_t)read_unsigned(data, item_size);
                break;
            case HID_GLOBAL_REPORT_ID:
                global.report_id = (uint8_t)read_unsigned(data, item_size);
                break;
            case HID_GLOBAL_REPORT_CNT:
                global.report_count = (uint8_t)read_unsigned(data, item_size);
                break;
            case HID_GLOBAL_PUSH:
                if (stack_depth < 4) stack[stack_depth++] = global;
                break;
            case HID_GLOBAL_POP:
                if (stack_depth > 0) global = stack[--stack_depth];
                break;
            }
        } else if (item_type == HID_ITEM_TYPE_LOCAL) {
            switch (item_tag) {
            case HID_LOCAL_USAGE: {
                uint32_t u = read_unsigned(data, item_size);
                if (item_size <= 2) u = USAGE(global.usage_page, u);
                if (local_usage_count < 32)
                    local_usages[local_usage_count++] = u;
                break;
            }
            case HID_LOCAL_USAGE_MIN:
                local_usage_min = read_unsigned(data, item_size);
                if (item_size <= 2) local_usage_min = USAGE(global.usage_page, local_usage_min);
                have_usage_range = true;
                break;
            case HID_LOCAL_USAGE_MAX:
                local_usage_max = read_unsigned(data, item_size);
                if (item_size <= 2) local_usage_max = USAGE(global.usage_page, local_usage_max);
                have_usage_range = true;
                break;
            }
        } else if (item_type == HID_ITEM_TYPE_MAIN) {
            switch (item_tag) {
            case HID_MAIN_COLLECTION: {
                uint32_t coll_usage = (local_usage_count > 0) ? local_usages[0] : 0;
                if (collection_depth < HID_MAX_DEPTH)
                    collection_path[collection_depth++] = coll_usage;
                local_usage_count = 0;
                have_usage_range = false;
                break;
            }
            case HID_MAIN_END_COLL:
                if (collection_depth > 0) collection_depth--;
                break;

            case HID_MAIN_INPUT:
            case HID_MAIN_OUTPUT:
            case HID_MAIN_FEATURE: {
                hid_item_type_t field_type =
                    (item_tag == HID_MAIN_INPUT)   ? HID_ITEM_TYPE_INPUT :
                    (item_tag == HID_MAIN_OUTPUT)  ? HID_ITEM_TYPE_OUTPUT :
                                                     HID_ITEM_TYPE_FEATURE;

                uint8_t count = global.report_count;
                uint8_t rid   = global.report_id;

                for (uint8_t f = 0; f < count && map->count < HID_MAX_FIELDS; f++) {
                    uint32_t usage = 0;
                    if (have_usage_range) {
                        uint32_t base = local_usage_min + f;
                        if (base <= local_usage_max) usage = base;
                    } else if (f < local_usage_count) {
                        usage = local_usages[f];
                    } else if (local_usage_count > 0) {
                        usage = local_usages[local_usage_count - 1];
                    }

                    hid_field_t *fld = &map->fields[map->count++];
                    fld->usage            = usage;
                    fld->report_id        = rid;
                    fld->bit_offset       = bit_offsets[field_type][rid];
                    fld->bit_size         = global.report_size;
                    fld->logical_min      = global.logical_min;
                    fld->logical_max      = global.logical_max;
                    fld->phy_min          = global.phy_min;
                    fld->phy_max          = global.phy_max;
                    fld->unit             = global.unit;
                    fld->unit_exp         = global.unit_exp;
                    fld->item_type        = field_type;
                    fld->collection_depth = (uint8_t)(collection_depth < HID_MAX_DEPTH
                                                      ? collection_depth : HID_MAX_DEPTH);
                    memcpy(fld->collection_path, collection_path,
                           fld->collection_depth * sizeof(uint32_t));

                    bit_offsets[field_type][rid] += global.report_size;
                }

                local_usage_count = 0;
                have_usage_range  = false;
                break;
            }
            }
        }
    }

    ESP_LOGI(TAG, "parsed %u fields from descriptor (%zu bytes)", map->count, len);
    return ESP_OK;
}

const hid_field_t *hid_find_field(const hid_report_map_t *map,
                                  uint32_t usage,
                                  hid_item_type_t type)
{
    for (uint8_t i = 0; i < map->count; i++) {
        if (map->fields[i].usage == usage && map->fields[i].item_type == type)
            return &map->fields[i];
    }
    return NULL;
}

const hid_field_t *hid_find_field_in_collection(const hid_report_map_t *map,
                                                 uint32_t usage,
                                                 hid_item_type_t type,
                                                 uint32_t parent_collection)
{
    for (uint8_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        if (f->usage != usage || f->item_type != type) continue;
        for (uint8_t d = 0; d < f->collection_depth; d++) {
            if (f->collection_path[d] == parent_collection)
                return f;
        }
    }
    return NULL;
}

int32_t hid_extract_field_value(const uint8_t *report, size_t report_len,
                                const hid_field_t *field)
{
    uint32_t bit_off   = field->bit_offset;
    uint32_t bit_sz    = field->bit_size;
    uint32_t byte_off  = bit_off / 8;
    uint32_t bit_shift = bit_off % 8;

    if (byte_off >= report_len) return 0;

    uint32_t raw = 0;
    for (uint32_t b = 0; b < (bit_sz + bit_shift + 7) / 8 && (byte_off + b) < report_len; b++)
        raw |= (uint32_t)report[byte_off + b] << (b * 8);

    raw >>= bit_shift;
    raw &= (bit_sz == 32) ? 0xFFFFFFFFu : ((1u << bit_sz) - 1u);

    if (field->logical_min < 0 && bit_sz < 32) {
        if (raw & (1u << (bit_sz - 1)))
            raw |= ~((1u << bit_sz) - 1u);
    }

    return (int32_t)raw;
}

double hid_scale_value(int32_t logical, const hid_field_t *f)
{
    /* Fix unsigned wrap: when logical_max < logical_min (e.g. lmin=0, lmax=-1),
     * the HID descriptor means "unsigned max for this field size".
     * Common on CyberPower and other non-APC UPS devices. */
    int32_t lmin = f->logical_min;
    int32_t lmax = f->logical_max;
    if (lmax < lmin && f->bit_size < 32) {
        lmax = (int32_t)((1u << f->bit_size) - 1u);
    }

    /* Clamp to logical range */
    if (logical < lmin) logical = lmin;
    if (logical > lmax) logical = lmax;

    /* Step 1: Logical → Physical linear scaling.
     * If phy_min == phy_max == 0, skip (physical = logical). */
    double physical;
    if (f->phy_min == 0 && f->phy_max == 0) {
        physical = (double)logical;
    } else {
        double log_range = (double)(lmax - lmin);
        if (log_range == 0.0) {
            physical = (double)f->phy_min;
        } else {
            double phy_range = (double)(f->phy_max - f->phy_min);
            physical = (double)f->phy_min +
                       (double)(logical - lmin) * phy_range / log_range;
        }
    }

    /* Step 2: Unit exponent adjustment.
     * NUT canonical exponents (from NUT's HIDUnits table):
     *   Voltage (0x00F0D121) = 7, Power/VA (0x0000D121) = 7, all others = 0
     * adjusted_exp = unit_exp - canonical_exp
     * physical *= 10^adjusted_exp  */
    int8_t canon = 0;
    if (f->unit == 0x00F0D121u || f->unit == 0x0000D121u) canon = 7;

    int exp = (int)f->unit_exp - (int)canon;
    /* Avoid pow() dependency — multiply/divide by 10 in a loop.
     * Practical range for |exp| is 0-9, so this is fast. */
    if (exp > 0)      for (int i = 0; i < exp;  i++) physical *= 10.0;
    else if (exp < 0) for (int i = 0; i > exp;  i--) physical /= 10.0;

    return physical;
}

void hid_dump_report_map(const hid_report_map_t *map)
{
    static const char *type_name[] = {"INPUT", "OUTPUT", "FEATURE"};
    ESP_LOGI(TAG, "=== HID Report Map (%u fields) ===", map->count);
    for (uint8_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        ESP_LOGI(TAG,
                 "[%2u] %-7s rid=0x%02x usage=0x%08"PRIx32" bit_off=%3u bit_sz=%u "
                 "lmin=%"PRId32" lmax=%"PRId32" pmin=%"PRId32" pmax=%"PRId32
                 " unit=0x%08"PRIx32" uexp=%d",
                 i, type_name[f->item_type], f->report_id, f->usage,
                 f->bit_offset, f->bit_size,
                 f->logical_min, f->logical_max,
                 f->phy_min, f->phy_max,
                 f->unit, (int)f->unit_exp);
    }
    ESP_LOGI(TAG, "=== End HID Report Map ===");
}
