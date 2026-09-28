#ifndef IOTDATA_NODE_CONFIG_STAT_H
#define IOTDATA_NODE_CONFIG_STAT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_stat.h - CONFIG rows for reporting a node's own statistics.
//
// FOUR GROUPS, AND EACH IS ONE ROW. Split along two axes that really are independent -- WHERE the
// numbers go (shown locally, or published somewhere), and WHICH numbers they are:
//
//     IOTDATA_CONFIG_ENTRIES_STAT_DISPLAY(X)       show the node's own execution stats, and how often
//     IOTDATA_CONFIG_ENTRIES_STAT_PUBLISH(X)       publish them, and how often
//     IOTDATA_CONFIG_ENTRIES_STAT_PUBLISH_MQTT(X)  ...when the publish path is MQTT: where to
//     IOTDATA_CONFIG_ENTRIES_STAT_DISPLAY_MESH(X)  show the network/mesh table, and how often
//
// The groups exist because the capabilities are genuinely separable. Everything with a log can
// display; only a node with a transport can publish; only one whose transport is MQTT needs a
// topic; only one that meshes has a table to show. A sensor takes DISPLAY alone. A gateway takes
// all four. Nothing composes a row it will never read, which is what keeps a CONFIG report a
// description of the node rather than of the family it belongs to.
//
// AN INTERVAL OF ZERO IS OFF, which is why "whether" needs no row of its own. That was the obvious
// way to make this verbose -- an enable beside each interval, two rows per group, eight in total,
// and two ways for an operator to turn one thing off that can then disagree. Zero already means
// "never" for a report schedule and "unbounded" for a recorder bound; it means "never" here too.
//
// SECONDS, everywhere, because these are human-scale cadences -- a person deciding how often they
// want to look at something. Nothing here is in a scheduler's milliseconds.
//
//     #include "iotdata_node_config.h"            // the types
//     #include "iotdata_node_config_stat.h"       // the row blocks
//     #define IOTDATA_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_CONFIG_ENTRIES_STAT_DISPLAY(X)
//         IOTDATA_CONFIG_ENTRIES_STAT_PUBLISH(X)
//     #include "iotdata_node_config.h"            // expand
//
// 0x0C0-0x0CF, one realm for all four groups. A realm holds sixteen and this uses four, so there is
// no reason to spend three more realms on making the grouping visible in the numbering -- the
// mesh header already establishes that a row's id says where it was ISSUED, not which group holds
// it today.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_CFGID_STAT_DISPLAY_INTERVAL      0x0C0
#define IOTDATA_CFGID_STAT_PUBLISH_INTERVAL      0x0C1
#define IOTDATA_CFGID_STAT_PUBLISH_MQTT_TOPIC    0x0C2
#define IOTDATA_CFGID_STAT_DISPLAY_MESH_INTERVAL 0x0C3

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_STAT_INTERVAL
#define IOTDATA_CONFIG_STAT_INTERVAL 300u
#endif
#ifndef IOTDATA_CONFIG_STAT_DISPLAY_INTERVAL
#define IOTDATA_CONFIG_STAT_DISPLAY_INTERVAL IOTDATA_CONFIG_STAT_INTERVAL
#endif
#ifndef IOTDATA_CONFIG_STAT_PUBLISH_INTERVAL
#define IOTDATA_CONFIG_STAT_PUBLISH_INTERVAL IOTDATA_CONFIG_STAT_INTERVAL
#endif
#ifndef IOTDATA_CONFIG_STAT_DISPLAY_MESH_INTERVAL
#define IOTDATA_CONFIG_STAT_DISPLAY_MESH_INTERVAL IOTDATA_CONFIG_STAT_INTERVAL
#endif
#ifndef IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC
#define IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC "iotdata/stats"
#endif
#ifndef IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC_MAX
#define IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC_MAX 63
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_STAT_INTERVAL_MAX
#define IOTDATA_CONFIG_STAT_INTERVAL_MAX 3600
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_STAT_NOTIFY
static inline bool iotdata_config_stat_changed(__attribute__((unused)) const iotdata_config_row_t *const row, __attribute__((unused)) const iotdata_config_value_t *const was,
                                               __attribute__((unused)) const iotdata_config_info_t *const info) {
    return false;
}
#define IOTDATA_CONFIG_STAT_NOTIFY iotdata_config_stat_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Anything with somewhere to write a line. */
#define IOTDATA_CONFIG_ENTRIES_STAT_DISPLAY(X) \
    X(STAT_DISPLAY_INTERVAL, IOTDATA_CFGID_STAT_DISPLAY_INTERVAL, U16, 0, IOTDATA_CONFIG_STAT_INTERVAL_MAX, IOTDATA_CONFIG_STAT_DISPLAY_INTERVAL, 0, NULL, IOTDATA_CONFIG_STAT_NOTIFY, \
      "how often to show this node's own counters, in seconds (0 = never)")

/* Anything with a transport to send them over. */
#define IOTDATA_CONFIG_ENTRIES_STAT_PUBLISH(X) \
    X(STAT_PUBLISH_INTERVAL, IOTDATA_CFGID_STAT_PUBLISH_INTERVAL, U16, 0, IOTDATA_CONFIG_STAT_INTERVAL_MAX, IOTDATA_CONFIG_STAT_PUBLISH_INTERVAL, 0, NULL, IOTDATA_CONFIG_STAT_NOTIFY, \
      "how often to publish this node's own counters, in seconds (0 = never)")

/* ...when that transport is MQTT. Separate from PUBLISH because the cadence is a decision about
   how much you want to know and the topic is a decision about a broker's namespace; a node that
   publishes over something else keeps the first and has no use for the second. */
#define IOTDATA_CONFIG_ENTRIES_STAT_PUBLISH_MQTT(X) \
    X(STAT_PUBLISH_MQTT_TOPIC_PREFIX, IOTDATA_CFGID_STAT_PUBLISH_MQTT_TOPIC, STRING, 1, IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC_MAX, IOTDATA_CONFIG_STAT_PUBLISH_MQTT_TOPIC, IOTDATA_CONFIG_FLAG_LOCAL | IOTDATA_CONFIG_FLAG_REBOOT, NULL, \
      IOTDATA_CONFIG_STAT_NOTIFY, "the topic the counters are published under")

/* Only a node with a table of other stations to show.
 *
 * NAMED FOR THE MESH, and on a gateway that is slightly generous: the table it gates also carries
 * plain sensors that never mesh, so "network" would describe the gateway's use of it better. MESH
 * is the composition rule that actually matters -- a node with no mesh has no such table at all --
 * and one name has to serve both. Worth revisiting if a non-meshing node ever grows one. */
#define IOTDATA_CONFIG_ENTRIES_STAT_DISPLAY_MESH(X) \
    X(STAT_DISPLAY_MESH_INTERVAL, IOTDATA_CFGID_STAT_DISPLAY_MESH_INTERVAL, U16, 0, IOTDATA_CONFIG_STAT_INTERVAL_MAX, IOTDATA_CONFIG_STAT_DISPLAY_MESH_INTERVAL, 0, NULL, IOTDATA_CONFIG_STAT_NOTIFY, \
      "how often to show the stations this node knows about, in seconds (0 = never)")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_STAT_H */
