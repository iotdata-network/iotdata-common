
#ifndef IOTDATA_NODE_STATIONS_H
#define IOTDATA_NODE_STATIONS_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_stations.h - what this node knows about the OTHER stations, for any node that hears
// more than one. Two tables, because they answer two questions:
//
//   FILTER     who we will accept a frame from. Policy, set by an operator.
//   STATIONS   who we have actually heard, of what kind, how strongly, how recently. Observation.
//
// Neither is mesh-specific and neither belongs to one application: a gateway with no mesh filters
// and records exactly as a relay does, and a plain sensor blocking downstream frames from an origin
// would use the first of them alone. (The stations half lived in the relay until it turned out the
// mesh module wanted it too, which was the point at which it stopped being the relay's.)
//
// DELIBERATELY NOT THE MESH PEER TABLE. That one is routing-critical and fixed-size -- it decides a
// parent -- so sensor traffic must never age-evict the relay you route through. The sets overlap
// and neither contains the other: a sensor is in here and never a peer.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef FILTER_MAX
#define FILTER_MAX 16
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    FILTER_BLOCK = 0,
    FILTER_ALLOW = 1
} filter_action_t;
typedef enum {
    FILTER_MANUAL = 0,
    FILTER_AUTO = 1
} filter_source_t;
typedef enum {
    FILTER_SCOPE_ALL = 0,
    FILTER_SCOPE_MANUAL = 1,
    FILTER_SCOPE_AUTO = 2
} filter_scope_t;

typedef struct {
    uint16_t station;
    filter_action_t action;
    filter_source_t source;
} filter_entry_t;

typedef struct {
    filter_entry_t e[FILTER_MAX];
    int count;
    uint32_t stat_blocked;
} filter_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline int filter_count(const filter_t *const f) {
    return f->count;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int filter_locate(const filter_t *const f, const uint16_t station) {
    for (int i = 0; i < f->count; i++)
        if (f->e[i].station == station)
            return i;
    return -1;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filter_insert(filter_t *const f, const uint16_t station, const filter_action_t action, const filter_source_t source) {
    int slot = -1;
    for (int i = 0; i < f->count; i++)
        if (f->e[i].station == station) {
            slot = i;
            break;
        }
    if (slot < 0) {
        if (f->count == (int)(sizeof(f->e) / sizeof(f->e[0])))
            return false;
        slot = f->count++;
    }
    filter_entry_t *const e = &f->e[slot];
    e->station = station;
    e->action = action;
    e->source = source;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filter_remove(filter_t *const f, const uint16_t station) {
    for (int i = 0; i < f->count; i++)
        if (f->e[i].station == station) {
            f->e[i] = f->e[f->count - 1];
            f->count--;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int filter_clear(filter_t *const f, const filter_scope_t scope) {
    int n = 0;
    for (int i = 0; i < f->count;) {
        const filter_entry_t *const e = &f->e[i];
        if (scope == FILTER_SCOPE_ALL || (scope == FILTER_SCOPE_MANUAL && e->source == FILTER_MANUAL) || (scope == FILTER_SCOPE_AUTO && e->source == FILTER_AUTO)) {
            f->e[i] = f->e[f->count - 1];
            f->count--;
            n++;
        } else
            i++;
    }
    return n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filter_allows(const filter_t *const f, const uint16_t station) {
    bool any_allow = false, is_allowed = false;
    for (int i = 0; i < f->count; i++) {
        const filter_entry_t *const e = &f->e[i];
        if (e->action == FILTER_ALLOW) {
            any_allow = true;
            if (e->station == station)
                is_allowed = true;
        } else if (e->station == station)
            return false; /* BLOCK wins */
    }
    return !any_allow || is_allowed;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline void filter_init(filter_t *const f) {
    *f = (filter_t){ 0 };
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------
// STATIONS HEARD
//
// KIND IS THE CALLER'S TO DECIDE, all of it, through stations_note_kind(). A variant 0-14 frame is
// a SENSOR; a mesh BEACON at cost 0 is a GATEWAY, at cost >0 a RELAY -- all three are readings of
// a frame this table never sees, and it used to infer the first of them from the variant while
// leaving the other two to the caller. One decision made in two places is one place too many, and
// it was also the only thing making this otherwise protocol-agnostic table depend on what value
// the mesh happens to use for its variant.
//
// A kind only ever moves away from UNKNOWN: note_kind ignores an UNKNOWN, so a caller that has not
// worked out what something is cannot un-say what an earlier one did.
// ------------------------------------------------------------------------------------------------------------------------

#ifndef STATIONS_MAX
#define STATIONS_MAX 24
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    STATION_KIND_UNKNOWN = 0,
    STATION_KIND_GATEWAY,
    STATION_KIND_RELAY,
    STATION_KIND_SENSOR,
} station_kind_t;

typedef struct {
    uint16_t station;
    station_kind_t kind;
    uint8_t variant; /* last variant seen */
    int rssi;        /* last RSSI, dBm */
    uint32_t last_ms;
    uint32_t rx_count;
} station_entry_t;

typedef struct {
    station_entry_t s[STATIONS_MAX];
    int count;
} stations_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline const char *station_kind_name(const station_kind_t k) {
    switch (k) {
    case STATION_KIND_GATEWAY:
        return "GATE";
    case STATION_KIND_RELAY:
        return "RLAY";
    case STATION_KIND_SENSOR:
        return "SENS";
    default:
        return "?";
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline int stations_count(const stations_t *const t) {
    return t->count;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int stations_locate(const stations_t *const t, const uint16_t station) {
    for (int i = 0; i < t->count; i++)
        if (t->s[i].station == station)
            return i;
    return -1;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline bool stations_seen(stations_t *const t, const uint16_t station, const uint8_t variant, const int rssi, const uint32_t now_ms) {
    int slot = -1, slot_oldest = 0;
    for (int i = 0; i < t->count; i++) {
        if (t->s[i].station == station) {
            slot = i;
            break;
        }
        if (t->s[i].last_ms < t->s[slot_oldest].last_ms)
            slot_oldest = i;
    }
    const bool is_new = (slot < 0);
    if (is_new) {
        slot = (t->count < STATIONS_MAX) ? t->count++ : slot_oldest;
        station_entry_t *const e = &t->s[slot];
        e->station = station;
        e->kind = STATION_KIND_UNKNOWN;
        e->rx_count = 0;
    }
    station_entry_t *const e = &t->s[slot];
    e->variant = variant;
    e->rssi = rssi;
    e->last_ms = now_ms;
    e->rx_count++;
    return is_new;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void stations_note_kind(stations_t *const t, const uint16_t station, const station_kind_t kind) {
    const int i = stations_locate(t, station);
    if (i >= 0 && kind != STATION_KIND_UNKNOWN) {
        station_entry_t *const e = &t->s[i];
        e->kind = kind;
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline void stations_clear(stations_t *const t) {
    t->count = 0;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void stations_init(stations_t *const t) {
    *t = (stations_t){ 0 };
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_STATIONS_H */
