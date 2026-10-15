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
//   - The scalar accessors (count, high_water, added, expired, rejected) are deliberately NOT
//     locked: they are diagnostics, and a moment's staleness costs nothing. PEEK IS LOCKED, and
//     takes a non-const queue because of it -- a read that acquires a lock is not a const read.
//     What the lock buys is that the handle, tag and key it hands back describe ONE entry. They
//     are three separate loads: unlocked, another thread's take and a later add could reuse that
//     slot between them, and the caller would get a tag from one frame and a buffer from another,
//     for an entry that was never due. What the lock cannot buy is freshness -- the entry stays in
//     the queue and no reference is transferred, so on a queue another thread takes from, the
//     handle can go stale the instant the lock is dropped. Peek is for deciding whether to bother;
//     take is the only safe way to get a frame OFF a shared queue.
//
// NO FIELD ACCESS. Everything is a function or a macro. The struct is visible because this is a
// header-only library and the storage has to be declared somewhere, not because it is an interface.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define BUFFER_NONE 0xFFu /* not a handle: what acquire returns when the pool is empty */

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

typedef struct {
    uint8_t refs;
    uint16_t at;  /* `at` is where the data starts, moving BACKWARDS as headers are prepended */
    uint16_t len; /* `len` is how many bytes of data there are from `at`. */
} buffer_meta_t;

typedef struct {
    uint8_t *data; /* count * stride bytes */
    buffer_meta_t *meta;
    uint32_t *free_map; /* one bit per buffer, SET = free (so "find a free one" is a ctz) */
    uint16_t count;
    uint16_t stride;
    uint16_t offset;    /* where a receive starts: the room reserved for prepending */
    uint16_t hint_word; /* which map word last yielded a buffer: see buffer_acquire */
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
    p->hint_word = 0;
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
 * Take a free buffer: find a set bit, clear it, stamp the meta.
 *
 * IT STARTS WHERE IT LAST SUCCEEDED, rather than always at word 0, and that is worth the one field
 * it costs on a pool of more than 32. The scan takes the LOWEST free bit, so buffers held for a
 * long time settle into the low words and the short-lived ones are pushed above them -- a gateway
 * dimensioned 24 processing + 128 downstream will hold most of those 128 for as long as the nodes
 * behind them are alive, and every receive then walks three or four solid words to reach the bit it
 * wants. Worst exactly when the pool is busiest, which is the wrong way round.
 *
 * A HINT, NOT A CACHE OF FREED HANDLES. There is no second structure to keep in step and no
 * invariant beyond `hint_word < words`: being wrong costs one wasted word load, and the scan still
 * visits every word before it gives up, so nothing is ever stranded. (The kernels' per-cpu freelist
 * -- slab magazines, page_pool's recycle cache, DPDK's mempool cache -- is the heavier version of
 * this idea, and it is there to avoid a LOCK rather than a scan. The lock here is the caller's
 * choice and the scan was never the expensive part; a start word is the whole of what this needs.)
 *
 * It does mean a handle is no longer deterministically the lowest free index -- allocation is
 * round-robin from the last success. A single-word pool (count <= 32) is unaffected: there is one
 * word and the hint is always 0.
 */
static inline buffer_handle_t buffer_acquire(buffer_pool_t *const p) {
    const uint16_t words = (uint16_t)BUFFER_POOL_MAP_WORDS(p->count);
    BUFFER_LOCK_ACQUIRE(&p->lock);
    for (uint16_t n = 0; n < words; n++) {
        uint16_t w = (uint16_t)(p->hint_word + n);
        if (w >= words)
            w = (uint16_t)(w - words); /* one subtract, not a modulo: n < words, so w < 2 * words */
        if (p->free_map[w] != 0u) {
            const uint16_t bit = (uint16_t)__builtin_ctz(p->free_map[w]);
            const uint16_t i = (uint16_t)(w * 32u + bit);
            if (i < p->count) {
                p->free_map[w] &= ~(1u << bit);
                p->hint_word = w;
                p->meta[i].refs = 1;
                p->meta[i].at = p->offset; /* a receive lands past the reserved prefix */
                p->meta[i].len = 0;
                p->acquires++;
                if (++p->used > p->high_water)
                    p->high_water = p->used;
                BUFFER_LOCK_RELEASE(&p->lock);
                return (buffer_handle_t)i;
            }
        }
    }
    p->fails++;
    BUFFER_LOCK_RELEASE(&p->lock);
    return BUFFER_NONE;
}

static inline bool buffer_valid(const buffer_pool_t *const p, const buffer_handle_t h) {
    return h != BUFFER_NONE && h < p->count && p->meta[h].refs > 0;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline buffer_handle_t buffer_ref(buffer_pool_t *const p, const buffer_handle_t h) {
    BUFFER_LOCK_ACQUIRE(&p->lock);
    if (buffer_valid(p, h) && p->meta[h].refs < 0xFFu)
        p->meta[h].refs++;
    BUFFER_LOCK_RELEASE(&p->lock);
    return h;
}

static inline void buffer_unref(buffer_pool_t *const p, const buffer_handle_t h) {
    BUFFER_LOCK_ACQUIRE(&p->lock);
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

static inline uint8_t *buffer_data(buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? &p->data[(size_t)h * p->stride + p->meta[h].at] : NULL;
}
static inline uint16_t buffer_len(const buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? p->meta[h].len : 0u;
}
static inline uint16_t buffer_room(const buffer_pool_t *const p, const buffer_handle_t h) {
    return buffer_valid(p, h) ? (uint16_t)(p->stride - p->meta[h].at) : 0u;
}
static inline void buffer_set_len(buffer_pool_t *const p, const buffer_handle_t h, const uint16_t len) {
    if (buffer_valid(p, h))
        p->meta[h].len = (len > buffer_room(p, h)) ? buffer_room(p, h) : len;
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * Write `n` bytes in FRONT of the data, consuming reserved prefix.
 *
 * This is the whole reason a receive starts at an offset: a forwarding header goes on without the
 * payload being moved. Fails rather than truncating if the prefix is already spent -- a frame that
 * cannot carry its header must not go out looking like one that can.
 *
 * The `n > at` guard is load-bearing and must not be "simplified": `at` is unsigned, so without it
 * an over-long prepend wraps to ~65535 and the memcopy lands far outside the pool. Dropping it does
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
    if (hdr)
        memcpy(&p->data[(size_t)h * p->stride + p->meta[h].at], hdr, n);
    return true;
}

static inline bool buffer_write(buffer_pool_t *const p, const buffer_handle_t h, const void *const src, const uint16_t n) {
    if (!buffer_valid(p, h) || n > buffer_room(p, h))
        return false;
    memcpy(buffer_data(p, h), src, n);
    p->meta[h].len = n;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

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
// a copy, so queueing a received frame for forwarding costs a reference and no memcopy.
//
// The expiry is what makes back-pressure survivable. A radio that cannot accept a frame now is not
// an error: the caller leaves the frame queued and asks again next cycle, and only the expiry
// decides to give up. So the queue must be aged even on a cycle where nothing can be transmitted,
// which is why expiry is its own call and not a side effect of taking a frame.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define BUFFER_QUEUE_TAG_NONE 0xFFu

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

// ------------------------------------------------------------------------------------------------------------------------

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

// ------------------------------------------------------------------------------------------------------------------------

static inline void buffer_queue_set_tag_name(buffer_queue_t *const q, const buffer_tag_name_fn fn) {
    q->tag_name = fn;
}

static inline const char *buffer_queue_tag_name(const buffer_queue_t *const q, const uint8_t tag) {
    static char fallback[8];
    if (q->tag_name) {
        const char *const n = q->tag_name(tag);
        if (n)
            return n;
    }
    return snprintf_inline(fallback, sizeof(fallback), "tag%u", (unsigned)tag);
}

static inline uint16_t buffer_queue_count(const buffer_queue_t *const q) {
    return q->used;
}

// ------------------------------------------------------------------------------------------------------------------------

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
    for (uint16_t i = 0; i < q->size; i++)
        if (!q->slot[i].valid) {
            q->slot[i].buf = buffer_ref(q->pool, h);
            q->slot[i].valid = true;
            q->slot[i].tag = tag;
            q->slot[i].key = key;
            q->slot[i].due_ms = due_ms;
            q->slot[i].expiry_ms = expiry_ms;
            q->st_added++;
            if (++q->used > q->high_water)
                q->high_water = q->used;
            BUFFER_LOCK_RELEASE(&q->lock);
            return true;
        }
    q->st_rejected++;
    BUFFER_LOCK_RELEASE(&q->lock);
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * Age the queue. Called every pass, whether or not anything can be transmitted -- see above.
 *
 * AN EMPTY QUEUE IS ANSWERED WITHOUT THE WALK, and without the lock. expire(), peek() and take()
 * are the three a polling loop calls on every pass regardless of whether there is anything in
 * here, and on most of those passes there is not: a relay puts a frame or two in and drains them,
 * and a gateway's survey and trace queues are empty except during a diagnostic. Scanning `size`
 * slots to discover that is the only work the loop does when it has nothing to do.
 *
 * `used` is exact -- it moves with `valid` under the lock -- so the test is not an approximation,
 * and reading it unlocked is safe in BOTH directions, which is why it comes before the lock: read
 * stale-high and we take the lock, scan, and find nothing, which is the right answer; read
 * stale-low, because another thread added between the read and now, and we skip a pass -- the
 * entry is still there with its own absolute due and expiry, and the next pass takes it. The same
 * conservative read buffer_queue_count() has always handed out.
 *
 * NOT on remove_key/remove_tag/clear. Those are called because something happened -- an ack
 * arrived, a request superseded another, a module is shutting down -- so the queue is usually not
 * empty when they run and there is nothing to skip. Only the polling path repeats.
 */
static inline uint16_t buffer_queue_expire(buffer_queue_t *const q, const uint32_t now_ms) {
    if (q->used == 0)
        return 0;
    uint16_t expired = 0;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++)
        if (q->slot[i].valid && q->slot[i].expiry_ms != 0)
            if ((int32_t)(now_ms - q->slot[i].expiry_ms) >= 0) {
                buffer_unref(q->pool, q->slot[i].buf);
                q->slot[i].valid = false;
                q->used--;
                q->st_expired++;
                expired++;
            }
    BUFFER_LOCK_RELEASE(&q->lock);
    return expired;
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * Take the earliest-due frame, HANDING OVER the queue's reference.
 *
 * The caller now owns that reference and must unref when done with it -- or pass it to another
 * holder, which is what an ACK tracker does. Returns BUFFER_NONE when nothing is due, which is the
 * normal case and not a failure.
 */
/* Which frame take() would hand over, WITHOUT taking it. The reference is NOT transferred: the
   queue still holds it, so the caller must read and let go, never unref. For deciding whether it
   is worth asking the radio at all, and for a test that wants to look at what is queued.
   LOCKED -- see the LOCKING block above for what that does and does not make safe: the three
   values describe one entry, and on a shared queue the handle can still go stale afterwards. */
static inline buffer_handle_t buffer_queue_peek(buffer_queue_t *const q, const uint32_t now_ms, uint8_t *const out_tag, uint32_t *const out_key) {
    if (q->used == 0)
        return BUFFER_NONE;
    int best = -1;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++)
        if (q->slot[i].valid && (int32_t)(now_ms - q->slot[i].due_ms) >= 0)
            if (best < 0 || (int32_t)(q->slot[i].due_ms - q->slot[best].due_ms) < 0)
                best = (int)i;
    const buffer_handle_t h = best < 0 ? BUFFER_NONE : q->slot[best].buf;
    if (h != BUFFER_NONE) {
        if (out_tag)
            *out_tag = q->slot[best].tag;
        if (out_key)
            *out_key = q->slot[best].key;
    }
    BUFFER_LOCK_RELEASE(&q->lock);
    return h;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline buffer_handle_t buffer_queue_take(buffer_queue_t *const q, const uint32_t now_ms, uint8_t *const out_tag, uint32_t *const out_key) {
    if (q->used == 0)
        return BUFFER_NONE;
    int best = -1;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++)
        if (q->slot[i].valid && (int32_t)(now_ms - q->slot[i].due_ms) >= 0)
            if (best < 0 || (int32_t)(q->slot[i].due_ms - q->slot[best].due_ms) < 0)
                best = (int)i;
    const buffer_handle_t h = best < 0 ? BUFFER_NONE : q->slot[best].buf;
    if (h != BUFFER_NONE) {
        if (out_tag)
            *out_tag = q->slot[best].tag;
        if (out_key)
            *out_key = q->slot[best].key;
        q->slot[best].valid = false;
        q->used--;
    }
    BUFFER_LOCK_RELEASE(&q->lock);
    return h; /* the reference travels with it: no ref/unref pair here on purpose */
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool buffer_queue_remove_key(buffer_queue_t *const q, const uint8_t tag, const uint32_t key) {
    if (q->used == 0)
        return false;
    uint16_t n = 0;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size && n == 0; i++)
        if (q->slot[i].valid && q->slot[i].tag == tag && q->slot[i].key == key) {
            buffer_unref(q->pool, q->slot[i].buf);
            q->slot[i].valid = false;
            q->used--;
            n++;
        }
    BUFFER_LOCK_RELEASE(&q->lock);
    return n > 0;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline uint16_t buffer_queue_remove_tag(buffer_queue_t *const q, const uint8_t tag) {
    if (q->used == 0)
        return 0;
    uint16_t n = 0;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++)
        if (q->slot[i].valid && q->slot[i].tag == tag) {
            buffer_unref(q->pool, q->slot[i].buf);
            q->slot[i].valid = false;
            q->used--;
            n++;
        }
    BUFFER_LOCK_RELEASE(&q->lock);
    return n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void buffer_queue_clear(buffer_queue_t *const q) {
    if (q->used == 0)
        return;
    BUFFER_LOCK_ACQUIRE(&q->lock);
    for (uint16_t i = 0; i < q->size; i++)
        if (q->slot[i].valid) {
            buffer_unref(q->pool, q->slot[i].buf);
            q->slot[i].valid = false;
        }
    q->used = 0;
    BUFFER_LOCK_RELEASE(&q->lock);
}

// ------------------------------------------------------------------------------------------------------------------------

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

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_BUFFERS_H */
