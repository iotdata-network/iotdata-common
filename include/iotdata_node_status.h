#ifndef IOTDATA_NODE_STATUS_H
#define IOTDATA_NODE_STATUS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_status.h - how a node says it is DOING: the single source of truth for STATUS.
//
//   node   0x00..0x1F   uptime, restarts, why it last booted, supply, heap -- true of any node
//   mesh   0x20..0x3F   parent, cost, and the counters of taking part in a mesh
//
// STATUS_REQUEST carries which groups are wanted; absent means all of them, so the periodic
// report -- which asks for nothing -- carries everything. A node in no mesh omits the mesh group
// rather than reporting a zeroed one, which is what `present` is for: "no mesh" and "a mesh in
// which nothing has happened yet" are different answers, and a fleet view must be able to tell
// them apart.
//
// WHAT IS OPTIONAL IS DECLARED, NOT INFERRED. Every field but the uptime and the boot reason has
// a presence flag. A gateway on mains has no supply reading and a Linux box's "free heap" means
// something different from an MCU's, so both are omitted rather than sent as zero -- a zero here
// would read as a flat battery or an exhausted heap.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE GROUPS
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * The mesh group, as a struct.
 *
 * Every station that takes part in the mesh answers these -- a gateway (the root), a relay, and in
 * due course a combined relay/sensor. They differ in what they can say, not in how they say it: a
 * root has no parent and never reparents, a relay has both. So the shape is here, once, and each
 * implementation fills in what is true of it.
 *
 * Field names follow the relay's mesh engine, which is where the values originate.
 */
typedef struct {
    bool present;  /* false on a node with no mesh: omit the group entirely */
    uint8_t state; /* IOTDATA_NODE_STATUS_MESH_STATE_* */
    uint16_t parent;
    uint8_t cost;
    uint16_t generation;
    int8_t parent_rssi;
    uint8_t peers;
    bool accepting;
    uint32_t beacon_rx, beacon_tx;
    uint32_t peer_new, reparent, failover, orphan;
    uint32_t rerr_rx, rerr_tx;
    uint32_t forwards, duplicates;
} iotdata_node_status_mesh_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * The node group, plus the mesh group it may or may not have. One struct is the whole of what a
 * node reports about itself, so a device that grows a mesh -- or loses a battery -- changes a
 * field rather than a code path.
 *
 * The uptime and the reason carry no flag: every node has been up for some length of time and
 * every node booted for some cause, `unknown` included. The rest say whether they mean anything.
 */
typedef struct {
    uint32_t uptime_s;   /* this session */
    uint32_t lifetime_s; /* cumulative across boots -- needs somewhere to persist a counter */
    uint32_t active_s;   /* awake, as against asleep: the interesting one on a duty-cycled sensor */
    uint32_t heap_free, heap_min;
    uint16_t restarts;
    uint16_t supply_mv;
    int8_t temperature;
    uint8_t reason;                     /* IOTDATA_NODE_REASON_* */
    uint8_t stations, filters, content; /* table sizes -- SCALARS, so they survive a lost tail */
    bool has_stations, has_filters, has_content;
    bool has_lifetime, has_active, has_heap, has_restarts, has_supply, has_temperature;
    iotdata_node_status_mesh_t mesh;
} iotdata_node_status_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE SCOPE
//
// Which groups a request is asking for. The bits are protocol (iotdata_node.h); the WORDS are
// here, because a scope is typed by a person -- into a console, or into the JSON a manager sends
// -- and both media must spell it the same way or an operator learns two vocabularies.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Absent (0) means the SCALAR groups, not every group. A table is unbounded and spans frames, so
   a periodic report -- which asks for nothing -- must not carry three of them, and a mistyped
   request must not cost twenty frames of airtime. A caller that wants a table says which. */
static inline bool iotdata_node_status_scope_wants(const uint8_t scope, const uint8_t group) {
    return (((scope == 0) ? (uint8_t)IOTDATA_NODE_STATUS_SCOPE_DEFAULT : scope) & group) != 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* "node", "mesh", "stations", "filters", "peers", or any combination; "all" is every group
   INCLUDING the tables, which is the one way to ask for everything now that absent does not.
   Order matters: "peers" is tested before "mesh" only in that both may appear, and "stations"
   must not be swallowed by a substring of another word. */
static inline uint8_t iotdata_node_status_scope_from_name(const char *const s) {
    if (s == NULL)
        return 0;
    if (strcmp(s, "all") == 0)
        return (uint8_t)(IOTDATA_NODE_STATUS_SCOPE_NODE | IOTDATA_NODE_STATUS_SCOPE_MESH | IOTDATA_NODE_STATUS_SCOPE_STATIONS | IOTDATA_NODE_STATUS_SCOPE_FILTERS | IOTDATA_NODE_STATUS_SCOPE_MESH_PEERS | IOTDATA_NODE_STATUS_SCOPE_CONTENT);
    uint8_t bits = 0;
    if (strstr(s, "node") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_NODE;
    if (strstr(s, "mesh") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_MESH;
    if (strstr(s, "stations") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_STATIONS;
    if (strstr(s, "filters") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_FILTERS;
    if (strstr(s, "peers") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_MESH_PEERS;
    if (strstr(s, "content") != NULL)
        bits |= IOTDATA_NODE_STATUS_SCOPE_CONTENT;
    return bits;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Is this word one of ours at all? The parser above is deliberately lenient -- an unrecognised
   scope means the default -- which is right where a member name already said what the word was
   for, and wrong on a console, where an unrecognised trailing word is far likelier a mistyped
   command than a scope. */
static inline bool iotdata_node_status_scope_is_name(const char *const s) {
    return s != NULL && iotdata_node_status_scope_from_name(s) != 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *iotdata_node_status_scope_name(const uint8_t scope, char *const out, const size_t size) {
    if (out == NULL || size == 0)
        return "";
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = {
        { IOTDATA_NODE_STATUS_SCOPE_NODE, "node" },       { IOTDATA_NODE_STATUS_SCOPE_MESH, "mesh" },        { IOTDATA_NODE_STATUS_SCOPE_STATIONS, "stations" },
        { IOTDATA_NODE_STATUS_SCOPE_FILTERS, "filters" }, { IOTDATA_NODE_STATUS_SCOPE_MESH_PEERS, "peers" }, { IOTDATA_NODE_STATUS_SCOPE_CONTENT, "content" },
    };
    const uint8_t want = (scope == 0) ? (uint8_t)IOTDATA_NODE_STATUS_SCOPE_DEFAULT : scope;
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if ((want & names[i].bit) != 0)
            n += (size_t)snprintf(out + n, n < size ? size - n : 0, "%s%s", n > 0 ? "," : "", names[i].name);
    if (n == 0)
        (void)snprintf(out, size, "none");
    return out;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// PACKAGING
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_status_pack_node(iotdata_kvr_t *const kv, const iotdata_node_status_t *const s) {
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_UPTIME, s->uptime_s);
    if (s->has_lifetime)
        iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_LIFETIME, s->lifetime_s);
    if (s->has_restarts)
        iotdata_kvr_add_u16(kv, IOTDATA_NODE_STATUS_RESTARTS, s->restarts);
    iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_REASON, s->reason);
    if (s->has_temperature)
        iotdata_kvr_add_i8(kv, IOTDATA_NODE_STATUS_TEMPERATURE, s->temperature);
    if (s->has_supply)
        iotdata_kvr_add_u16(kv, IOTDATA_NODE_STATUS_SUPPLY, s->supply_mv);
    if (s->has_heap) {
        iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_HEAP_FREE, s->heap_free);
        iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_HEAP_MIN, s->heap_min);
    }
    if (s->has_active)
        iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_ACTIVE, s->active_s);
    /* The table sizes are scalars and go out with the rest of them, BEFORE any row. A report whose
       tail is lost then still says how big each table was, which is the half worth keeping. */
    if (s->has_stations)
        iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_STATIONS_COUNT, s->stations);
    if (s->has_filters)
        iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_FILTERS_COUNT, s->filters);
    if (s->has_content)
        iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_CONTENT_COUNT, s->content);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_status_pack_mesh(iotdata_kvr_t *const kv, const iotdata_node_status_mesh_t *const m) {
    if (!m->present)
        return;
    iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_MESH_STATE, m->state);
    iotdata_kvr_add_u16(kv, IOTDATA_NODE_STATUS_MESH_PARENT, m->parent);
    iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_MESH_COST, m->cost);
    iotdata_kvr_add_u16(kv, IOTDATA_NODE_STATUS_MESH_GENERATION, m->generation);
    iotdata_kvr_add_i8(kv, IOTDATA_NODE_STATUS_MESH_PARENT_RSSI, m->parent_rssi);
    iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_MESH_ACCEPTING, m->accepting ? 1u : 0u);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_BEACON_RX, m->beacon_rx);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_BEACON_TX, m->beacon_tx);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_PEER_NEW, m->peer_new);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_REPARENT, m->reparent);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_FAILOVER, m->failover);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_ORPHAN, m->orphan);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_RERR_RX, m->rerr_rx);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_RERR_TX, m->rerr_tx);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_FORWARDS, m->forwards);
    iotdata_kvr_add_u32(kv, IOTDATA_NODE_STATUS_MESH_DUPLICATES, m->duplicates);
    /* a scalar like the others, for the same reason the node's table sizes are */
    iotdata_kvr_add_u8(kv, IOTDATA_NODE_STATUS_MESH_PEERS_COUNT, m->peers);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_status_pack(iotdata_kvr_t *const kv, const iotdata_node_status_t *const s, const uint8_t scope) {
    if (kv == NULL || s == NULL)
        return -1;
    if (iotdata_node_status_scope_wants(scope, IOTDATA_NODE_STATUS_SCOPE_NODE))
        _iotdata_node_status_pack_node(kv, s);
    if (iotdata_node_status_scope_wants(scope, IOTDATA_NODE_STATUS_SCOPE_MESH))
        _iotdata_node_status_pack_mesh(kv, &s->mesh);
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// RENDERING
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_STATUS_STR_MAX 200

static inline const char *iotdata_node_status_str(const iotdata_node_status_t *const s, char *const out, const size_t size) {
    if (out == NULL || size == 0)
        return "";
    if (s == NULL) {
        out[0] = '\0';
        return out;
    }
    int n = snprintf(out, size, "up=%us reason=%s", (unsigned)s->uptime_s, iotdata_node_tlv_status_reason_str(s->reason));
    if (n < 0)
        n = 0;
    size_t at = ((size_t)n < size) ? (size_t)n : size;
#define _STATUS_APPEND(...) \
    do { \
        const int _w = snprintf(out + at, size - at, __VA_ARGS__); \
        if (_w > 0) \
            at += ((size_t)_w < size - at) ? (size_t)_w : size - at - 1u; \
    } while (0)
    if (s->has_restarts)
        _STATUS_APPEND(" restarts=%u", (unsigned)s->restarts);
    if (s->has_lifetime)
        _STATUS_APPEND(" lifetime=%us", (unsigned)s->lifetime_s);
    if (s->has_active)
        _STATUS_APPEND(" active=%us", (unsigned)s->active_s);
    if (s->has_supply)
        _STATUS_APPEND(" supply=%umV", (unsigned)s->supply_mv);
    if (s->has_temperature)
        _STATUS_APPEND(" temp=%dC", (int)s->temperature);
    if (s->has_heap)
        _STATUS_APPEND(" heap=%u/%u", (unsigned)s->heap_free, (unsigned)s->heap_min);
    if (s->mesh.present) {
        _STATUS_APPEND(" mesh=%s", iotdata_node_tlv_status_mesh_state_str(s->mesh.state));
        if (s->mesh.state == IOTDATA_NODE_STATUS_MESH_STATE_JOINED)
            _STATUS_APPEND(" parent=%04X@%ddBm", (unsigned)s->mesh.parent, (int)s->mesh.parent_rssi);
        if (s->mesh.state == IOTDATA_NODE_STATUS_MESH_STATE_JOINED || s->mesh.state == IOTDATA_NODE_STATUS_MESH_STATE_GATEWAY)
            _STATUS_APPEND(" cost=%u", (unsigned)s->mesh.cost);
        _STATUS_APPEND(" peers=%u", (unsigned)s->mesh.peers);
    }
#undef _STATUS_APPEND
    return out;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: JSON
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

static inline bool iotdata_node_status_json_key(cJSON *const obj, const char *const name, const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    if (vlen != 1)
        return false; /* both are u8; anything else is malformed, and the generic path can say so */
    switch (key) {
    case IOTDATA_NODE_STATUS_REASON:
        cJSON_AddStringToObject(obj, name, iotdata_node_tlv_status_reason_str(val[0]));
        return true;
    case IOTDATA_NODE_STATUS_MESH_STATE:
        cJSON_AddStringToObject(obj, name, iotdata_node_tlv_status_mesh_state_str(val[0]));
        return true;
    case IOTDATA_NODE_STATUS_MESH_ACCEPTING:
        cJSON_AddBoolToObject(obj, name, val[0] != 0);
        return true;
    default:
        return false;
    }
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_STATUS_H */
