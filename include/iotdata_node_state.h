#ifndef IOTDATA_NODE_STATE_H
#define IOTDATA_NODE_STATE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_state.h - what a node has to remember across a restart, as opposed to what it can
// be told (CONFIG) or asked (STATUS).
//
// Because it will drift otherwise: state is read and written by the firmware ONLY. Never by an
// operator, never over the air, never rendered. So it needs no names, no validators, no adapters
// and no wire format -- it is native bytes.
//
// MODULES REGISTER THEIR OWN BLOCKS and keep their own memory:
//
//     static _RTC_DATA_STRUCT my_persist_t my_persist;   /* survives deep sleep by itself */
//     iotdata_node_state_insert(s, MY_TAG, MY_VERSION, &my_persist, sizeof(my_persist), &MY_DEFAULTS);
//
// so nothing owns a central struct that every module has to edit, and RTC placement stays a
// per-module decision. The store holds pointers and serialises on demand.
//
// A block is found on reload by its tag, so adding or removing a module does not corrupt the
// others: unknown tags are skipped, missing ones defaulted, the rest restored. Ordering would
// make every firmware change a migration. A tag whose block changed size is defaulted rather
// than restored, because a struct whose layout moved cannot be partly trusted.
//
//   iotdata_node_state_touch()  write-behind. The tick persists it later. For counters, cursors and
//                          anything where coming back a little stale is merely lossy.
//   iotdata_node_state_flush()  write-through, now. For anything where coming back BEHIND is unsafe.
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

/* Register a block, which must happen BEFORE iotdata_node_state_load() */
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
    if (datastore_read(s->ds, s->key, buf, buflen, &len) || len < 8u) {
        if (((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3]) == IOTDATA_NODE_STATE_MAGIC && buf[4] == IOTDATA_NODE_STATE_VERSION) {
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
    }
    return false;
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
    if (datastore_write(s->ds, s->key, buf, at)) {
        s->dirty = false;
        s->stat_saved++;
        return true;
    } else {
        s->stat_failed++;
        return false;
    }
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
