#ifndef IOTDATA_NODE_CONFIG_CART_H
#define IOTDATA_NODE_CONFIG_CART_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_cart.h - CONFIG rows for the switched-rail companion (d_interface_cart.h).
//
// A SUBSYSTEM, like the radio's block and the mesh's: a node either tows a cart or it does not, and
// that says nothing about what else it is. The relay is the first to carry one; nothing here is
// relay-shaped.
//
// OFF BY DEFAULT, which is the important row. Every node that compiles this block gets the rows, and
// almost none of them have a cart wired -- a node that switched an unconnected pin on a schedule
// would be harmless, and a node that switched a pin someone had used for something ELSE would not
// be. So the default is a table that does nothing until somebody says otherwise, and CART_ENABLE
// carries REBOOT because the module is brought up once at start.
//
// WHY THESE ARE SETTINGS AND NOT CONSTANTS. A window is expensive -- it is the only thing on the
// site that draws real current -- and how often it is worth paying for depends on where the node is
// and what season it is. Being able to slow a site down to daily from the far end of a radio link,
// or open one on demand when somebody is about to drive out to it, is most of the value.
//
// WHAT IS NOT HERE: the PINS. Which GPIO switches the rail is a fact about the board, not a decision
// about the deployment; getting it wrong does not need a tuning knob, it needs a different build.
// They are compile-time overrides on the module, like the radio's module type.
//
//     #include "iotdata_node_config.h"            // the types
//     #include "iotdata_node_config_cart.h"       // this block
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_CART(X)
//     #include "iotdata_node_config.h"            // expand
//     #include "iotdata_node_config_cart.h"       // and now iotdata_node_config_cart_apply()
//
// 0x0D0-0x0DF, and never reused.
//
// UNITS ARE IN THE NAME, and they differ on purpose: the two schedule numbers are MINUTES because
// nobody reasons about a maintenance interval in seconds, and the three window numbers are SECONDS
// because they are the length of a boot and a shutdown. Converting silently at the seam would be one
// more place to be wrong.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_CART_ENABLE       0x0D0
#define IOTDATA_NODE_CFGID_CART_INTERVAL_MIN 0x0D1
#define IOTDATA_NODE_CFGID_CART_BOOT_S       0x0D2
#define IOTDATA_NODE_CFGID_CART_SILENT_S     0x0D3
#define IOTDATA_NODE_CFGID_CART_SETTLE_S     0x0D4
#define IOTDATA_NODE_CFGID_CART_LIMIT_MIN    0x0D5
#define IOTDATA_NODE_CFGID_CART_RETRY_MIN    0x0D6
#define IOTDATA_NODE_CFGID_CART_RETRY_MAX    0x0D7

#ifndef IOTDATA_NODE_CONFIG_CART_ENABLE
#define IOTDATA_NODE_CONFIG_CART_ENABLE false
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_INTERVAL_MIN
#define IOTDATA_NODE_CONFIG_CART_INTERVAL_MIN (12u * 60u) /* twice a day */
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_BOOT_S
#define IOTDATA_NODE_CONFIG_CART_BOOT_S 120u
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_SILENT_S
#define IOTDATA_NODE_CONFIG_CART_SILENT_S 15u
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_SETTLE_S
#define IOTDATA_NODE_CONFIG_CART_SETTLE_S 30u
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_LIMIT_MIN
#define IOTDATA_NODE_CONFIG_CART_LIMIT_MIN 120u
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_RETRY_MIN
#define IOTDATA_NODE_CONFIG_CART_RETRY_MIN (12u * 60u)
#endif
#ifndef IOTDATA_NODE_CONFIG_CART_RETRY_MAX
#define IOTDATA_NODE_CONFIG_CART_RETRY_MAX 4u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iotdata_node_config_cart_boot_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
static bool iotdata_node_config_cart_limit_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);

#ifndef IOTDATA_NODE_CONFIG_CART_NOTIFY
static inline bool iotdata_node_config_cart_changed(__attribute__((unused)) const iotdata_node_config_row_t *const row, __attribute__((unused)) const iotdata_node_config_value_t *const was,
                                                    __attribute__((unused)) const iotdata_node_config_info_t *const info) {
    return false;
}
#define IOTDATA_NODE_CONFIG_CART_NOTIFY iotdata_node_config_cart_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONFIG_ENTRIES_CART(X) \
    X(CART_ENABLE, IOTDATA_NODE_CFGID_CART_ENABLE, BOOL, 0, 1, IOTDATA_NODE_CONFIG_CART_ENABLE, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, "enabled") \
    X(CART_INTERVAL_MIN, IOTDATA_NODE_CFGID_CART_INTERVAL_MIN, U16, 1, 10080, IOTDATA_NODE_CONFIG_CART_INTERVAL_MIN, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, \
      "period between windows, measured from the last cut") \
    X(CART_BOOT_S, IOTDATA_NODE_CFGID_CART_BOOT_S, U16, 5, 3600, IOTDATA_NODE_CONFIG_CART_BOOT_S, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_cart_boot_ok, IOTDATA_NODE_CONFIG_CART_NOTIFY, \
      "time to hear the first cartbeat before giving up on the boot") \
    X(CART_SILENT_S, IOTDATA_NODE_CFGID_CART_SILENT_S, U16, 1, 3600, IOTDATA_NODE_CONFIG_CART_SILENT_S, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, "time without a cartbeat before believing it has stopped") \
    X(CART_SETTLE_S, IOTDATA_NODE_CFGID_CART_SETTLE_S, U16, 0, 3600, IOTDATA_NODE_CONFIG_CART_SETTLE_S, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, "time the shutdown gets after that, before the rail goes down") \
    X(CART_LIMIT_MIN, IOTDATA_NODE_CFGID_CART_LIMIT_MIN, U16, 1, 1440, IOTDATA_NODE_CONFIG_CART_LIMIT_MIN, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_cart_limit_ok, IOTDATA_NODE_CONFIG_CART_NOTIFY, \
      "time until a still-beating cart is cut anyway, safety backstop") \
    X(CART_RETRY_MIN, IOTDATA_NODE_CFGID_CART_RETRY_MIN, U16, 0, 10080, IOTDATA_NODE_CONFIG_CART_RETRY_MIN, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, "time added to the interval per consecutive failed window") \
    X(CART_RETRY_MAX, IOTDATA_NODE_CFGID_CART_RETRY_MAX, U8, 0, 60, IOTDATA_NODE_CONFIG_CART_RETRY_MAX, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CART_NOTIFY, "failures that back-off keeps stretching for")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_CART_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_EXPANDED) && !defined(IOTDATA_NODE_CONFIG_CART_APPLIED)
#define IOTDATA_NODE_CONFIG_CART_APPLIED

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iotdata_node_config_cart_boot_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t limit;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_CART_LIMIT_MIN, &limit) && v->u < limit.u * 60u;
}
static bool iotdata_node_config_cart_limit_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t boot;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_CART_BOOT_S, &boot) && v->u * 60u > boot.u;
}

/* APPLY NEEDS THE DRIVER, the validators do not. Separating them lets a host compose this block --
   id and bounds checks, which is what iotdata-common/tests/test_node_config_blocks.c does -- without
   dragging in an ESP-IDF header that cannot compile there. Define IOTDATA_NODE_CONFIG_NO_APPLY to take the
   rows and leave the hardware. */
#ifndef IOTDATA_NODE_CONFIG_NO_APPLY

static inline void iotdata_node_config_cart_apply(cart_config_t *const c, const gpio_num_t pin_power, const gpio_num_t pin_live) {
    if (c == NULL)
        return;
    c->pin_power = pin_power;
    c->pin_live = pin_live;
    c->interval_ms = (uint32_t)iotdata_node_config_u16(CART_INTERVAL_MIN) * 60u * 1000u;
    c->boot_ms = (uint32_t)iotdata_node_config_u16(CART_BOOT_S) * 1000u;
    c->silent_ms = (uint32_t)iotdata_node_config_u16(CART_SILENT_S) * 1000u;
    c->settle_ms = (uint32_t)iotdata_node_config_u16(CART_SETTLE_S) * 1000u;
    c->limit_ms = (uint32_t)iotdata_node_config_u16(CART_LIMIT_MIN) * 60u * 1000u;
    c->retry_ms = (uint32_t)iotdata_node_config_u16(CART_RETRY_MIN) * 60u * 1000u;
    c->retry_max = iotdata_node_config_u8(CART_RETRY_MAX);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
#endif /* !IOTDATA_NODE_CONFIG_NO_APPLY */

#endif /* IOTDATA_NODE_CONFIG_EXPANDED && !IOTDATA_NODE_CONFIG_CART_APPLIED */
