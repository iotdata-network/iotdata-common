#ifndef IOTDATA_NODE_CONFIG_MQTT_H
#define IOTDATA_NODE_CONFIG_MQTT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_mqtt.h - CONFIG rows for bridging to an MQTT broker.
//
// A SUBSYSTEM, NOT A ROLE, and emphatically not "the gateway block".
//
// Talking to a broker is something a node either does or does not do, and it says nothing about
// what the node is. A gateway bridges telemetry to one. So does a CONSOLE in the iotdata-machine
// system, which is not a gateway by any reading and has no radio in the argument at all. And a
// gateway with no mesh, hearing a handful of sensors directly, publishes exactly the same way as
// one at the root of a tree.
//
// So this cannot live inside the mesh header, which splits by ROLE WITHIN MESHING (any
// participant, a relay, the root) -- a different axis from what a node is FOR -- and it cannot be
// bundled with ddup under a "gateway" heading either. A box that bridges MQTT and dedups nothing
// should not compile in, or report, rows it will never read. Composing per subsystem is what keeps
// a CONFIG report a description of THIS node rather than of its product category.
//
//     #include "iotdata_node_config.h"            // the types
//     #include "iotdata_node_config_mqtt.h"       // this block
//     #define IOTDATA_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_CONFIG_ENTRIES_MQTT(X)
//     #include "iotdata_node_config.h"            // expand
//
// 0x080-0x08F, and never reused. The low realms are the SHARED ones: an id there means the same
// thing on every box in the fleet, which is what lets one name->id map fragment serve all of them
// and what makes a hex dump readable. An application's own settings stay in its own high realm.
//
// LOCAL, ALMOST THROUGHOUT. A broker URL, a client id, a TLS switch and a topic prefix are things
// whose value is either meaningless to a manager on the radio or has no business travelling over
// one -- and a node talked into publishing somewhere else is a node you have lost. The two
// reconnect delays are not in that category: they are timing, they are readable, and an operator
// may legitimately want to slow a node that is hammering a broker.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_CFGID_MQTT_CLIENT              0x080
#define IOTDATA_CFGID_MQTT_SERVER              0x081
#define IOTDATA_CFGID_MQTT_TLS_INSECURE        0x082
#define IOTDATA_CFGID_MQTT_RECONNECT_DELAY     0x083
#define IOTDATA_CFGID_MQTT_RECONNECT_DELAY_MAX 0x084
#define IOTDATA_CFGID_MQTT_TOPIC_PREFIX        0x085
#define IOTDATA_CFGID_MQTT_DEBUG               0x086

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_MQTT_CLIENT
#define IOTDATA_CONFIG_MQTT_CLIENT "iotdata"
#endif
#ifndef IOTDATA_CONFIG_MQTT_SERVER
#define IOTDATA_CONFIG_MQTT_SERVER "mqtt://localhost"
#endif
#ifndef IOTDATA_CONFIG_MQTT_TOPIC_PREFIX
#define IOTDATA_CONFIG_MQTT_TOPIC_PREFIX "iotdata"
#endif
#ifndef IOTDATA_CONFIG_MQTT_TLS_INSECURE
#define IOTDATA_CONFIG_MQTT_TLS_INSECURE false
#endif
#ifndef IOTDATA_CONFIG_MQTT_RECONNECT_DELAY
#define IOTDATA_CONFIG_MQTT_RECONNECT_DELAY 5u
#endif
#ifndef IOTDATA_CONFIG_MQTT_RECONNECT_DELAY_MAX
#define IOTDATA_CONFIG_MQTT_RECONNECT_DELAY_MAX 60u
#endif
#ifndef IOTDATA_CONFIG_MQTT_DEBUG
#define IOTDATA_CONFIG_MQTT_DEBUG false
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_MQTT_SERVER_MAX
#define IOTDATA_CONFIG_MQTT_SERVER_MAX 191
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iotdata_config_mqtt_reconnect_delay_ok(const iotdata_config_row_t *row, const iotdata_config_value_t *v, const struct iotdata_config_update *u);
static bool iotdata_config_mqtt_reconnect_delay_max_ok(const iotdata_config_row_t *row, const iotdata_config_value_t *v, const struct iotdata_config_update *u);

#ifndef IOTDATA_CONFIG_MQTT_NOTIFY
static inline bool iotdata_config_mqtt_changed(__attribute__((unused)) const iotdata_config_row_t *const row, __attribute__((unused)) const iotdata_config_value_t *const was,
                                               __attribute__((unused)) const iotdata_config_info_t *const info) {
    return false;
}
#define IOTDATA_CONFIG_MQTT_NOTIFY iotdata_config_mqtt_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_CONFIG_ENTRIES_MQTT(X) \
    X(MQTT_CLIENT, IOTDATA_CFGID_MQTT_CLIENT, STRING, 1, 63, IOTDATA_CONFIG_MQTT_CLIENT, IOTDATA_CONFIG_FLAG_REBOOT, NULL, IOTDATA_CONFIG_MQTT_NOTIFY, "the client id to connect to the broker as") \
    X(MQTT_SERVER, IOTDATA_CFGID_MQTT_SERVER, STRING, 1, IOTDATA_CONFIG_MQTT_SERVER_MAX, IOTDATA_CONFIG_MQTT_SERVER, IOTDATA_CONFIG_FLAG_REBOOT, NULL, IOTDATA_CONFIG_MQTT_NOTIFY, "the broker URL to publish telemetry to") \
    X(MQTT_TLS_INSECURE, IOTDATA_CFGID_MQTT_TLS_INSECURE, BOOL, 0, 1, IOTDATA_CONFIG_MQTT_TLS_INSECURE, IOTDATA_CONFIG_FLAG_REBOOT, NULL, IOTDATA_CONFIG_MQTT_NOTIFY, \
      "skip broker certificate verification; for a test broker, not for a deployment") \
    X(MQTT_RECONNECT_DELAY, IOTDATA_CFGID_MQTT_RECONNECT_DELAY, U16, 1, 3600, IOTDATA_CONFIG_MQTT_RECONNECT_DELAY, IOTDATA_CONFIG_FLAG_NONE, iotdata_config_mqtt_reconnect_delay_ok, IOTDATA_CONFIG_MQTT_NOTIFY, \
      "how long to wait before the first reconnection attempt, in seconds") \
    X(MQTT_RECONNECT_DELAY_MAX, IOTDATA_CFGID_MQTT_RECONNECT_DELAY_MAX, U16, 1, 3600, IOTDATA_CONFIG_MQTT_RECONNECT_DELAY_MAX, IOTDATA_CONFIG_FLAG_NONE, iotdata_config_mqtt_reconnect_delay_max_ok, IOTDATA_CONFIG_MQTT_NOTIFY, \
      "the ceiling the retry wait widens to, in seconds") \
    X(MQTT_TOPIC_PREFIX, IOTDATA_CFGID_MQTT_TOPIC_PREFIX, STRING, 1, 63, IOTDATA_CONFIG_MQTT_TOPIC_PREFIX, IOTDATA_CONFIG_FLAG_NONE, NULL, IOTDATA_CONFIG_MQTT_NOTIFY, "what every published topic starts with") \
    X(MQTT_DEBUG, IOTDATA_CFGID_MQTT_DEBUG, BOOL, 0, 1, IOTDATA_CONFIG_MQTT_DEBUG, IOTDATA_CONFIG_FLAG_LOCAL, NULL, IOTDATA_CONFIG_MQTT_NOTIFY, "log every broker interaction")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_MQTT_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_EXPANDED) && !defined(IOTDATA_NODE_CONFIG_MQTT_APPLIED)
#define IOTDATA_NODE_CONFIG_MQTT_APPLIED

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iotdata_config_mqtt_reconnect_delay_ok(__attribute__((unused)) const iotdata_config_row_t *const row, const iotdata_config_value_t *const v, const struct iotdata_config_update *const u) {
    iotdata_config_value_t mx;
    return iotdata_config_update_peek(u, IOTDATA_CFGID_MQTT_RECONNECT_DELAY_MAX, &mx) && v->u <= mx.u;
}
static bool iotdata_config_mqtt_reconnect_delay_max_ok(__attribute__((unused)) const iotdata_config_row_t *const row, const iotdata_config_value_t *const v, const struct iotdata_config_update *const u) {
    iotdata_config_value_t mn;
    return iotdata_config_update_peek(u, IOTDATA_CFGID_MQTT_RECONNECT_DELAY, &mn) && v->u >= mn.u;
}

#endif /* IOTDATA_NODE_CONFIG_EXPANDED && !IOTDATA_NODE_CONFIG_MQTT_APPLIED */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
