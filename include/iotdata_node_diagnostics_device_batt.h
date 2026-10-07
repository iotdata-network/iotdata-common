#ifndef IOTDATA_NODE_DIAGNOSTICS_DEVICE_BATT_H
#define IOTDATA_NODE_DIAGNOSTICS_DEVICE_BATT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_diagnostics_device_batt.h - the power story a battery node tells its blackbox: what
// it runs on, and what that supply is doing.
//
// ONE FILE PER DEVICE, named for the device, next to the driver -- the same move
// iotdata_node_config_device_e22900t22.h makes, and for the same reason. What a driver REPORTS is
// as much a property of that driver as what it can be SET to, and neither belongs in whichever
// application wanted it first.
//
// Two rails, because they answer different questions. The PACK is the cell: its chemistry, its
// range, what it reads now. The SUPPLY is the 3V3 the regulator makes from it, and that is the one
// the brownout detector watches -- so the floor at which the node actually stops is a property of
// the regulator and the BOD level, not of the cell.
//
// Depends on d_interface_batt.h for the pack constants, iotdata_node_diagnostics.h for the
// recorder, and the APPLICATION for the board's own wiring -- BATTERY_TYPE, PIN_BATTERY_ADC, the
// divider. Those stay with the board, because that is what they describe. Include it after all of
// them.
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <math.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if POWER_DIAGNOSTICS

#define PW_RAIL_PACK   0 /* the cell */
#define PW_RAIL_SUPPLY 1 /* the 3V3 it feeds, via the regulator */

static uint8_t power_chem_of(const battery_type_t type) {
    switch (type) {
    case BATTERY_TYPE_LIPO:
        return IOTDATA_NODE_BB_PW_CHEM_LIPO;
    case BATTERY_TYPE_LIFEPO4:
        return IOTDATA_NODE_BB_PW_CHEM_LIFEPO4;
    case BATTERY_TYPE_LIION:
    default:
        return IOTDATA_NODE_BB_PW_CHEM_LIION;
    }
}
static iotdata_node_bb_pw_event_t power_event_of(const uint32_t cycles, const int16_t was_mv, const uint8_t now_pct) {
    if (cycles <= 1u)
        return IOTDATA_NODE_BB_PW_BOOT;
    if (was_mv > 0) {
        const uint8_t was_pct = battery_percent_of(was_mv, battery_profile(), false, false);
        if (now_pct < BATTERY_PCT_CRITICAL && was_pct >= BATTERY_PCT_CRITICAL)
            return IOTDATA_NODE_BB_PW_CRITICAL;
        if (now_pct <= BATTERY_PCT_LOW && was_pct > BATTERY_PCT_LOW)
            return IOTDATA_NODE_BB_PW_LOW;
    }
    return IOTDATA_NODE_BB_PW_SAMPLE;
}

static void power_startup(const int16_t now_mv) {
    iotdata_node_bb_power_event_t pre = IOTDATA_NODE_BB_POWER_EVENT_INIT(PW_RAIL_PACK);
    pre.event = IOTDATA_NODE_BB_PW_BROWNOUT_PRE;
    pre.mv = now_mv;
    pre.flags = (uint8_t)(IOTDATA_NODE_BB_PW_FLAG_PRESENT | IOTDATA_NODE_BB_PW_FLAG_CRITICAL);
    iotdata_node_diagnostics_power(&pre);
}

static void power_describe(const bool battery_present) {
    char buf[32];
    if (battery_present) {
        const battery_profile_t *const prof = battery_profile();
        int min_mv = 0, max_mv = 0;
        battery_range_mv(prof, false, false, &min_mv, &max_mv);
        iotdata_node_bb_power_source_t pack = IOTDATA_NODE_BB_POWER_SOURCE_INIT(PW_RAIL_PACK, IOTDATA_NODE_BB_PW_TYPE_BATTERY);
        pack.chem = power_chem_of(prof->type);
        pack.cells = 1; /* 1S only: a series pack reads plausibly and gauges nonsense -- see the profile */
        pack.min_mv = min_mv;
        pack.max_mv = max_mv;
        pack.capacity_mah = prof->capacity_mah;
        pack.ratio_x100 = BATTERY_DIVIDER_RATIO_X100;
        pack.offset_mv = prof->offset_mv;
        iotdata_node_diagnostics_power_source(&pack);
        iotdata_node_diagnostics_power_detail(PW_RAIL_PACK, "adc", snprintf_inline(buf, sizeof(buf), "gpio%d/%dnf", (int)PIN_BATTERY_ADC, BATTERY_DIVIDER_C1_NF));
    }
    iotdata_node_bb_power_source_t supply = IOTDATA_NODE_BB_POWER_SOURCE_INIT(PW_RAIL_SUPPLY, IOTDATA_NODE_BB_PW_TYPE_REGULATED);
    supply.from_rail = battery_present ? PW_RAIL_PACK : IOTDATA_NODE_BB_PW_NO_RAIL;
    supply.nominal_mv = 3300;
    iotdata_node_diagnostics_power_source(&supply);
#if defined(CONFIG_ESP_BROWNOUT_DET_LVL)
    /* The brownout detector watches THIS rail, not the pack, and it is what decides when the node
       stops -- so the floor is a property of the regulator and this number, not of the cell. */
    iotdata_node_diagnostics_power_detail(PW_RAIL_SUPPLY, "bod", snprintf_inline(buf, sizeof(buf), "lvl%d", CONFIG_ESP_BROWNOUT_DET_LVL));
#endif
}

/* `ambient_c` is NULL when nothing measured one. A pointer rather than a value so that absent
   stays distinguishable from a genuine 0 degC -- the same reason ua and mw are left out below. */
static void power_sample(const battery_reading_t *const r, const float *const ambient_c, const uint32_t cycles, const int16_t was_mv, const uint8_t now_pct) {
    iotdata_node_bb_power_event_t s = IOTDATA_NODE_BB_POWER_EVENT_INIT(PW_RAIL_PACK);
    s.event = power_event_of(cycles, was_mv, now_pct);
    s.mv = r->voltage_mv;
    s.pct = r->percent;
    /* ua and mw stay ABSENT: nothing on this board measures current, and an absent field says so
       where a zero would lie. */
    if (ambient_c != NULL) /* in the same housing -- the closest thing to the pack's own temperature */
        s.degc = (int8_t)lrintf(*ambient_c);
    s.flags = (uint8_t)(IOTDATA_NODE_BB_PW_FLAG_PRESENT | (r->charging ? IOTDATA_NODE_BB_PW_FLAG_CHARGING : IOTDATA_NODE_BB_PW_FLAG_DISCHARGING) | (r->percent <= BATTERY_PCT_LOW ? IOTDATA_NODE_BB_PW_FLAG_LOW : 0u) |
                        (r->percent < BATTERY_PCT_CRITICAL ? IOTDATA_NODE_BB_PW_FLAG_CRITICAL : 0u));
    iotdata_node_diagnostics_power(&s);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#else

static void power_startup(__attribute__((unused)) const int16_t now_mv) {
}
static void power_describe(__attribute__((unused)) const bool battery_present) {
}
static void power_sample(__attribute__((unused)) const battery_reading_t *const r, __attribute__((unused)) const float *const ambient_c, __attribute__((unused)) const uint32_t cycles, __attribute__((unused)) const int16_t was_mv,
                         __attribute__((unused)) const uint8_t now_pct) {
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif // POWER_DIAGNOSTICS

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_DIAGNOSTICS_DEVICE_BATT_H */
