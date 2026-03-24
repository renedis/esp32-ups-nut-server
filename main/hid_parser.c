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

    uint16_t bit_offsets[256] = {0};

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
                    fld->bit_offset       = bit_offsets[rid];
                    fld->bit_size         = global.report_size;
                    fld->logical_min      = global.logical_min;
                    fld->logical_max      = global.logical_max;
                    fld->item_type        = field_type;
                    fld->collection_depth = (uint8_t)(collection_depth < HID_MAX_DEPTH
                                                      ? collection_depth : HID_MAX_DEPTH);
                    memcpy(fld->collection_path, collection_path,
                           fld->collection_depth * sizeof(uint32_t));

                    bit_offsets[rid] += global.report_size;
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

void hid_dump_report_map(const hid_report_map_t *map)
{
    static const char *type_name[] = {"INPUT", "OUTPUT", "FEATURE"};
    ESP_LOGI(TAG, "=== HID Report Map (%u fields) ===", map->count);
    for (uint8_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        ESP_LOGI(TAG,
                 "[%2u] %-7s rid=0x%02x usage=0x%08"PRIx32" bit_off=%3u bit_sz=%u "
                 "lmin=%"PRId32" lmax=%"PRId32,
                 i, type_name[f->item_type], f->report_id, f->usage,
                 f->bit_offset, f->bit_size, f->logical_min, f->logical_max);
    }
    ESP_LOGI(TAG, "=== End HID Report Map ===");
}
