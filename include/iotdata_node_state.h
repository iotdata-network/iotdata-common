#ifndef IOTDATA_NODE_STATE_H
#define IOTDATA_NODE_STATE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_state.h - what a node has to REMEMBER across a restart, as opposed to what it can
// be told (CONFIG) or asked (STATUS).
//
// THE BOUNDARY, because it will drift otherwise: state is read and written by the firmware ONLY.
// Never by an operator, never over the air, never rendered. So it needs no names, no validators,
// no adapters and no wire format -- it is native bytes, and that is the whole reason this is a
// far smaller thing than CONFIG despite sitting on the same datastore. The moment something here
// wants to be operator-visible it has stopped being state: settable makes it CONFIG, reportable
// makes it STATUS. Sequence numbers, cursors and high-water marks stay.
//
// MODULES REGISTER THEIR OWN BLOCKS and keep their own memory:
//
//     static _RTC_DATA_STRUCT my_persist_t my_persist;   /* survives deep sleep by itself */
//     iotdata_node_state_insert(s, MY_TAG, MY_VERSION, &my_persist, sizeof(my_persist), &MY_DEFAULTS);
//
// so nothing owns a central struct that every module has to edit, and RTC placement stays a
// per-module decision. The store holds pointers and serialises on demand.
//
// KEYED BY TAG, NEVER BY ORDER. A block is found on reload by its tag, so adding or removing a
// module does not corrupt the others: unknown tags are skipped, missing ones defaulted, the rest
// restored. Ordering would make every firmware change a migration.
//
// AND THE SIZE AND VERSION MUST MATCH TOO. A tag whose block changed size is defaulted rather than
// restored, because a struct whose layout moved cannot be partly trusted -- this is the check
// _RTC_DATA_VALID does not do, comparing a magic only, so a struct that GREW between firmware
// versions is read back as valid through the old layout. The version catches what size cannot: the
// same bytes meaning something different, which is the change that would otherwise be restored
// confidently and be wrong. A module bumps it when its OWN meaning moves; nothing central does.
//
// TWO WRITE PATTERNS, AND CONFLATING THEM IS A BUG:
//
//   iotdata_node_state_touch()  write-behind. The tick persists it later. For counters, cursors and
//                          anything where coming back a little stale is merely lossy.
//   iotdata_node_state_flush()  write-through, now. For anything where coming back BEHIND is unsafe.
//
// A sequence number is the second kind, and the asymmetry is the whole reason this module exists:
// GAPS ARE SAFE, REPEATS ARE NOT. A gap is how a receiver detects loss and the protocol says so;
// a repeat is indistinguishable from a duplicate and corrupts dedup and loss accounting alike.
// Write-behind can come back behind -- persist every 60s, lose power at 59s, and the node reuses
// numbers it has already sent. So sequence RESERVES AHEAD instead (see below): it persists a high
// water mark, spends it from RAM, and on an unclean restart resumes at the mark, skipping whatever
// it had not used. One store write per N packets, and N can be large precisely because skipping
// forward costs nothing.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_STATE_BLOCKS_MAX
#define IOTDATA_NODE_STATE_BLOCKS_MAX 8
#endif
#ifndef IOTDATA_NODE_STATE_BYTES_MAX
#define IOTDATA_NODE_STATE_BYTES_MAX 512 /* the serialised image: every block plus its record headers */
#endif
#ifndef IOTDATA_NODE_STATE_SAVE_MS
#define IOTDATA_NODE_STATE_SAVE_MS 60000u /* how often the tick may write, when anything is dirty */
#endif
#ifndef IOTDATA_NODE_STATE_KEY
#define IOTDATA_NODE_STATE_KEY "state"
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_STATE_MAGIC   0x53544131UL /* "STA1" */
#define IOTDATA_NODE_STATE_VERSION 1u

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint32_t tag;
    uint16_t version; /* bumped by the module when the MEANING of its bytes changes */
    void *data;
    uint16_t size;
    const void *defaults; /* NULL: zero-fill */
    bool restored;        /* came back from the store, rather than being defaulted */
} iotdata_node_state_block_t;

typedef struct {
    datastore_t *ds;
    const char *key;
    iotdata_node_state_block_t block[IOTDATA_NODE_STATE_BLOCKS_MAX];
    uint8_t count;
    bool dirty;
    bool loaded;
    uint32_t save_ms, save_last_ms;
    uint32_t stat_saved, stat_failed, stat_restored, stat_defaulted;
    uint8_t _buffer[IOTDATA_NODE_STATE_BYTES_MAX];
} iotdata_node_state_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_state_init(iotdata_node_state_t *const s, datastore_t *const ds, const char *const key) {
    *s = (iotdata_node_state_t){ 0 };
    s->ds = ds;
    s->key = (key != NULL) ? key : IOTDATA_NODE_STATE_KEY;
    s->save_ms = IOTDATA_NODE_STATE_SAVE_MS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Register a block. Every insert must happen BEFORE iotdata_node_state_load(), because load is what
   matches the persisted records against what is registered -- a block inserted afterwards has
   already missed its restore and is holding whatever the app left in it. */
static inline bool iotdata_node_state_insert(iotdata_node_state_t *const s, const uint32_t tag, const uint16_t version, void *const data, const size_t size, const void *const defaults) {
    if (s == NULL || data == NULL || size == 0 || size > IOTDATA_NODE_STATE_BYTES_MAX || s->count >= IOTDATA_NODE_STATE_BLOCKS_MAX || s->loaded)
        return false;
    for (uint8_t i = 0; i < s->count; i++)
        if (s->block[i].tag == tag)
            return false; /* a duplicate tag would make the restore ambiguous */
    s->block[s->count++] = (iotdata_node_state_block_t){ .tag = tag, .version = version, .data = data, .size = (uint16_t)size, .defaults = defaults, .restored = false };
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_state_default(iotdata_node_state_block_t *const b) {
    if (b->defaults != NULL)
        memcpy(b->data, b->defaults, b->size);
    else
        memset(b->data, 0, b->size);
    b->restored = false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_state_load(iotdata_node_state_t *const s) {
    s->loaded = true;
    for (uint8_t i = 0; i < s->count; i++)
        _iotdata_node_state_default(&s->block[i]);
    if (s->ds == NULL)
        return false;
    uint8_t *const buf = s->_buffer;
    const size_t buflen = sizeof(s->_buffer);
    size_t len = 0;
    if (!datastore_read(s->ds, s->key, buf, buflen, &len) || len < 8u)
        return false;
    if (((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3]) != IOTDATA_NODE_STATE_MAGIC || buf[4] != IOTDATA_NODE_STATE_VERSION)
        return false;
    size_t at = 8;
    while (at + 8u <= len) {
        const uint32_t tag = (uint32_t)buf[at] << 24 | (uint32_t)buf[at + 1] << 16 | (uint32_t)buf[at + 2] << 8 | buf[at + 3];
        const uint16_t version = (uint16_t)((uint16_t)buf[at + 4] << 8 | buf[at + 5]);
        const uint16_t size = (uint16_t)((uint16_t)buf[at + 6] << 8 | buf[at + 7]);
        at += 8u;
        if (at + size > len)
            break; /* truncated image: what has been read stands, the rest defaults */
        for (uint8_t i = 0; i < s->count; i++) {
            iotdata_node_state_block_t *const b = &s->block[i];
            /* All THREE must agree. Size catches a struct that changed shape; version catches the
               nastier one it cannot see -- same bytes, different meaning, which would otherwise be
               restored confidently and be wrong. */
            if (b->tag == tag && b->version == version && b->size == size) {
                memcpy(b->data, &buf[at], size);
                b->restored = true;
                s->stat_restored++;
                break;
            }
        }
        at += size;
    }
    for (uint8_t i = 0; i < s->count; i++)
        if (!s->block[i].restored)
            s->stat_defaulted++;
    return s->stat_restored > 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_state_flush(iotdata_node_state_t *const s) {
    if (s == NULL || s->ds == NULL)
        return false;
    uint8_t *const buf = s->_buffer;
    const size_t buflen = sizeof(s->_buffer);
    buf[0] = (uint8_t)(IOTDATA_NODE_STATE_MAGIC >> 24);
    buf[1] = (uint8_t)(IOTDATA_NODE_STATE_MAGIC >> 16);
    buf[2] = (uint8_t)(IOTDATA_NODE_STATE_MAGIC >> 8);
    buf[3] = (uint8_t)(IOTDATA_NODE_STATE_MAGIC & 0xFFu);
    buf[4] = (uint8_t)IOTDATA_NODE_STATE_VERSION;
    buf[5] = s->count;
    buf[6] = buf[7] = 0;
    size_t at = 8;
    for (uint8_t i = 0; i < s->count; i++) {
        const iotdata_node_state_block_t *const b = &s->block[i];
        if (at + 8u + b->size > buflen) {
            s->stat_failed++;
            return false; /* the image does not fit: refuse rather than write a partial one */
        }
        buf[at] = (uint8_t)(b->tag >> 24);
        buf[at + 1] = (uint8_t)(b->tag >> 16);
        buf[at + 2] = (uint8_t)(b->tag >> 8);
        buf[at + 3] = (uint8_t)(b->tag & 0xFFu);
        buf[at + 4] = (uint8_t)(b->version >> 8);
        buf[at + 5] = (uint8_t)(b->version & 0xFFu);
        buf[at + 6] = (uint8_t)(b->size >> 8);
        buf[at + 7] = (uint8_t)(b->size & 0xFFu);
        at += 8u;
        memcpy(&buf[at], b->data, b->size);
        at += b->size;
    }
    if (!datastore_write(s->ds, s->key, buf, at)) {
        s->stat_failed++;
        return false;
    }
    s->dirty = false;
    s->stat_saved++;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_state_touch(iotdata_node_state_t *const s) {
    if (s != NULL)
        s->dirty = true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_state_tick(iotdata_node_state_t *const s, const uint32_t now_ms) {
    if (s == NULL || !s->dirty)
        return false;
    if (s->save_last_ms != 0u && (uint32_t)(now_ms - s->save_last_ms) < s->save_ms)
        return false;
    s->save_last_ms = now_ms;
    return iotdata_node_state_flush(s);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_STATE_H */
