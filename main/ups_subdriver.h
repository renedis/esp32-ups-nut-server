/* ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hid_parser.h"

/* -----------------------------------------------------------------------
 * Lookup table: maps raw HID integer values to NUT strings.
 * Terminate with {0, NULL}.
 *
 * For status-bit entries (UPS_MAP_STATUS_BIT):
 *   {1, "TOKEN"} → append TOKEN to ups.status when bit=1
 *   {0, "TOKEN"} → append TOKEN when bit=0
 *   Use "" to append nothing for a given state.
 * ----------------------------------------------------------------------- */
typedef struct {
    int32_t     hid_val;
    const char *nut_val;
} ups_lkp_t;

/* -----------------------------------------------------------------------
 * Scale function: converts raw HID value + field metadata → NUT string.
 * Standard implementations are in ups_driver.c / declared in ups_driver.h.
 * ----------------------------------------------------------------------- */
/* physical is the already-scaled value from hid_scale_value() (SI units) */
typedef void (*ups_scale_fn_t)(char *buf, size_t len,
                               double physical, const hid_field_t *field);

/* -----------------------------------------------------------------------
 * Mapping-table entry flags
 * ----------------------------------------------------------------------- */
#define UPS_MAP_STATIC      (1u<<0)  /* Read once at connect, skip if already set */
#define UPS_MAP_SEMI_STATIC (1u<<1)  /* Configuration param — polled every cycle   */
#define UPS_MAP_STATUS_BIT  (1u<<2)  /* Boolean contributing to ups.status string  */

/* -----------------------------------------------------------------------
 * Single mapping-table entry: one HID field → one NUT variable.
 *
 * Resolution order for scale/format:
 *   1. lkp   → table lookup (for enums and status bits)
 *   2. scale_fn → custom conversion (voltage, temperature, date, …)
 *   3. NULL/NULL → raw int32 printed with %"PRId32"
 *
 * parent_coll: if non-zero the engine first tries to match the field
 *   inside that collection; on failure it falls back to the first field
 *   with the given usage anywhere in the descriptor.  Set to 0 to skip
 *   collection filtering entirely.
 * ----------------------------------------------------------------------- */
typedef struct {
    const char        *nut_name;    /* NUT variable name, e.g. "battery.charge"   */
    uint32_t           usage;       /* HID usage: USAGE(page, id)                 */
    uint32_t           parent_coll; /* Preferred parent collection usage; 0 = any  */
    hid_item_type_t    item_type;   /* FEATURE / INPUT / OUTPUT                   */
    uint8_t            flags;       /* UPS_MAP_* bitmask                          */
    const ups_lkp_t   *lkp;        /* Enum/status lookup table, or NULL          */
    ups_scale_fn_t     scale_fn;    /* Scale function, or NULL                    */
} ups_var_map_t;

/* -----------------------------------------------------------------------
 * VID:PID table entry (terminate with {0, 0, NULL, NULL}).
 * mfr / model may be NULL if the device supplies them via USB string
 * descriptors (not currently fetched; keep them set for reliability).
 * ----------------------------------------------------------------------- */
typedef struct {
    uint16_t    vid;
    uint16_t    pid;
    const char *mfr;    /* Manufacturer string, e.g. "APC"         */
    const char *model;  /* Model string, e.g. "Back-UPS BE850G2"   */
} ups_vid_pid_t;

/* -----------------------------------------------------------------------
 * Subdriver: one struct per vendor / UPS family.
 * Register new subdrivers in ups_driver.c : s_subdriver_list[].
 * ----------------------------------------------------------------------- */
typedef struct {
    const char           *name;     /* Short identifier, e.g. "apc"       */
    const ups_vid_pid_t  *devices;  /* {0,0,NULL,NULL}-terminated          */
    const ups_var_map_t  *var_map;  /* {NULL,…}-terminated mapping table   */
} ups_subdriver_t;
