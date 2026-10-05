#ifndef IOTDATA_NODE_CONFIG_DOWN_H
#define IOTDATA_NODE_CONFIG_DOWN_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_down.h - CONFIG rows for the DOWNSTREAM hold store.
//
// NOT MESH. A gateway with no mesh at all still holds commands for sleeping sensors, and so would a
// relay that never routed anything -- holding is what the down store is, and meshing is how a frame
// gets somewhere. Hence its own block rather than a corner of the mesh one.
//
// iotdata_node_down.h already had the setters for all three of these. NOTHING CALLED THEM: the
// knobs existed, were reachable from C, and had no way of being reached by a person. A default that
// cannot be moved is not a default, it is a constant with an apologetic name.
//
// WHAT THEY ARE FOR. A command held for a node that is asleep has to expire eventually or the store
// fills with orders nobody will ever collect. How long is a deployment question and not a protocol
// one: a sensor that wakes hourly wants hours, one on a weekly service round wants a week. The
// broadcast TTL is separate because a broadcast is held for everyone at once and is usually the
// thing you least want dropped early.
//
//     #include "iotdata_node_config.h"      // the types
//     #include "iotdata_node_config_down.h" // the row block
//     #define IOTDATA_CONFIG_ENTRIES(X)  IOTDATA_CONFIG_ENTRIES_DOWN(X)
//     #include "iotdata_node_config.h"      // expand
//     #include "iotdata_node_config_down.h" // now iotdata_config_down_apply()
//
// Requires iotdata_node_down.h, for iotdata_down_t and the defaults below.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_DOWN_MIN_MAX
#define IOTDATA_CONFIG_DOWN_MIN_MAX (IOTDATA_DOWN_TTL_MS_MAX / 60000UL)
#endif
#ifndef IOTDATA_CONFIG_DOWN_TTL_MIN
#define IOTDATA_CONFIG_DOWN_TTL_MIN (IOTDATA_DOWN_TTL_MS_DEFAULT / 60000UL)
#endif
#ifndef IOTDATA_CONFIG_DOWN_TTL_BCAST_MIN
#define IOTDATA_CONFIG_DOWN_TTL_BCAST_MIN (IOTDATA_DOWN_TTL_BCAST_MS_DEFAULT / 60000UL)
#endif
#ifndef IOTDATA_CONFIG_DOWN_REPEAT_MIN
#define IOTDATA_CONFIG_DOWN_REPEAT_MIN (IOTDATA_DOWN_REPEAT_MS_DEFAULT / 60000UL)
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_DOWN_NOTIFY
static inline bool iotdata_config_down_changed(__attribute__((unused)) const iotdata_config_row_t *const row, __attribute__((unused)) const iotdata_config_value_t *const was,
                                               __attribute__((unused)) const iotdata_config_info_t *const info) {
    return false;
}
#define IOTDATA_CONFIG_DOWN_NOTIFY iotdata_config_down_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* 0 is a real value and means "never expire", which the store already understands -- so the floor is
   0 and not 1, and an operator who wants a command to wait indefinitely can say so. */
#define IOTDATA_CONFIG_ENTRIES_DOWN(X) \
    X(DOWN_TTL_MIN, 0x020, U32, 0, IOTDATA_CONFIG_DOWN_MIN_MAX, IOTDATA_CONFIG_DOWN_TTL_MIN, IOTDATA_CONFIG_FLAG_NONE, NULL, IOTDATA_CONFIG_DOWN_NOTIFY, "time to hold a unicast frame waiting for a node (0 = indefinite)") \
    X(DOWN_TTL_BCAST_MIN, 0x021, U32, 0, IOTDATA_CONFIG_DOWN_MIN_MAX, IOTDATA_CONFIG_DOWN_TTL_BCAST_MIN, IOTDATA_CONFIG_FLAG_NONE, NULL, IOTDATA_CONFIG_DOWN_NOTIFY, "time to hold a broadcast frame") \
    X(DOWN_REPEAT_MIN, 0x022, U32, 0, IOTDATA_CONFIG_DOWN_MIN_MAX, IOTDATA_CONFIG_DOWN_REPEAT_MIN, IOTDATA_CONFIG_FLAG_NONE, NULL, IOTDATA_CONFIG_DOWN_NOTIFY, "period to repeat a frame to a node")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_DOWN_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_EXPANDED) && !defined(IOTDATA_NODE_CONFIG_DOWN_APPLIED)
#define IOTDATA_NODE_CONFIG_DOWN_APPLIED

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_config_down_apply(iotdata_down_t *const ds) {
    if (ds == NULL)
        return;
    iotdata_down_set_ttl_ms(ds, iotdata_config_u32(DOWN_TTL_MIN) * 60000UL);
    iotdata_down_set_ttl_bcast_ms(ds, iotdata_config_u32(DOWN_TTL_BCAST_MIN) * 60000UL);
    iotdata_down_set_repeat_ms(ds, iotdata_config_u32(DOWN_REPEAT_MIN) * 60000UL);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_EXPANDED && !IOTDATA_NODE_CONFIG_DOWN_APPLIED */
