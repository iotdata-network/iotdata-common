
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
// EVICTED OLDEST-FIRST WHEN FULL, AND ON A LONG EXPIRY. A sensor buried under snow for a week
// still collects its pending request on the day it comes back, which is exactly when it is
// wanted -- so the expiry is measured in days, not minutes, and holding is the rule rather than
// the exception.
//
// It is not unbounded though, and the reason is worth being precise about. Commands are expected
// to be IDEMPOTENT and state-relative rather than imperative -- "clear diagnostics up to record
// N", not "clear diagnostics" -- so a stale one that finally lands is a no-op, and that is what
// replaces the acknowledgement this path does not have. But idempotent means harmless to the
// PROTOCOL, not wanted by the OPERATOR: a node that wakes after two months and is handed a
// factory reset queued in a different season is obeying an instruction nobody still means. The
// expiry bounds that. A week by default -- long enough that a genuinely slow node is still
// served, short enough that nobody is surprised by what arrives.
//
// EXPIRY IS DRIVEN BY A TICK, not by offering or delivering, exactly as it is for the transmit
// queue: iotdata_down_tick() is called from the caller's loop. A table that is never ticked never
// expires anything, so a caller that forgets it silently loses the bound.
//
// The tick is called every cycle and SCANS far less often. Against an expiry measured in days,
// per-cycle scanning buys resolution nobody can use and costs a walk of the table on every pass
// of the loop, so the tick gates itself: one comparison per cycle, a scan every scan interval.
// The interval is the resolution of the expiry -- a command can outlive its welcome by up to one
// interval -- which is why it is tuned against the TTL and not against the loop. An interval that
// is a large fraction of the TTL makes the TTL meaningless; the default pair (15s against a week)
// is five orders of magnitude apart.
//
// See the DOWN notes in iotdata_node.h.
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

/* How long a held command stays worth delivering. The compile-time value is a STARTING point:
   firmware boots with it and calls iotdata_down_set_ttl_ms() once its own configuration is
   loaded, so the default only has to be a sane one rather than the right one for every site. */
#ifndef IOTDATA_DOWN_TTL_MS_DEFAULT
#define IOTDATA_DOWN_TTL_MS_DEFAULT (7UL * 24UL * 60UL * 60UL * 1000UL) /* one week */
#endif

/* The ceiling, and it is not arbitrary. Times here are uint32 MILLISECONDS, to match the transmit
   queue and the loop's clock, and that clock wraps every 49.7 days; the wrap-safe comparison in
   iotdata_down_tick() can only tell "before" from "after" across less than half of that range. So
   an interval has to stay under ~24.8 days or the comparison starts reading backwards. 24 days
   leaves the margin. A site wanting longer needs a seconds-resolution clock, not a bigger
   constant. */
#define IOTDATA_DOWN_TTL_MS_MAX (24UL * 24UL * 60UL * 60UL * 1000UL)

#if IOTDATA_DOWN_TTL_MS_DEFAULT > IOTDATA_DOWN_TTL_MS_MAX
#error "IOTDATA_DOWN_TTL_MS_DEFAULT exceeds what a wrapping 32-bit millisecond clock can order"
#endif

/* How often the tick actually scans. 15s against a week-long expiry is 0.002% slop -- far finer
   than anything the expiry is trying to decide -- while turning a table walk per loop cycle into
   a single comparison. Retunable at runtime for the same reason the TTL is; 0 scans every call,
   which is what a test wanting to drive the expiry directly asks for. */
#ifndef IOTDATA_DOWN_SCAN_MS_DEFAULT
#define IOTDATA_DOWN_SCAN_MS_DEFAULT (15UL * 1000UL)
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool valid;
    uint16_t target;
    uint32_t age;          /* insertion order; the lowest is the oldest, for FIFO eviction */
    uint32_t sends;        /* how many times we have put it on the air */
    uint32_t stored_ms;    /* when this COMMAND arrived, which is what the expiry measures */
    buffer_handle_t frame; /* a reference the slot owns, released when the slot is given up */
} iotdata_down_slot_t;

typedef struct {
    buffer_pool_t *pool;
    iotdata_down_slot_t slot[IOTDATA_DOWN_SLOTS];
    int count;
    uint32_t age_next;
    uint32_t ttl_ms;       /* 0 = hold forever; set by iotdata_down_set_ttl_ms() */
    uint32_t scan_ms;      /* how often the tick scans; 0 = every call */
    uint32_t scan_last_ms; /* when it last did */
    uint32_t stat_stored, stat_superseded, stat_echo, stat_evicted, stat_delivered, stat_toobig, stat_expired;
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
    ds->ttl_ms = IOTDATA_DOWN_TTL_MS_DEFAULT;
    ds->scan_ms = IOTDATA_DOWN_SCAN_MS_DEFAULT;
}

/* Retune the expiry at runtime -- the point being that firmware boots on the compile-time default
   and adopts the configured value once it has one (from NVS, a config file, an operator command)
   without the held table being rebuilt. Takes effect immediately, including on frames already
   held: their age is measured from when they ARRIVED, not from when the setting changed, so
   shortening the expiry can retire something on the very next tick. That is the intent.
   0 means hold forever. Anything above the wrap ceiling is clamped rather than refused, because
   the alternative is a setting that is silently ignored. */
static inline void iotdata_down_set_ttl_ms(iotdata_down_t *const ds, const uint32_t ttl_ms) {
    ds->ttl_ms = ttl_ms > (uint32_t)IOTDATA_DOWN_TTL_MS_MAX ? (uint32_t)IOTDATA_DOWN_TTL_MS_MAX : ttl_ms;
}

static inline uint32_t iotdata_down_ttl_ms(const iotdata_down_t *const ds) {
    return ds->ttl_ms;
}

/* Retune how often the tick scans. Keep it small against the TTL: it is the expiry's resolution,
   so a command can outlive its welcome by up to one interval. 0 scans on every call. */
static inline void iotdata_down_set_scan_ms(iotdata_down_t *const ds, const uint32_t scan_ms) {
    ds->scan_ms = scan_ms;
}

static inline uint32_t iotdata_down_scan_ms(const iotdata_down_t *const ds) {
    return ds->scan_ms;
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
    if (e->valid) {
        buffer_unref(ds->pool, e->frame);
        e->frame = BUFFER_NONE;
        e->valid = false;
    }
}

static inline int iotdata_down_locate(const iotdata_down_t *const ds, const uint16_t target) {
    for (int i = 0, c = 0; i < (int)(sizeof(ds->slot) / sizeof(ds->slot[0])) && c < ds->count; i++) {
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
    int slot_oldest = -1;
    for (int i = 0, c = 0; i < (int)(sizeof(ds->slot) / sizeof(ds->slot[0])) && c < ds->count; i++) {
        const iotdata_down_slot_t *const e = &ds->slot[i];
        if (e->valid) {
            c++;
            if (slot_oldest < 0 || e->age < ds->slot[slot_oldest].age)
                slot_oldest = i;
        }
    }
    return slot_oldest;
}

/* Offer a frame held in a POOLED buffer. The slot takes a reference of its own when it keeps the
   frame, so the caller's reference remains the caller's to release either way -- exactly as with
   the transmit queue. */
static inline iotdata_down_ev_t iotdata_down_offer(iotdata_down_t *const ds, const uint16_t target, const buffer_handle_t frame, const uint32_t now_ms) {
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
        e->stored_ms = now_ms; /* a NEW command, so its life starts now */
        ds->stat_superseded++;
        return IOTDATA_DOWN_SUPERSEDED;
    }

    /* UPSERT: find a free slot, else evict the oldest. Scanning for !valid means no count guard --
       a free slot is exactly what the count does not point at. */
    int slot = -1;
    for (int j = 0; j < (int)(sizeof(ds->slot) / sizeof(ds->slot[0])); j++)
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
    e->stored_ms = now_ms;
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
        return BUFFER_NONE;
    iotdata_down_slot_t *e = &ds->slot[i];
    e->sends++;
    ds->stat_delivered++;
    if (len_out != NULL)
        *len_out = iotdata_down_len(ds, i);
    return e->frame;
}

/* Retire anything held longer than the expiry. Driven from the caller's loop; see the note at the
   top about a table that is never ticked.
   NOTE what does NOT refresh a slot. An ECHO does not: our own rebroadcast coming back is not new
   evidence that anybody still wants the command, and refreshing on it would let a frame live
   forever in a mesh that keeps echoing it. Nor does DELIVERY: a command that has been sent has
   done its job, and holding past that only covers a send nobody could acknowledge. Only a
   genuinely new or superseding command restarts the clock. */
static inline int iotdata_down_tick(iotdata_down_t *const ds, const uint32_t now_ms) {
    if (ds->ttl_ms == 0)
        return 0; /* holding forever, deliberately */
    /* The cheap gate that lets this be called every cycle: one wrap-safe comparison, and the walk
       below only on the interval. scan_last_ms moves to NOW rather than by one interval, so a late
       call does not leave a backlog of scans to catch up on -- there is nothing to catch up, the
       state being aged is a timestamp and not a queue of events. */
    if (ds->scan_ms != 0) {
        if ((int32_t)(now_ms - (ds->scan_last_ms + ds->scan_ms)) < 0)
            return 0;
        ds->scan_last_ms = now_ms;
    }
    int dropped = 0;
    /* `c` counts valid slots we KEPT, so `c < ds->count` is exactly "there are still valid slots
       ahead of us": kept + released < the count we entered with. It cannot stop early while a
       stale slot remains, and on a sparse table of 128 it stops at the last valid one instead of
       walking the whole array every tick. Same idiom as iotdata_down_locate(). */
    for (int i = 0, c = 0; i < (int)(sizeof(ds->slot) / sizeof(ds->slot[0])) && c < ds->count; i++) {
        iotdata_down_slot_t *e = &ds->slot[i];
        if (e->valid) {
            /* wrap-safe: a signed difference orders two times across the clock's rollover, which a
            plain `now_ms > stored_ms + ttl_ms` cannot. Sound for intervals under half the clock's
            range, which is what IOTDATA_DOWN_TTL_MS_MAX guarantees. */
            if ((int32_t)(now_ms - (e->stored_ms + ds->ttl_ms)) >= 0) {
                iotdata_down_release(ds, i);
                ds->count--;
                ds->stat_expired++;
                dropped++;
            } else
                c++;
        }
    }
    return dropped;
}

/* How long a target's held command has been waiting, for a diagnostic that wants to say so. */
static inline uint32_t iotdata_down_age_ms(const iotdata_down_t *const ds, const uint16_t target, const uint32_t now_ms) {
    const int i = iotdata_down_locate(ds, target);
    return i < 0 ? 0u : (uint32_t)(now_ms - ds->slot[i].stored_ms);
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
    for (int i = 0; i < (int)(sizeof(ds->slot) / sizeof(ds->slot[0])) && ds->count > 0; i++)
        if (ds->slot[i].valid) {
            iotdata_down_release(ds, i);
            ds->count--;
        }
    return n;
}

#endif /* IOTDATA_DOWN_H */
