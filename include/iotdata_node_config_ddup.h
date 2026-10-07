#ifndef IOTDATA_NODE_CONFIG_DDUP_H
#define IOTDATA_NODE_CONFIG_DDUP_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_ddup.h - CONFIG rows for cross-gateway deduplication.
//
// A SUBSYSTEM, NOT A ROLE, and the sibling of iotdata_node_config_mqtt.h. Two receivers whose
// coverage overlaps will both hear the same sensor, and both will pass it on; ddup is them telling
// each other what they have seen so that only one does. It needs no mesh -- overlapping coverage is
// the ordinary case for plain receivers -- and it needs no broker, since a node may dedup and hand
// its data on by some other route entirely.
//
// GATEWAY-SHAPED IN PRACTICE, NOT GATEWAY-EXCLUSIVE IN PRINCIPLE. Everything that dedups today is a
// gateway, and that may well stay true; but a tiered or distributed arrangement running dedup
// decoupled from the receivers is describable, if unlikely, and the point is that nothing in these
// rows assumes otherwise. Naming the file for the job rather than for the box costs nothing now and
// does not have to be undone if that day arrives.
//
// That is also why mqtt and ddup are separate files rather than one "gateway" block: a node that
// bridges to a broker and dedups nothing should not carry, or report, rows it will never read, and
// mqtt already has a consumer -- the iotdata-machine console -- that is not a gateway at all.
//
//     #include "iotdata_node_config.h"            // the types
//     #include "iotdata_node_config_ddup.h"       // this block
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_DDUP(X)
//     #include "iotdata_node_config.h"            // expand
//
// 0x090-0x09F, and never reused: the low realms are the shared ones, where an id means the same
// thing on every box in the fleet.
//
// THE PEER LIST IS THE ONE BIG ROW. It is a comma-separated host:port list, and its maximum is its
// storage -- see THE EXPANSION in iotdata_node_config.h -- so it is sized for a handful of peers
// rather than for an arbitrary fleet. A deployment that outgrows it wants a discovery mechanism,
// not a longer string.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_DDUP_ENABLE                  0x090
#define IOTDATA_NODE_CFGID_DDUP_PORT                    0x091
#define IOTDATA_NODE_CFGID_DDUP_PEERS                   0x092
#define IOTDATA_NODE_CFGID_DDUP_DELAY_MS                0x093
#define IOTDATA_NODE_CFGID_DDUP_HOLD_MS                 0x094
#define IOTDATA_NODE_CFGID_DDUP_DEBUG                   0x095
#define IOTDATA_NODE_CFGID_DDUP_DEBUG_INJECT_LATENCY_MS 0x096

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_DDUP_ENABLE
#define IOTDATA_NODE_CONFIG_DDUP_ENABLE false
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_PORT
#define IOTDATA_NODE_CONFIG_DDUP_PORT 9876u
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_PEERS
#define IOTDATA_NODE_CONFIG_DDUP_PEERS ""
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_DELAY
#define IOTDATA_NODE_CONFIG_DDUP_DELAY 20u /* ms until gateway announces a batch. */
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_HOLD_MS
#define IOTDATA_NODE_CONFIG_DDUP_HOLD_MS 1000u
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_DEBUG
#define IOTDATA_NODE_CONFIG_DDUP_DEBUG false
#endif
#ifndef IOTDATA_NODE_CONFIG_DDUP_DEBUG_INJECT_LATENCY_MS
#define IOTDATA_NODE_CONFIG_DDUP_DEBUG_INJECT_LATENCY_MS 0u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_DDUP_PEERS_MAX
#define IOTDATA_NODE_CONFIG_DDUP_PEERS_MAX 255
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_DDUP_NOTIFY
static inline bool iotdata_node_config_ddup_changed(__attribute__((unused)) const iotdata_node_config_row_t *const row, __attribute__((unused)) const iotdata_node_config_value_t *const was,
                                                    __attribute__((unused)) const iotdata_node_config_info_t *const info) {
    return false;
}
#define IOTDATA_NODE_CONFIG_DDUP_NOTIFY iotdata_node_config_ddup_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONFIG_ENTRIES_DDUP(X) \
    X(DDUP_ENABLE, IOTDATA_NODE_CFGID_DDUP_ENABLE, BOOL, 0, 1, IOTDATA_NODE_CONFIG_DDUP_ENABLE, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, "enabled") \
    X(DDUP_PORT, IOTDATA_NODE_CFGID_DDUP_PORT, U16, 1, 65535, IOTDATA_NODE_CONFIG_DDUP_PORT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, "default UDP port") \
    X(DDUP_PEERS, IOTDATA_NODE_CFGID_DDUP_PEERS, STRING, 0, IOTDATA_NODE_CONFIG_DDUP_PEERS_MAX, IOTDATA_NODE_CONFIG_DDUP_PEERS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, \
      "peers, comma-separated host:port (or 'mqtt' for discovery)") \
    X(DDUP_DELAY_MS, IOTDATA_NODE_CFGID_DDUP_DELAY_MS, U16, 0, 10000, IOTDATA_NODE_CONFIG_DDUP_DELAY, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, "batching period, until announcement") \
    X(DDUP_HOLD_MS, IOTDATA_NODE_CFGID_DDUP_HOLD_MS, U32, 0, 60000, IOTDATA_NODE_CONFIG_DDUP_HOLD_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, \
      "ceiling period, until peers can contest (0 = publish at once, no election)") \
    X(DDUP_DEBUG, IOTDATA_NODE_CFGID_DDUP_DEBUG, BOOL, 0, 1, IOTDATA_NODE_CONFIG_DDUP_DEBUG, IOTDATA_NODE_CONFIG_FLAG_LOCAL, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, "debug: log every dedup decision") \
    X(DDUP_DEBUG_INJECT_LATENCY_MS, IOTDATA_NODE_CFGID_DDUP_DEBUG_INJECT_LATENCY_MS, U32, 0, 10000, IOTDATA_NODE_CONFIG_DDUP_DEBUG_INJECT_LATENCY_MS, IOTDATA_NODE_CONFIG_FLAG_LOCAL, NULL, IOTDATA_NODE_CONFIG_DDUP_NOTIFY, \
      "debug: fake peer-claim latency, for election exercise (0 = none)")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_DDUP_H */
