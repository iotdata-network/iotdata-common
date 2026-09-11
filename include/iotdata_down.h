
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
// FRAMES LIVE IN A POOL (d_module_buffers.h), which a caller supplies -- a slot holds a REFERENCE,
// not a copy. That is what lets a relay deliver a held frame by handing the handle to its transmit
// queue instead of copying the bytes back out, and it puts held frames into the same accounting as
// every other frame, where a full table is visible rather than invisible.
//
// The consequence to size for: this is a LONG-TERM holder. A slot can keep its reference for days,
// deliberately, so those buffers are not available to the receive and transmit paths for that
// whole time. A pool shared with them must be dimensioned for in-flight frames PLUS the slots
// here, or a table full of commands for sleeping nodes starves the traffic that would wake them.
//
// This is also why the header now needs `device/d_module_buffers.h`, making it the first thing in
// iotdata-common/include to depend on the device layer below it. That is a deliberate layering
// step: holding a frame for a week is a memory-management question, not a protocol one, and the
// alternative was a private 240-byte array per slot that no accounting could see.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_DOWN_SLOTS
#define IOTDATA_DOWN_SLOTS 8
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool valid;
    uint16_t target;
    uint32_t age;          /* insertion order; the lowest is the oldest, for FIFO eviction */
    uint32_t sends;        /* how many times we have put it on the air */
    buffer_handle_t frame; /* a reference the slot owns, released when the slot is given up */
} iotdata_down_slot_t;

typedef struct {
    buffer_pool_t *pool;
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

static inline void iotdata_down_init(iotdata_down_t *const ds, buffer_pool_t *const pool) {
    memset(ds, 0, sizeof(*ds));
    ds->pool = pool;
}

/* How long the frame in a slot is. Read from the pool rather than kept alongside, so there is no
   second copy of the length to fall out of step with the buffer. */
static inline uint16_t iotdata_down_len(const iotdata_down_t *const ds, const int i) {
    return buffer_len(ds->pool, ds->slot[i].frame);
}

/* Give up a slot and the reference it holds. Every exit that invalidates a slot goes through here:
   a missed release is a buffer leaked for the life of the process. */
static inline void iotdata_down_release(iotdata_down_t *const ds, const int i) {
    iotdata_down_slot_t *const e = &ds->slot[i];
    if (!e->valid)
        return;
    buffer_unref(ds->pool, e->frame);
    e->frame = (buffer_handle_t)BUFFER_NONE;
    e->valid = false;
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

/* Offer a frame held in a POOLED buffer. The slot takes a reference of its own when it keeps the
   frame, so the caller's reference remains the caller's to release either way -- exactly as with
   the transmit queue. */
static inline iotdata_down_ev_t iotdata_down_offer(iotdata_down_t *const ds, const uint16_t target, const buffer_handle_t frame) {
    /* nobody transmits as the broadcast id, so there is no arrival that could ever deliver it */
    if (target == IOTDATA_STATION_BROADCAST)
        return IOTDATA_DOWN_UNHOLDABLE;
    const uint16_t len = buffer_valid(ds->pool, frame) ? buffer_len(ds->pool, frame) : 0u;
    if (len == 0) {
        ds->stat_toobig++; /* nothing to hold: an empty or invalid frame */
        return IOTDATA_DOWN_UNHOLDABLE;
    }

    int i = iotdata_down_locate(ds, target);
    if (i >= 0) {
        iotdata_down_slot_t *e = &ds->slot[i];
        /* the slot IS the dedup state: same bytes means this is an echo of what we already sent.
           The same BUFFER is trivially the same bytes, and is what a caller re-offering the frame
           it is already holding will present. */
        if (e->frame == frame || (iotdata_down_len(ds, i) == len && memcmp(buffer_data(ds->pool, e->frame), buffer_data(ds->pool, frame), len) == 0)) {
            ds->stat_echo++;
            return IOTDATA_DOWN_ECHO;
        }
        buffer_unref(ds->pool, e->frame); /* the superseded frame goes back */
        e->frame = buffer_ref(ds->pool, frame);
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
        iotdata_down_release(ds, slot); /* the evicted frame's reference goes with it */
        ds->count--;
    }
    iotdata_down_slot_t *e = &ds->slot[slot];
    e->valid = true;
    e->target = target;
    e->age = ds->age_next++;
    e->sends = 0;
    e->frame = buffer_ref(ds->pool, frame);
    ds->count++;
    ds->stat_stored++;
    return IOTDATA_DOWN_STORED;
}

/* The frame to send, as a handle. BORROWED: the slot keeps its reference, because delivery does
   not clear the slot (there is no ack, so a lost send has to be retried on the next window). A
   caller that needs to keep it beyond the call -- queueing it for transmit, say -- takes its own
   reference, which is what buffer_queue_add does. Returns BUFFER_NONE when nothing is held. */
static inline buffer_handle_t iotdata_down_deliver(iotdata_down_t *const ds, const uint16_t target, size_t *const len_out) {
    const int i = iotdata_down_locate(ds, target);
    if (i < 0)
        return (buffer_handle_t)BUFFER_NONE;
    iotdata_down_slot_t *e = &ds->slot[i];
    e->sends++;
    ds->stat_delivered++;
    if (len_out != NULL)
        *len_out = iotdata_down_len(ds, i);
    return e->frame;
}

/* Drop what we hold for a target: for an operator command, not for delivery. */
static inline bool iotdata_down_drop(iotdata_down_t *const ds, const uint16_t target) {
    const int i = iotdata_down_locate(ds, target);
    if (i < 0)
        return false;
    iotdata_down_release(ds, i);
    ds->count--;
    return true;
}

static inline int iotdata_down_clear(iotdata_down_t *const ds) {
    const int n = ds->count;
    for (int i = 0; i < IOTDATA_DOWN_SLOTS && ds->count > 0; i++)
        if (ds->slot[i].valid) {
            iotdata_down_release(ds, i);
            ds->count--;
        }
    return n;
}

#endif /* IOTDATA_DOWN_H */
