#ifndef D_MODULE_BUFFERS_H
#define D_MODULE_BUFFERS_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_buffers.h - a reference-counted frame pool, and a timed queue over it.
//
// Two different beasts in one header because they are always used together and neither is large:
// the POOL owns the bytes and counts references; the QUEUE owns *when* a frame goes out and holds
// a reference while it waits. Keeping them apart in the file, together in the module.
//
// WHY. A relay's dominant workload is a received frame becoming a transmitted one. Without a pool
// that is three copies of the same bytes -- into the receive buffer, into a transmit queue slot,
// and back out into a transmit buffer -- plus a fourth held for retransmit. With one, it is one
// buffer and three references.
//
// REFERENCE COUNTED, not handed over. A frame legitimately has several holders at once: the
// transmit queue needs it to send and the ACK tracker needs it to retransmit, simultaneously. An
// ownership-transfer model cannot express that, and it is the part that is painful to retrofit.
//
// RESERVED PREFIX. A buffer is longer than a frame, and a receive starts at an OFFSET into it, so
// a forwarding header can later be written into the space in front of the payload rather than the
// payload being moved. Dimension the stride as
//     max frame + 1 (the trailing RSSI byte) + the largest header you will prepend
// and the offset as that largest header.
//
// LOCKING IS OPTIONAL and the caller's to choose. By default there is none, which is what a
// cooperative loop wants and what an MCU should pay. Define the four BUFFER_LOCK_* macros before
// including to make a pool thread-safe -- pthread on a host, a FreeRTOS semaphore on an MCU:
//
//     #define BUFFER_LOCK_TYPE       pthread_mutex_t
//     #define BUFFER_LOCK_INIT(l)    pthread_mutex_init((l), NULL)
//     #define BUFFER_LOCK_ACQUIRE(l) pthread_mutex_lock(l)
//     #define BUFFER_LOCK_RELEASE(l) pthread_mutex_unlock(l)
//
// What the lock protects is the POOL'S OWN BOOKKEEPING -- the free map, the reference counts, the
// counters -- and nothing else. It does not protect buffer CONTENTS: two threads that share one
// buffer still need their own discipline about who writes when. The safe pattern across a thread
// is a HANDOFF: one side acquires and fills, passes the handle, and the other side releases.
//
// The QUEUE takes the same four macros and guards its own slots, so declaring them once makes both
// structures safe rather than leaving each caller to work out what needs a lock of its own. Two
// consequences worth knowing before relying on it:
//
//   - LOCK ORDER is queue then pool, always. A queue operation may take the pool lock underneath
//     (add references, expire and clear release); nothing in the pool ever reaches for a queue. Do
//     not invert it by calling into a queue from inside your own pool-holding critical section.
//   - Each CALL is atomic; a SEQUENCE of calls is not. "Remove by tag, then add" -- the supersede
//     idiom -- is two operations, and another thread can land between them. A caller that needs
//     that pair to be indivisible needs its own mutex around the pair; the queue's lock is not it.
//   - The scalar accessors (count, high_water, added, expired, rejected) and peek are deliberately
//     NOT locked. The counters are diagnostics and a moment's staleness costs nothing. Peek is
//     different: it hands back a handle the queue still owns, so on a queue another thread can
//     take from, the handle can go stale under you -- peek is for a queue you own, or for a
//     diagnostic, and take is the only safe way to get a frame off a shared one.
//
// NO FIELD ACCESS. Everything is a function or a macro. The struct is visible because this is a
// header-only library and the storage has to be declared somewhere, not because it is an interface.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define BUFFER_NONE 0xFFu /* not a handle: what acquire returns when the pool is empty */

/* No lock unless the caller asked for one. The placeholder keeps the struct layout and the call
   sites identical either way, so the locked and unlocked builds are the same code. */
#ifndef BUFFER_LOCK_TYPE
#define BUFFER_LOCK_TYPE       char
#define BUFFER_LOCK_INIT(l)    ((void)(l))
#define BUFFER_LOCK_ACQUIRE(l) ((void)(l))
#define BUFFER_LOCK_RELEASE(l) ((void)(l))
#endif

/* A handle is an INDEX, not a pointer: one byte, so a holder costs a byte rather than four or
   eight, and a stale one can be range-checked rather than merely hoped about. The cost is a
   ceiling of 255 buffers per pool, of which 0xFF is BUFFER_NONE -- so 254 usable. That is ample
   for frames in flight, but a pool sized for a large table of long-term holds can approach it:
   BUFFER_POOL_DECLARE fails the build rather than silently minting handles that collide with
   BUFFER_NONE. Widening this to uint16_t is the fix if a pool ever genuinely needs more. */
typedef uint8_t buffer_handle_t;
#define BUFFER_POOL_COUNT_MAX 254

/* Per-buffer bookkeeping. `at` is where the data currently starts, which moves BACKWARDS as
   headers are prepended; `len` is how many bytes of data there are from `at`. */
typedef struct {
    uint8_t refs;
    uint16_t at;
    uint16_t len;
} buffer_meta_t;

typedef struct {
    uint8_t *data; /* count * stride bytes */
    buffer_meta_t *meta;
    uint32_t *free_map; /* one bit per buffer, SET = free (so "find a free one" is a ctz) */
    uint16_t count;
    uint16_t stride;
    uint16_t offset; /* where a receive starts: the room reserved for prepending */
    /* stats, for the periodic report */
    uint16_t used;
    uint16_t high_water;
    uint32_t acquires;
    uint32_t fails; /* acquire on an empty pool -- a real condition, not an assertion */
    BUFFER_LOCK_TYPE lock;
} buffer_pool_t;

#define BUFFER_POOL_MAP_WORDS(count) (((count) + 31u) / 32u)

/* Declare the storage and the pool together, so a user dimensions it in one place and never
   touches a field. `name` is the pool; the arrays are private by convention. */
#define BUFFER_POOL_DECLARE(name, count, stride) \
    /* a handle is one byte and 0xFF is reserved, so the pool cannot exceed 254 buffers */ \
    typedef char name##_count_fits_[((count) >= 1 && (count) <= BUFFER_POOL_COUNT_MAX) ? 1 : -1]; \
    static uint8_t name##_data_[(count) * (stride)]; \
    static buffer_meta_t name##_meta_[(count)]; \
    static uint32_t name##_map_[BUFFER_POOL_MAP_WORDS(count)]; \
    static buffer_pool_t name

/* `offset` is the reserved prefix: how many bytes may later be prepended. */
#define BUFFER_POOL_INIT(name, count, stride, offset) buffer_pool_init(&(name), name##_data_, name##_meta_, name##_map_, (count), (stride), (offset))

// ------------------------------------------------------------------------------------------------------------------------

static inline void buffer_pool_init(buffer_pool_t *const p, uint8_t *const data, buffer_meta_t *const meta, uint32_t *const map, const uint16_t count, const uint16_t stride, const uint16_t offset) {
    p->data = data;
    p->meta = meta;
    p->free_map = map;
    p->count = count;
    p->stride = stride;
    p->offset = offset;
    p->used = 0;
    p->high_water = 0;
    p->acquires = 0;
    p->fails = 0;
    BUFFER_LOCK_INIT(&p->lock);
    memset(meta, 0, sizeof(buffer_meta_t) * count);
    memset(map, 0, sizeof(uint32_t) * BUFFER_POOL_MAP_WORDS(count));
    for (uint16_t i = 0; i < count; i++) /* every buffer starts free */
        map[i / 32u] |= (1u << (i % 32u));
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * Acquire a buffer, reference count 1.
 *
 * The free map is a bitmap rather than a list because allocation must not be a walk: the first
 * non-zero word gives the first free buffer in one count-trailing-zeros, whatever the pool size.
 */
static inline buffer_handle_t buffer_acquire(buffer_pool_t *const p) {
    BUFFER_LOCK_ACQUIRE(&p->lock);
    const uint16_t words = (uint16_t)BUFFER_POOL_MAP_WORDS(p->count);
    for (uint16_t w = 0; w < words; w++) {
        if (p->free_map[w] == 0u)
            continue;
        const uint16_t bit = (uint16_t)__builtin_ctz(p->free_map[w]);
        const uint16_t i = (uint16_t)(w * 32u + bit);
        if (i >= p->count)
            break; /* the tail of the last word is padding, not buffers */
        p->free_map[w] &= ~(1u << bit);
        p->meta[i].refs = 1;
        p->meta[i].at = p->offset; /* a receive lands past the reserved prefix */
        p->meta[i].len = 0;
        p->used++;
        p->acquires++;
        if (p->used > p->high_water)
            p->high_water = p->used;
        BUFFER_LOCK_RELEASE(&p->lock);
        return (buffer_handle_t)i;
    }
    p->fails++;
    BUFFER_LOCK_RELEASE(&p->lock);
    return (buffer_handle_t)BUFFER_NONE;
}

static inline bool buffer_valid(const buffer_pool_t *const p, const buffer_handle_t h) {
    return h != (buffer_handle_t)BUFFER_NONE && h < p->count && p->meta[h].refs > 0;
}

/* Take another reference. Returns the handle, so a holder can write `q = buffer_ref(p, h)`. */
static inline buffer_handle_t buffer_ref(buffer_pool_t *const p, const buffer_handle_t h) {
    BUFFER_LOCK_ACQUIRE(&p->lock);
    if (buffer_valid(p, h) && p->meta[h].refs < 0xFFu)
        p->meta[h].refs++;
    BUFFER_LOCK_RELEASE(&p->lock);
    return h;
}

/* Drop a reference; the buffer returns to the pool when the last one goes. */
static inline void buffer_unref(buffer_pool_t *const p, const buffer_handle_t h) {
    BUFFER_LOCK_ACQUIRE(&p->lock);
    /* the decrement and the return-to-pool are one step: a reader between them would see a
       buffer with no holders that is not yet free */
    if (buffer_valid(p, h) && --p->meta[h].refs == 0) {
        p->free_map[h / 32u] |= (1u << (h % 32u));
        p->used--;
    }
    BUFFER_LOCK_RELEASE(&p->lock);
}

static inline uint8_t buffer_refs(const buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? p->meta[h].refs : 0u;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The data, and how much of it there is. */
static inline uint8_t *buffer_data(buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? &p->data[(size_t)h * p->stride + p->meta[h].at] : NULL;
}
static inline uint16_t buffer_len(const buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? p->meta[h].len : 0u;
}
/* How many bytes can be written at buffer_data(): the stride less the prefix still reserved. */
static inline uint16_t buffer_room(const buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? (uint16_t)(p->stride - p->meta[h].at) : 0u;
}

static inline void buffer_set_len(buffer_pool_t *const p, const buffer_handle_t h, const uint16_t len) {
    if (buffer_valid(p, h))
        p->meta[h].len = (len > buffer_room(p, h)) ? buffer_room(p, h) : len;
}

/*
 * Write `n` bytes in FRONT of the data, consuming reserved prefix.
 *
 * This is the whole reason a receive starts at an offset: a forwarding header goes on without the
 * payload being moved. Fails rather than truncating if the prefix is already spent -- a frame that
 * cannot carry its header must not go out looking like one that can.
 *
 * The `n > at` guard is load-bearing and must not be "simplified": `at` is unsigned, so without it
 * an over-long prepend wraps to ~65535 and the memcpy lands far outside the pool. Dropping it does
 * not produce a wrong answer, it produces a segfault somewhere else entirely.
 */
/* Put a buffer you already hold back into the state acquire() would have given it: the payload
   start back at the reserved prefix, the length back to zero. For REUSE -- holding one buffer
   across cycles instead of acquiring and releasing one per cycle -- because skipping the acquire
   also skips the reset, so a buffer that was prepended into once would stay shifted forever.
   Only legal while you are the ONLY holder: moving the payload start under another holder is
   exactly the corruption reference counting exists to prevent, so this refuses rather than
   trusting the caller. */
static inline bool buffer_reset(buffer_pool_t *const p, const buffer_handle_t h) {
    if (!buffer_valid(p, h) || p->meta[h].refs != 1)
        return false;
    p->meta[h].at = p->offset;
    p->meta[h].len = 0;
    return true;
}

static inline bool buffer_prepend(buffer_pool_t *const p, const buffer_handle_t h, const void *const hdr, const uint16_t n) {
    if (!buffer_valid(p, h) || n > p->meta[h].at)
        return false;
    p->meta[h].at = (uint16_t)(p->meta[h].at - n);
    p->meta[h].len = (uint16_t)(p->meta[h].len + n);
    if (hdr != NULL)
        memcpy(&p->data[(size_t)h * p->stride + p->meta[h].at], hdr, n);
    return true;
}

/* Copy bytes into a buffer at its data start, setting the length. For a frame that was built
   elsewhere; a receive should read straight into buffer_data() instead. */
static inline bool buffer_write(buffer_pool_t *const p, const buffer_handle_t h, const void *const src, const uint16_t n) {
    if (!buffer_valid(p, h) || n > buffer_room(p, h))
        return false;
    memcpy(buffer_data(p, h), src, n);
    p->meta[h].len = n;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Stats, for the periodic report: in use of total, the worst it has ever been, and how often an
   acquire found nothing -- which is the number that says the pool is too small. */
static inline uint16_t buffer_pool_total(const buffer_pool_t *const p) {
    return p->count;
}
static inline uint16_t buffer_pool_used(const buffer_pool_t *const p) {
    return p->used;
}
static inline uint16_t buffer_pool_high_water(const buffer_pool_t *const p) {
    return p->high_water;
}
static inline uint32_t buffer_pool_fails(const buffer_pool_t *const p) {
    return p->fails;
}
static inline uint32_t buffer_pool_acquires(const buffer_pool_t *const p) {
    return p->acquires;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// The QUEUE: when a frame goes out, and how long it is still worth sending.
//
// A different beast from the pool. The pool owns bytes and counts holders; the queue owns TIME --
// a due moment and an expiry -- and is simply one of those holders. Entries carry a handle, never
// a copy, so queueing a received frame for forwarding costs a reference and no memcpy.
//
// The expiry is what makes back-pressure survivable. A radio that cannot accept a frame now is not
// an error: the caller leaves the frame queued and asks again next cycle, and only the expiry
// decides to give up. So the queue must be aged even on a cycle where nothing can be transmitted,
// which is why expiry is its own call and not a side effect of taking a frame.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define BUFFER_QUEUE_TAG_NONE 0xFFu

/* What a tag MEANS is the caller's vocabulary, but wanting to print one is generic -- every log
   line about a queued frame wants its name. So the queue carries an optional resolver rather than
   each caller formatting tags itself. */
typedef const char *(*buffer_tag_name_fn)(uint8_t tag);

typedef struct {
    buffer_handle_t buf;
    bool valid;
    uint8_t tag;        /* the caller's classification: what kind of frame this is */
    uint32_t key;       /* the caller's identity for it, for remove-by-key */
    uint32_t due_ms;    /* not before this */
    uint32_t expiry_ms; /* give up at this; 0 = never expires */
} buffer_queue_entry_t;

typedef struct {
    buffer_queue_entry_t *slot;
    buffer_pool_t *pool;
    uint16_t size; /* capacity */
    uint16_t used;
    uint16_t high_water; /* the deepest this queue has ever been: what the capacity is judged on */
    uint32_t st_added, st_expired, st_rejected;
    buffer_tag_name_fn tag_name; /* optional: NULL renders a tag as its number */
    BUFFER_LOCK_TYPE lock;
} buffer_queue_t;

#define BUFFER_QUEUE_DECLARE(name, capacity) \
    static buffer_queue_entry_t name##_slot_[(capacity)]; \
    static buffer_queue_t name

#define BUFFER_QUEUE_INIT(name, capacity, pool) buffer_queue_init(&(name), name##_slot_, (capacity), (pool))

static inline void buffer_queue_init(buffer_queue_t *const q, buffer_queue_entry_t *const slot, const uint16_t size, buffer_pool_t *const pool) {
    q->slot = slot;
    q->pool = pool;
    q->size = size;
    q->used = 0;
    q->high_water = 0;
    q->st_added = 0;
    q->st_expired = 0;
    q->st_rejected = 0;
    q->tag_name = NULL;
    BUFFER_LOCK_INIT(&q->lock);
    memset(slot, 0, sizeof(buffer_queue_entry_t) * size);
}

/* Optional, and deliberately separate from init: a queue works without it. */
static inline void buffer_queue_set_tag_name(buffer_queue_t *const q, const buffer_tag_name_fn fn) {
    q->tag_name = fn;
}

/* Never NULL, so a format string can use it without a guard. Falls back to the number, which is
   still more use than nothing when a caller has not named its tags. */
static inline const char *buffer_queue_tag_name(const buffer_queue_t *const q, const uint8_t tag) {
    static char fallback[8];
    if (q->tag_name != NULL) {
        const char *const n = q->tag_name(tag);
        if (n != NULL)
            return n;
    }
    (void)snprintf(fallback, sizeof(fallback), "tag%u", (unsigned)tag);
    return fallback;
}

static inline uint16_t buffer_queue_count(const buffer_queue_t *const q) {
    return q->used;
}

/*
 * Queue a frame, taking a reference to it.
 *
 * The caller keeps its own reference: it acquired the buffer and must still unref it. That is what
 * makes "queue it AND track it for retransmit" expressible -- two holders, one buffer.
 *
 * Rejects rather than evicting when full, and takes no reference when it does, so a failed add
 * leaves the caller owning exactly what it owned before.
 */
static inline bool buffer_queue_add(buffer_queue_t *const q, const buffer_handle_t h, const uint32_t due_ms, const uint32_t expiry_ms, const uint8_t tag, const uint32_t key) {
    if (!buffer_valid(q->pool, h)) {
        q->st_rejected++;
        return false;
    }
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (q->slot[i].valid)
            continue;
        q->slot[i].buf = buffer_ref(q->pool, h);
        q->slot[i].valid = true;
        q->slot[i].tag = tag;
        q->slot[i].key = key;
        q->slot[i].due_ms = due_ms;
        q->slot[i].expiry_ms = expiry_ms;
        q->used++;
        q->st_added++;
        if (q->used > q->high_water)
            q->high_water = q->used;
        BUFFER_LOCK_RELEASE(&q->lock);
        return true;
    }
    q->st_rejected++;
    BUFFER_LOCK_RELEASE(&q->lock);
    return false;
}

/*
 * Reap whatever has run out of time. Returns how many.
 *
 * Called on every cycle, including cycles where the radio is busy: a frame too stale to be worth
 * sending must be dropped then, not sent late when the radio frees up.
 */
static inline uint16_t buffer_queue_expire(buffer_queue_t *const q, const uint32_t now_ms) {
    uint16_t expired = 0;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid || q->slot[i].expiry_ms == 0)
            continue;
        if ((int32_t)(now_ms - q->slot[i].expiry_ms) < 0)
            continue;
        buffer_unref(q->pool, q->slot[i].buf);
        q->slot[i].valid = false;
        q->used--;
        q->st_expired++;
        expired++;
    }
    BUFFER_LOCK_RELEASE(&q->lock);
    return expired;
}

/*
 * Take the earliest-due frame, HANDING OVER the queue's reference.
 *
 * The caller now owns that reference and must unref when done with it -- or pass it to another
 * holder, which is what an ACK tracker does. Returns BUFFER_NONE when nothing is due, which is the
 * normal case and not a failure.
 */
/* Which frame take() would hand over, WITHOUT taking it. The reference is NOT transferred: the
   queue still holds it, so the caller must read and let go, never unref. For deciding whether it
   is worth asking the radio at all, and for a test that wants to look at what is queued. */
static inline buffer_handle_t buffer_queue_peek(const buffer_queue_t *const q, const uint32_t now_ms, uint8_t *const out_tag, uint32_t *const out_key) {
    int best = -1;
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid || (int32_t)(now_ms - q->slot[i].due_ms) < 0)
            continue;
        if (best < 0 || (int32_t)(q->slot[i].due_ms - q->slot[best].due_ms) < 0)
            best = (int)i;
    }
    if (best < 0)
        return (buffer_handle_t)BUFFER_NONE;
    if (out_tag != NULL)
        *out_tag = q->slot[best].tag;
    if (out_key != NULL)
        *out_key = q->slot[best].key;
    return q->slot[best].buf;
}

static inline buffer_handle_t buffer_queue_take(buffer_queue_t *const q, const uint32_t now_ms, uint8_t *const out_tag, uint32_t *const out_key) {
    int best = -1;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid || (int32_t)(now_ms - q->slot[i].due_ms) < 0)
            continue;
        if (best < 0 || (int32_t)(q->slot[i].due_ms - q->slot[best].due_ms) < 0)
            best = (int)i;
    }
    if (best < 0) {
        BUFFER_LOCK_RELEASE(&q->lock);
        return (buffer_handle_t)BUFFER_NONE;
    }
    const buffer_handle_t h = q->slot[best].buf;
    if (out_tag != NULL)
        *out_tag = q->slot[best].tag;
    if (out_key != NULL)
        *out_key = q->slot[best].key;
    q->slot[best].valid = false;
    q->used--;
    BUFFER_LOCK_RELEASE(&q->lock);
    return h; /* the reference travels with it: no ref/unref pair here on purpose */
}

/* Drop a queued frame by tag+key, e.g. on an ACK arriving for it. Returns whether one was found. */
static inline bool buffer_queue_remove_key(buffer_queue_t *const q, const uint8_t tag, const uint32_t key) {
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid || q->slot[i].tag != tag || q->slot[i].key != key)
            continue;
        buffer_unref(q->pool, q->slot[i].buf);
        q->slot[i].valid = false;
        q->used--;
        BUFFER_LOCK_RELEASE(&q->lock);
        return true;
    }
    BUFFER_LOCK_RELEASE(&q->lock);
    return false;
}

/* Drop every queued frame of a tag, e.g. a stale beacon superseded by a newer one. */
static inline uint16_t buffer_queue_remove_tag(buffer_queue_t *const q, const uint8_t tag) {
    uint16_t n = 0;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid || q->slot[i].tag != tag)
            continue;
        buffer_unref(q->pool, q->slot[i].buf);
        q->slot[i].valid = false;
        q->used--;
        n++;
    }
    BUFFER_LOCK_RELEASE(&q->lock);
    return n;
}

/* Release everything. A queue going away must not leak the frames it was holding. */
static inline void buffer_queue_clear(buffer_queue_t *const q) {
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++) {
        if (!q->slot[i].valid)
            continue;
        buffer_unref(q->pool, q->slot[i].buf);
        q->slot[i].valid = false;
    }
    q->used = 0;
    BUFFER_LOCK_RELEASE(&q->lock);
}

/* The depth it has reached, and how often an add found no room. Inferable from the pool's own
   high-water only if nothing else holds buffers, which is exactly when it stops being true -- so
   the queue reports its own. */
static inline uint16_t buffer_queue_high_water(const buffer_queue_t *const q) {
    return q->high_water;
}

static inline uint32_t buffer_queue_added(const buffer_queue_t *const q) {
    return q->st_added;
}
static inline uint32_t buffer_queue_expired(const buffer_queue_t *const q) {
    return q->st_expired;
}
static inline uint32_t buffer_queue_rejected(const buffer_queue_t *const q) {
    return q->st_rejected;
}

#endif /* D_MODULE_BUFFERS_H */
