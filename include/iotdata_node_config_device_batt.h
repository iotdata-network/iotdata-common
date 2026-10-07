#ifndef IOTDATA_NODE_CONFIG_DEVICE_BATT_H
#define IOTDATA_NODE_CONFIG_DEVICE_BATT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_device_batt.h - CONFIG rows for the battery gauge.
//
// WHAT IS HERE AND WHAT IS NOT is the line d_interface_batt.h already draws: these are properties
// of the INSTALLATION, not of the chemistry.
//
//   MIN  is where THIS node stops being useful. It moves with the regulator and with how hard the
//        radio pulls: a board carrying a 30dBm module sags further under a transmit than one
//        carrying 22dBm, so the same cell warrants a different floor. It is NOT the cell's
//        datasheet cutoff -- though on an UNPROTECTED cell (IMR and INR are often sold bare,
//        because protection limits the high-drain use they are built for) it is also the only
//        thing standing between the pack and an over-discharge, which makes it load-bearing
//        rather than advisory.
//   MAX  is what YOUR charger leaves a cell RESTING at once surface charge has relaxed, not the
//        4.20V it terminated at. Measure it a few minutes after a charge.
//   OFFSET trims the reading against a meter. The classic thing to want fixed without a reflash.
//   TYPE selects the discharge curve.
//   CAPACITY is INVENTORY. It has no effect on the percentage and must never gain one -- it is
//        here so a fleet can say what a node runs on without being opened, and so a runtime
//        estimate has something to multiply. It is the PACK's, so two 500mAh cells is 1000.
//
// NOT HERE, deliberately:
//   KNEE / MID           the shape of the discharge curve: chemistry, not installation.
//   DIVIDER_RATIO_X100   the resistors actually fitted, which is a build property of that carrier.
//   C1_NF                BATTERY_SETTLE_MS is DERIVED from it precisely so the cap cannot be
//                        changed without the settle following; making it settable would undo that.
//   the probe limits     they diagnose a broken divider, not a pack, and an operator relaxing them
//                        remotely would hide the fault they exist to find.
//
// ON CHEMISTRY NAMES: IMR, INR and ICR are all li-ion here. An IMR14500 is a 3.7V/4.2V cell like an
// 18650, just a smaller one, and its curve bends in the same places. A separate type would claim a
// curve that does not exist and make the reported chemistry a lie.
//
//     #include "iotdata_node_config.h"                  // the types
//     #include "iotdata_node_config_device_batt.h"      // the validators and the row block
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_BATT(X)
//     #include "iotdata_node_config.h"                  // expand
//     #include "iotdata_node_config_device_batt.h"      // now iotdata_node_config_batt_apply()
//
// Requires d_interface_batt.h, for battery_profile_t and the defaults below.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_BATT_TYPE         0x0E0
#define IOTDATA_NODE_CFGID_BATT_CAPACITY_MAH 0x0E1
#define IOTDATA_NODE_CFGID_BATT_MV_MIN       0x0E2
#define IOTDATA_NODE_CFGID_BATT_MV_MAX       0x0E3
#define IOTDATA_NODE_CFGID_BATT_OFFSET_MV    0x0E4

/* THE CHEMISTRY VALUES, for a build that wants the rows without the driver. Unlike the other blocks,
   this one's type validator names the enum members themselves, so it needs them in PASS ONE --
   IOTDATA_NODE_CONFIG_NO_APPLY cannot help, because the validator is not the part that applies anything.
   d_interface_batt.h's battery_type_t wins whenever it is present and these must match it.
   OPEN: the chemistry is pure data, and a config block reaching into an ESP32 driver for it is the
   coupling worth removing -- moving battery_type_t somewhere host-visible would retire this block. */
#ifndef BATTERY_TYPE_LIION
#define BATTERY_TYPE_LIION   0
#define BATTERY_TYPE_LIPO    1
#define BATTERY_TYPE_LIFEPO4 2
#endif
#ifndef BATTERY_TYPE
#define BATTERY_TYPE BATTERY_TYPE_LIION
#endif
#ifndef IOTDATA_NODE_CONFIG_BATT_TYPE
#define IOTDATA_NODE_CONFIG_BATT_TYPE BATTERY_TYPE
#endif
#ifndef IOTDATA_NODE_CONFIG_BATT_CAPACITY_MAH
#define IOTDATA_NODE_CONFIG_BATT_CAPACITY_MAH BATTERY_CAPACITY_MAH
#endif
#ifndef IOTDATA_NODE_CONFIG_BATT_MV_MIN
#define IOTDATA_NODE_CONFIG_BATT_MV_MIN BATTERY_DEFAULT_MV_MIN
#endif
#ifndef IOTDATA_NODE_CONFIG_BATT_MV_MAX
#define IOTDATA_NODE_CONFIG_BATT_MV_MAX BATTERY_DEFAULT_MV_MAX
#endif
#ifndef IOTDATA_NODE_CONFIG_BATT_OFFSET_MV
#define IOTDATA_NODE_CONFIG_BATT_OFFSET_MV BATTERY_OFFSET_MV
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_config_batt_type_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, __attribute__((unused)) const struct iotdata_node_config_update *const u) {
    return v->u == (uint64_t)BATTERY_TYPE_LIION || v->u == (uint64_t)BATTERY_TYPE_LIPO || v->u == (uint64_t)BATTERY_TYPE_LIFEPO4;
}

/* MIN and MAX are only meaningful against each other, so each is checked against the CURRENT value
   of the other -- what the gauge would be left holding if this write landed. A pair that crosses is
   refused rather than clamped: a silently corrected range is a configuration that lies about
   itself, and the gauge would then divide by a negative span. Swapping both means writing them in
   whichever order never crosses on the way.
   DECLARED here and DEFINED after the expansion, because reading another row needs the accessors
   and the row names, and neither exists until the table has been expanded. The row block below
   refers to them by name, which a declaration is enough for. */
static bool iotdata_node_config_batt_mv_min_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
static bool iotdata_node_config_batt_mv_max_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_BATT_NOTIFY
#define IOTDATA_NODE_CONFIG_BATT_NOTIFY NULL
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONFIG_ENTRIES_BATT(X) \
    X(BATT_TYPE, IOTDATA_NODE_CFGID_BATT_TYPE, U8, 0, 2, IOTDATA_NODE_CONFIG_BATT_TYPE, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_batt_type_ok, IOTDATA_NODE_CONFIG_BATT_NOTIFY, \
      "cell chemistry (0 = li-ion, 1 = lipo, 2 = lifepo4)") \
    X(BATT_CAPACITY_MAH, IOTDATA_NODE_CFGID_BATT_CAPACITY_MAH, U16, 0, 60000, IOTDATA_NODE_CONFIG_BATT_CAPACITY_MAH, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BATT_NOTIFY, "the PACK's capacity (0 = not stated)") \
    X(BATT_MV_MIN, IOTDATA_NODE_CFGID_BATT_MV_MIN, I16, 1500, 4500, IOTDATA_NODE_CONFIG_BATT_MV_MIN, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_batt_mv_min_ok, IOTDATA_NODE_CONFIG_BATT_NOTIFY, \
      "0%: node's useful floor, not the cell's cutoff") \
    X(BATT_MV_MAX, IOTDATA_NODE_CFGID_BATT_MV_MAX, I16, 1500, 4500, IOTDATA_NODE_CONFIG_BATT_MV_MAX, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_batt_mv_max_ok, IOTDATA_NODE_CONFIG_BATT_NOTIFY, \
      "100%: node's charged maximum, not the cell's topout") \
    X(BATT_OFFSET_MV, IOTDATA_NODE_CFGID_BATT_OFFSET_MV, I16, -500, 500, IOTDATA_NODE_CONFIG_BATT_OFFSET_MV, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BATT_NOTIFY, "calibration trim against a meter")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_DEVICE_BATT_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_EXPANDED) && !defined(IOTDATA_NODE_CONFIG_DEVICE_BATT_APPLIED)
#define IOTDATA_NODE_CONFIG_DEVICE_BATT_APPLIED

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Applied WHOLE, never field by field: min and max only mean anything together, and a profile
   half-way between two settings would gauge against a range that was never configured. Returns
   false only if the pair crosses, which the validators above should already have prevented -- it is
   checked again here because a stored config from an older build has not been through them. */
static bool iotdata_node_config_batt_mv_min_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, __attribute__((unused)) const struct iotdata_node_config_update *const u) {
    return v->i < (int64_t)iotdata_node_config_i16(BATT_MV_MAX);
}

static bool iotdata_node_config_batt_mv_max_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, __attribute__((unused)) const struct iotdata_node_config_update *const u) {
    return v->i > (int64_t)iotdata_node_config_i16(BATT_MV_MIN);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* APPLY NEEDS THE DRIVER, the validators do not. Separating them lets a host compose this block --
   id and bounds checks, which is what iotdata-common/tests/test_node_config_blocks.c does -- without
   dragging in an ESP-IDF header that cannot compile there. Define IOTDATA_NODE_CONFIG_NO_APPLY to take the
   rows and leave the hardware. */
#ifndef IOTDATA_NODE_CONFIG_NO_APPLY

static inline bool iotdata_node_config_batt_apply(void) {
    return battery_profile_set(&(const battery_profile_t){
        .type = (battery_type_t)iotdata_node_config_u8(BATT_TYPE),
        .capacity_mah = iotdata_node_config_u16(BATT_CAPACITY_MAH),
        .mv_min = iotdata_node_config_i16(BATT_MV_MIN),
        .mv_max = iotdata_node_config_i16(BATT_MV_MAX),
        .offset_mv = iotdata_node_config_i16(BATT_OFFSET_MV),
    });
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
#endif /* !IOTDATA_NODE_CONFIG_NO_APPLY */

#endif /* IOTDATA_NODE_CONFIG_EXPANDED && !IOTDATA_NODE_CONFIG_DEVICE_BATT_APPLIED */
