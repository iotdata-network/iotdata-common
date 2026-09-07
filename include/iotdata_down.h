
#ifndef IOTDATA_DOWN_H
#define IOTDATA_DOWN_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_down.h - holding a down frame for a node that is not listening yet.
//
// A down frame (sequence == IOTDATA_SEQUENCE_DOWN, station == the target) reaches a
// node that happens to be awake when it is sent, and no other. Most nodes are asleep most of the
// time, so a gateway or relay HOLDS one frame per target and sends it again when that target says
// it is listening. Both do the same thing with it, so both use this.
//
// ONE SLOT PER TARGET, AND THE SLOT IS THE DEDUP STATE. An arriving frame is compared against the
// one already held for that target:
//
//   nothing held   -> store it, and transmit (this is the first anyone has seen of it)
//   identical      -> IGNORE. It is our own rebroadcast coming back, or a neighbour's. Not
//                     transmitting is what stops the flood: each node transmits each distinct
//                     frame at most once, so propagation is bounded by the number of nodes with
//                     no hop count needed -- which is just as well, since the header has no room
//                     for one.
//   different      -> a newer command supersedes the old one. Replace it, and transmit.
//
// That single comparison is both the loop-breaker and the supersede rule. It never looks INSIDE
// the frame, so a relay holding traffic for a sensor needs to know nothing about what it says.
//
// NEVER EVICTED ON TIME, only oldest-first when full. A sensor buried under snow for a week still
// collects its pending request on the day it comes back, which is exactly when it is wanted. That
// is only safe because commands are expected to be IDEMPOTENT and state-relative rather than
// imperative -- "clear diagnostics up to record N", not "clear diagnostics" -- so a stale one that
// finally lands is a no-op. There is no acknowledgement anywhere in this path; idempotence is what
// replaces it. See the DOWN notes in iotdata_node.h.
//
// DELIVERY is on RECEIVE: the target says it is listening, in its own outbound frame, and whoever
// holds something for it sends it then. We do not clear the slot on sending, because a send that
// collides with another holder's is lost silently and there is no ack to notice. Duplicates are
// harmless by construction, so holding on and trying again on the next window costs nothing and
// recovers the collision.
//
// BROADCAST CANNOT BE HELD. No station ever transmits as IOTDATA_STATION_BROADCAST, so there is no
// arrival that would trigger delivery. A broadcast down is transmitted once and dropped.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_DOWN_SLOTS
#define IOTDATA_DOWN_SLOTS 8
#endif
#ifndef IOTDATA_DOWN_FRAME_MAX
#define IOTDATA_DOWN_FRAME_MAX 240
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool valid;
    uint16_t target;
    uint16_t len;
    uint32_t age;   /* insertion order; the lowest is the oldest, for FIFO eviction */
    uint32_t sends; /* how many times we have put it on the air */
    uint8_t frame[IOTDATA_DOWN_FRAME_MAX];
} iotdata_down_slot_t;

typedef struct {
    iotdata_down_slot_t slot[IOTDATA_DOWN_SLOTS];
    int count; /* valid slots -- kept in sync so scans can short-circuit */
    uint32_t age_next;
    uint32_t stat_stored, stat_superseded, stat_echo, stat_evicted, stat_delivered, stat_toobig;
} iotdata_down_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

/* What offering a frame to the table decided. TRANSMIT tells the caller to put it on the air. */
typedef enum {
    IOTDATA_DOWN_STORED = 0, /* new to us: hold it, and transmit */
    IOTDATA_DOWN_SUPERSEDED, /* replaced an older command for the same target: transmit */
    IOTDATA_DOWN_ECHO,       /* we already hold exactly this: do NOT transmit */
    IOTDATA_DOWN_UNHOLDABLE, /* broadcast, or too big to hold: transmit, do not hold */
} iotdata_down_ev_t;

static inline bool iotdata_down_transmit(const iotdata_down_ev_t ev) {
    return ev != IOTDATA_DOWN_ECHO;
}

static inline const char *iotdata_down_ev_name(const iotdata_down_ev_t ev) {
    switch (ev) {
    case IOTDATA_DOWN_STORED:
        return "stored";
    case IOTDATA_DOWN_SUPERSEDED:
        return "superseded";
    case IOTDATA_DOWN_ECHO:
        return "echo";
    default:
        return "unholdable";
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_down_init(iotdata_down_t *const ds) {
    memset(ds, 0, sizeof(*ds));
}

static inline int iotdata_down_locate(const iotdata_down_t *const ds, const uint16_t target) {
    for (int i = 0, c = 0; i < IOTDATA_DOWN_SLOTS && c < ds->count; i++) {
        const iotdata_down_slot_t *const e = &ds->slot[i];
        if (e->valid) {
            if (e->target == target)
                return i;
            c++;
        }
    }
    return -1;
}

static inline bool iotdata_down_holds(const iotdata_down_t *const ds, const uint16_t target) {
    return iotdata_down_locate(ds, target) >= 0;
}

static inline int iotdata_down_oldest(const iotdata_down_t *const ds) {
    int oldest = -1;
    for (int i = 0; i < IOTDATA_DOWN_SLOTS; i++) {
        const iotdata_down_slot_t *const e = &ds->slot[i];
        if (e->valid && (oldest < 0 || e->age < ds->slot[oldest].age))
            oldest = i;
    }
    return oldest;
}

static inline iotdata_down_ev_t iotdata_down_offer(iotdata_down_t *const ds, const uint16_t target, const uint8_t *const frame, const size_t len) {
    /* nobody transmits as the broadcast id, so there is no arrival that could ever deliver it */
    if (target == IOTDATA_STATION_BROADCAST)
        return IOTDATA_DOWN_UNHOLDABLE;
    if (len == 0 || len > IOTDATA_DOWN_FRAME_MAX) {
        ds->stat_toobig++;
        return IOTDATA_DOWN_UNHOLDABLE;
    }

    int i = iotdata_down_locate(ds, target);
    if (i >= 0) {
        iotdata_down_slot_t *e = &ds->slot[i];
        /* the slot IS the dedup state: same bytes means this is an echo of what we already sent */
        if (e->len == (uint16_t)len && memcmp(e->frame, frame, len) == 0) {
            ds->stat_echo++;
            return IOTDATA_DOWN_ECHO;
        }
        memcpy(e->frame, frame, len);
        e->len = (uint16_t)len;
        e->sends = 0;
        ds->stat_superseded++;
        return IOTDATA_DOWN_SUPERSEDED;
    }

    /* UPSERT: find a free slot, else evict the oldest. Scanning for !valid means no count guard --
       a free slot is exactly what the count does not point at. */
    int slot = -1;
    for (int j = 0; j < IOTDATA_DOWN_SLOTS; j++)
        if (!ds->slot[j].valid) {
            slot = j;
            break;
        }
    if (slot < 0) {
        slot = iotdata_down_oldest(ds);
        ds->stat_evicted++;
        ds->count--; /* about to be overwritten */
    }
    iotdata_down_slot_t *e = &ds->slot[slot];
    e->valid = true;
    e->target = target;
    e->len = (uint16_t)len;
    e->age = ds->age_next++;
    e->sends = 0;
    memcpy(e->frame, frame, len);
    ds->count++;
    ds->stat_stored++;
    return IOTDATA_DOWN_STORED;
}

static inline const uint8_t *iotdata_down_deliver(iotdata_down_t *const ds, const uint16_t target, size_t *const len_out) {
    const int i = iotdata_down_locate(ds, target);
    if (i < 0)
        return NULL;
    iotdata_down_slot_t *e = &ds->slot[i];
    e->sends++;
    ds->stat_delivered++;
    if (len_out != NULL)
        *len_out = e->len;
    return e->frame;
}

/* Drop what we hold for a target: for an operator command, not for delivery. */
static inline bool iotdata_down_drop(iotdata_down_t *const ds, const uint16_t target) {
    const int i = iotdata_down_locate(ds, target);
    if (i < 0)
        return false;
    iotdata_down_slot_t *e = &ds->slot[i];
    e->valid = false;
    ds->count--;
    return true;
}

static inline int iotdata_down_clear(iotdata_down_t *const ds) {
    const int n = ds->count;
    for (int i = 0; i < IOTDATA_DOWN_SLOTS && ds->count > 0; i++) {
        iotdata_down_slot_t *e = &ds->slot[i];
        if (e->valid) {
            e->valid = false;
            ds->count--;
        }
    }
    return n;
}

#endif /* IOTDATA_DOWN_H */
