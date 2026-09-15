
#ifndef IOTDATA_NODE_STATIONS_H
#define IOTDATA_NODE_STATIONS_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_stations.h - generic per-station RX allow/block filter (mesh AND sensor traffic).
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef FILTER_MAX
#define FILTER_MAX 16
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef enum { FILTER_BLOCK = 0, FILTER_ALLOW = 1 } filter_action_t;
typedef enum { FILTER_MANUAL = 0, FILTER_AUTO = 1 } filter_source_t;
typedef enum { FILTER_SCOPE_ALL = 0, FILTER_SCOPE_MANUAL = 1, FILTER_SCOPE_AUTO = 2 } filter_scope_t;

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
            memset(&f->e[f->count - 1], 0, sizeof(f->e[0]));
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
            memset(&f->e[f->count - 1], 0, sizeof(f->e[0]));
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
    memset(f, 0, sizeof(*f));
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_STATIONS_H */
