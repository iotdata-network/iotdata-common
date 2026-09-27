#ifndef IOTDATA_NODE_CONFIG_TYPES_H
#define IOTDATA_NODE_CONFIG_TYPES_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config.h - what the DEVICE is set to: the application's own settable parameters.
//
// NOT SETTINGS. SETTINGS is what the PROTOCOL is set to -- station id, reporting schedule, receive
// schedule -- it is the same on every node, and the library owns it. This is the device's own, it
// differs on every device, and the library knows nothing about what any entry MEANS.
//
// THE TABLE IS COMPILED IN, and the header is included TWICE:
//
//     #include "iotdata_node_config.h"     // the types, so a handler can be written
//
//     static bool on_channel(const iotdata_config_row_t *r, const iotdata_config_value_t *was,
//                            const iotdata_config_info_t *i) { ... }
//
//     #define IOTDATA_CONFIG_ENTRIES(X)
//         X(SENSOR_TX_PERIOD_S, 0x001, U16, 10, 3600, 60, 0,                         NULL, NULL)
//         X(LORA_CHANNEL,       0x002, U8,  0,  83,   23, IOTDATA_CONFIG_FLAG_REBOOT, NULL, on_channel)
//     #include "iotdata_node_config.h"     // now the table, the ids and the accessors
//
// (each X line ends with a backslash in real code; they are omitted here so this stays a comment)
//
// ONE TRANSLATION UNIT. The expansion defines a STATIC table and a STATIC value array, so a second
// .c file including it gets a second, independent copy -- two halves of one device disagreeing
// about its own settings, silently. These apps are single-TU by construction (app.c #includes what
// it needs), which is what makes the compiled-in table affordable; an app that is not must put the
// table in one file and reach it through accessors from the others.
//
// THE GENERATED NAMES ARE IOTDATA_CFG_*, not IOTDATA_CONFIG_*. The latter is already the
// compile-time knob namespace (iotdata_config.h), and an entry named after one of those knobs --
// LORA_CHANNEL, say -- would otherwise expand an existing #define into the middle of an enum.
//
// Compiled in rather than registered, because that is what lets an accessor be TYPE-CHECKED: the id
// carries its type as a sibling macro, so config_get_u16(LORA_CHANNEL) fails to BUILD rather than
// returning a wrong answer at three in the morning. For the same reason the accessors assert rather
// than returning bool+out-param -- with a compiled-in table a missing id is a build error, never a
// runtime condition, and every call site would otherwise carry a branch that can never be taken.
//
// TWO WAYS TO CONSUME A VALUE, and a device wants both:
//
//   READ IT WHEN YOU NEED IT   config_get_u16(SENSOR_TX_PERIOD_S) in the loop that uses it. No
//                              cached global, so a change is picked up by construction and there is
//                              no third place for the value to go stale.
//
//   BE TOLD IT CHANGED         a notify handler, for anything that cannot simply be re-read: a
//                              radio that must be reconfigured, a buffer that must be re-sized.
//                              Reading it every time would not help -- the ACT is what matters.
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE RECORD
//
// id(12) | type(4), one 16-bit word, and it is the SAME shape in the datastore and on the wire. One
// encoder serves NVS, a file and a TLV, and a read is self-describing: a manager that has never
// heard of an id still knows how wide it is and how to print it.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_CONFIG_TYPE_BOOL      0x0
#define IOTDATA_CONFIG_TYPE_U8        0x1
#define IOTDATA_CONFIG_TYPE_I8        0x2
#define IOTDATA_CONFIG_TYPE_U16       0x3
#define IOTDATA_CONFIG_TYPE_I16       0x4
#define IOTDATA_CONFIG_TYPE_U32       0x5
#define IOTDATA_CONFIG_TYPE_I32       0x6
#define IOTDATA_CONFIG_TYPE_U64       0x7
#define IOTDATA_CONFIG_TYPE_I64       0x8
#define IOTDATA_CONFIG_TYPE_FLOAT     0x9
#define IOTDATA_CONFIG_TYPE_STRING    0xA
#define IOTDATA_CONFIG_TYPE_BLOB      0xB
/* 0xC..0xF spare */

#define IOTDATA_CONFIG_ID_MAX         0x0FFFu
#define IOTDATA_CONFIG_ID_PROPRIETARY 0x0800u /* the upper half of the id space is the vendor's */

static inline uint16_t iotdata_config_rec(const uint16_t id, const uint8_t type) {
    return (uint16_t)(((id & IOTDATA_CONFIG_ID_MAX) << 4) | (type & 0x0Fu));
}
static inline uint16_t iotdata_config_rec_id(const uint16_t rec) {
    return (uint16_t)(rec >> 4);
}
static inline uint8_t iotdata_config_rec_type(const uint16_t rec) {
    return (uint8_t)(rec & 0x0Fu);
}

static inline uint8_t iotdata_config_type_size(const uint8_t type) {
    switch (type) {
    case IOTDATA_CONFIG_TYPE_BOOL:
    case IOTDATA_CONFIG_TYPE_U8:
    case IOTDATA_CONFIG_TYPE_I8:
        return 1;
    case IOTDATA_CONFIG_TYPE_U16:
    case IOTDATA_CONFIG_TYPE_I16:
        return 2;
    case IOTDATA_CONFIG_TYPE_U32:
    case IOTDATA_CONFIG_TYPE_I32:
    case IOTDATA_CONFIG_TYPE_FLOAT:
        return 4;
    case IOTDATA_CONFIG_TYPE_U64:
    case IOTDATA_CONFIG_TYPE_I64:
        return 8;
    default:
        return 0; /* variable: string and blob */
    }
}

static inline bool iotdata_config_type_is_signed(const uint8_t type) {
    return type == IOTDATA_CONFIG_TYPE_I8 || type == IOTDATA_CONFIG_TYPE_I16 || type == IOTDATA_CONFIG_TYPE_I32 || type == IOTDATA_CONFIG_TYPE_I64;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE TABLE
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_CONFIG_FLAG_REBOOT   0x01u /* takes effect only after a restart                       */
#define IOTDATA_CONFIG_FLAG_READONLY 0x02u /* reportable, not writable: a fact rather than a setting  */

typedef union {
    bool b;
    int64_t i;
    uint64_t u;
    float f;
    struct {
        const char *p;
        uint16_t len;
    } s;
} iotdata_config_value_t;

struct iotdata_config_row;
struct iotdata_config_update;

typedef struct {
    bool validation_only; /* a dry run: check, do not act                                   */
    bool version_changed; /* the persisted image was written by a different build           */
} iotdata_config_info_t;

/* Check one proposed value. NULL means "the standard check", which is the bounds. Runs on BOTH
   paths -- a value arriving typed over the air never parses, and must still be validated.

   It is handed the whole UPDATE, not just its own row, and that is what staging buys: "the minimum
   must not exceed the maximum" can only be checked once both proposed values are in hand. Use
   iotdata_config_update_peek() to read another entry as it WILL BE, which is its staged value if
   the same update touched it and its current one otherwise. `u` is NULL for a standalone check. */
typedef bool (*iotdata_config_validate_fn)(const struct iotdata_config_row *row, const iotdata_config_value_t *proposed, const struct iotdata_config_update *u);

/* Told AFTER the whole set is committed and saved. Cannot veto -- that is what validate is for --
   and returns whether a reboot is needed to make it real. This is where a radio gets reconfigured.
   Separate from validate because they run on different paths and at different times: one may reject
   and must not act, the other may act and cannot reject. */
typedef bool (*iotdata_config_notify_fn)(const struct iotdata_config_row *row, const iotdata_config_value_t *old, const iotdata_config_info_t *info);

typedef struct iotdata_config_row {
    uint16_t id;
    uint8_t type;
    uint8_t flags;
    const char *name;
    iotdata_config_value_t min, max, dflt;
    iotdata_config_validate_fn validate;
    iotdata_config_notify_fn notify;
} iotdata_config_row_t;

#endif /* IOTDATA_NODE_CONFIG_TYPES_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE EXPANSION
//
// One X-macro list becomes four things: the ids, a TYPE sibling per id, an index per id, and the
// table itself. The TYPE sibling is the whole point -- it is what a typed accessor can assert on.
//
// SCALARS ONLY, for now. STRING and BLOB have record types so the WIRE format is complete, but a
// table row cannot use one yet: they need per-row storage the fixed-size RAM form does not have.
// The static assert below refuses one rather than letting it half-work.
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_CONFIG_ENTRIES) && !defined(IOTDATA_NODE_CONFIG_EXPANDED)
#define IOTDATA_NODE_CONFIG_EXPANDED

/* Bounds are always numeric -- a string's are its LENGTH -- so they need only the three families. */
#define _CFG_B_BOOL(x)                              { .u = (uint64_t)(x) }
#define _CFG_B_U8(x)                                { .u = (uint64_t)(x) }
#define _CFG_B_U16(x)                               { .u = (uint64_t)(x) }
#define _CFG_B_U32(x)                               { .u = (uint64_t)(x) }
#define _CFG_B_U64(x)                               { .u = (uint64_t)(x) }
#define _CFG_B_I8(x)                                { .i = (int64_t)(x) }
#define _CFG_B_I16(x)                               { .i = (int64_t)(x) }
#define _CFG_B_I32(x)                               { .i = (int64_t)(x) }
#define _CFG_B_I64(x)                               { .i = (int64_t)(x) }
#define _CFG_B_FLOAT(x)                             { .f = (float)(x) }
#define _CFG_B_STRING(x)                            { .u = (uint64_t)(x) }
#define _CFG_B_BLOB(x)                              { .u = (uint64_t)(x) }
#define _CFG_B(t, x)                                _CFG_B_##t(x)

#define _CFG_D_BOOL(x)                              { .b = (bool)(x) }
#define _CFG_D_U8(x)                                { .u = (uint64_t)(x) }
#define _CFG_D_U16(x)                               { .u = (uint64_t)(x) }
#define _CFG_D_U32(x)                               { .u = (uint64_t)(x) }
#define _CFG_D_U64(x)                               { .u = (uint64_t)(x) }
#define _CFG_D_I8(x)                                { .i = (int64_t)(x) }
#define _CFG_D_I16(x)                               { .i = (int64_t)(x) }
#define _CFG_D_I32(x)                               { .i = (int64_t)(x) }
#define _CFG_D_I64(x)                               { .i = (int64_t)(x) }
#define _CFG_D_FLOAT(x)                             { .f = (float)(x) }
#define _CFG_D_STRING(x)                            { .s = { (x), 0 } }
#define _CFG_D_BLOB(x)                              { .s = { (x), 0 } }
#define _CFG_D(t, x)                                _CFG_D_##t(x)

#define _CFG_ID(n, id, t, mn, mx, df, fl, va, no)   IOTDATA_CFG_##n = (id),
#define _CFG_TYPE(n, id, t, mn, mx, df, fl, va, no) IOTDATA_CFG_##n##__TYPE = IOTDATA_CONFIG_TYPE_##t,
#define _CFG_IX(n, id, t, mn, mx, df, fl, va, no)   IOTDATA_CFG_IX_##n,
#define _CFG_ROW(n, id, t, mn, mx, df, fl, va, no)  { (id), IOTDATA_CONFIG_TYPE_##t, (fl), #n, _CFG_B(t, mn), _CFG_B(t, mx), _CFG_D(t, df), va, no },

enum { IOTDATA_CONFIG_ENTRIES(_CFG_ID) _IOTDATA_CFG_ID_END };
enum { IOTDATA_CONFIG_ENTRIES(_CFG_TYPE) _IOTDATA_CFG_TYPE_END };
enum { IOTDATA_CONFIG_ENTRIES(_CFG_IX) IOTDATA_CFG_COUNT };

static const iotdata_config_row_t iotdata_config_table[IOTDATA_CFG_COUNT] = { IOTDATA_CONFIG_ENTRIES(_CFG_ROW) };

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE STORE
//
// Fixed-size in RAM so a read is an array index, self-describing on disk so adding or removing an
// entry does not wipe the rest: the image is a sequence of the same [id|type][value] records the
// wire carries, and a load matches each one BY ID. A record whose type or width disagrees with the
// table is skipped and that entry defaults -- which is what makes a firmware change survivable.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_STORE_KEY
#define IOTDATA_CONFIG_STORE_KEY "config"
#endif
#define IOTDATA_CONFIG_IMAGE_MAX (IOTDATA_CFG_COUNT * 10u + 4u)

static iotdata_config_value_t _iotdata_config_value[IOTDATA_CFG_COUNT];

static inline int iotdata_config_index(const uint16_t id) {
    for (int i = 0; i < (int)IOTDATA_CFG_COUNT; i++)
        if (iotdata_config_table[i].id == id)
            return i;
    return -1;
}

static inline const iotdata_config_row_t *iotdata_config_row(const uint16_t id) {
    const int i = iotdata_config_index(id);
    return (i < 0) ? NULL : &iotdata_config_table[i];
}

static inline void iotdata_config_defaults(void) {
    for (int i = 0; i < (int)IOTDATA_CFG_COUNT; i++)
        _iotdata_config_value[i] = iotdata_config_table[i].dflt;
}

/* The standard check: within bounds. A row's own validate REPLACES this, because a row that needs
   more than bounds usually needs something bounds cannot express. */
static inline bool iotdata_config_validate(const iotdata_config_row_t *const row, const iotdata_config_value_t *const v, const struct iotdata_config_update *const u) {
    if (row == NULL || v == NULL)
        return false;
    if (row->validate != NULL)
        return row->validate(row, v, u);
    if (iotdata_config_type_is_signed(row->type))
        return v->i >= row->min.i && v->i <= row->max.i;
    if (row->type == IOTDATA_CONFIG_TYPE_FLOAT)
        return v->f >= row->min.f && v->f <= row->max.f;
    return v->u >= row->min.u && v->u <= row->max.u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING
//
// Assert-style, and type-checked at COMPILE time: the id carries its type as a sibling enum, so
// iotdata_config_u16(LORA_CHANNEL) on a u8 row is a build error rather than a silent widening.
// With a compiled-in table a missing id cannot happen, so there is no bool+out-param and no branch
// at every call site that can never be taken.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define _IOTDATA_CONFIG_IS(n, want) ((void)sizeof(char[1 - 2 * !((int)(IOTDATA_CFG_##n##__TYPE) == (int)(want))]))

#define iotdata_config_bool(n)      (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_BOOL), _iotdata_config_value[IOTDATA_CFG_IX_##n].b)
#define iotdata_config_u8(n)        (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_U8), (uint8_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].u)
#define iotdata_config_u16(n)       (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_U16), (uint16_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].u)
#define iotdata_config_u32(n)       (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_U32), (uint32_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].u)
#define iotdata_config_i8(n)        (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_I8), (int8_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].i)
#define iotdata_config_i16(n)       (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_I16), (int16_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].i)
#define iotdata_config_i32(n)       (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_I32), (int32_t)_iotdata_config_value[IOTDATA_CFG_IX_##n].i)
#define iotdata_config_float(n)     (_IOTDATA_CONFIG_IS(n, IOTDATA_CONFIG_TYPE_FLOAT), _iotdata_config_value[IOTDATA_CFG_IX_##n].f)

/* Untyped, for the wire and the console, which meet an id rather than a name. */
static inline bool iotdata_config_get(const uint16_t id, iotdata_config_value_t *const out) {
    const int i = iotdata_config_index(id);
    if (i < 0 || out == NULL)
        return false;
    *out = _iotdata_config_value[i];
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// WRITING: BEGIN -> STAGE -> COMMIT -> notify
//
// THE WHOLE SET IS VALIDATED BEFORE ANY OF IT IS APPLIED. A rejection rejects EVERYTHING, because a
// half-applied set is worse than none: change a remote node's radio channel and its transmit power
// together, apply one, reject the other, and the node is unreachable and cannot be told so. There
// is deliberately no reject-this-one.
//
// Staging is also what makes CROSS-ENTRY validation possible at all -- "min must be below max" can
// only be checked once both proposed values are in hand, which is why validate sees the update and
// not just its own row.
//
// Notify runs AFTER the commit and the save, and cannot veto: by then it is true. It returns
// whether a reboot is needed, which is OR'd with the row's own flag -- the flag is necessary rather
// than advisory, because a row that validates entirely from its bounds has no handler to ask.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The image encoder, defined below: the commit saves before it announces, so that a handler which
   acts on a change can never be told about one that failed to persist. */
static inline bool iotdata_config_save(datastore_t *ds);

typedef struct iotdata_config_update {
    iotdata_config_value_t staged[IOTDATA_CFG_COUNT];
    bool touched[IOTDATA_CFG_COUNT];
    bool rejected;
    uint8_t count;
} iotdata_config_update_t;

/* An entry as it WILL BE: the staged value if this update touches it, the current one otherwise.
   What a cross-entry validate reads, so it judges the world the commit is about to create rather
   than the one it is replacing. */
static inline bool iotdata_config_update_peek(const struct iotdata_config_update *const u, const uint16_t id, iotdata_config_value_t *const out) {
    const int i = iotdata_config_index(id);
    if (i < 0 || out == NULL)
        return false;
    *out = (u != NULL && u->touched[i]) ? u->staged[i] : _iotdata_config_value[i];
    return true;
}

static inline void iotdata_config_update_begin(iotdata_config_update_t *const u) {
    if (u != NULL)
        *u = (iotdata_config_update_t){ 0 };
}

/* Propose one value. Returns false and poisons the whole update if it is not acceptable -- the
   caller need not check every call, only the commit. */
static inline bool iotdata_config_update_stage(iotdata_config_update_t *const u, const uint16_t id, const iotdata_config_value_t *const v) {
    if (u == NULL || v == NULL)
        return false;
    const int i = iotdata_config_index(id);
    /* staged FIRST, then validated, so a cross-entry check sees this value too */
    const bool was_touched = (i >= 0) && u->touched[i];
    iotdata_config_value_t was = { 0 };
    if (i >= 0) {
        was = u->staged[i];
        u->touched[i] = true;
        u->staged[i] = *v;
    }
    if (i < 0 || (iotdata_config_table[i].flags & IOTDATA_CONFIG_FLAG_READONLY) != 0u || !iotdata_config_validate(&iotdata_config_table[i], v, u)) {
        if (i >= 0) {
            u->touched[i] = was_touched;
            u->staged[i] = was;
        }
        u->rejected = true;
        return false;
    }
    if (!was_touched)
        u->count++;
    return true;
}

/* Everything staged, in one step: commit to the cache, write the image, then tell the handlers.
   Returns false if anything was rejected, in which case NOTHING was applied. */
static inline bool iotdata_config_update_commit(iotdata_config_update_t *const u, datastore_t *const ds, bool *const reboot_out) {
    if (u == NULL || u->rejected)
        return false;
    iotdata_config_value_t was[IOTDATA_CFG_COUNT];
    bool changed = false;
    for (int i = 0; i < (int)IOTDATA_CFG_COUNT; i++) {
        was[i] = _iotdata_config_value[i];
        if (u->touched[i] && memcmp(&was[i], &u->staged[i], sizeof(was[i])) != 0) {
            _iotdata_config_value[i] = u->staged[i];
            changed = true;
        }
    }
    if (!changed)
        return true; /* accepted, and nothing to save or announce */
    (void)iotdata_config_save(ds);
    const iotdata_config_info_t info = { .validation_only = false, .version_changed = false };
    bool reboot = false;
    for (int i = 0; i < (int)IOTDATA_CFG_COUNT; i++) {
        if (!u->touched[i] || memcmp(&was[i], &_iotdata_config_value[i], sizeof(was[i])) == 0)
            continue;
        if ((iotdata_config_table[i].flags & IOTDATA_CONFIG_FLAG_REBOOT) != 0u)
            reboot = true;
        if (iotdata_config_table[i].notify != NULL && iotdata_config_table[i].notify(&iotdata_config_table[i], &was[i], &info))
            reboot = true;
    }
    if (reboot_out != NULL)
        *reboot_out = reboot;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE IMAGE: one encoder for the datastore and the wire
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Append one entry as [id|type][value], big-endian, the same shape the TLV carries. Returns the
   bytes written, or 0 if it does not fit. */
static inline size_t iotdata_config_encode(const int i, uint8_t *const buf, const size_t size) {
    if (i < 0 || i >= (int)IOTDATA_CFG_COUNT || buf == NULL)
        return 0;
    const iotdata_config_row_t *const row = &iotdata_config_table[i];
    const uint8_t w = iotdata_config_type_size(row->type);
    if (w == 0 || (size_t)(2u + w) > size)
        return 0; /* variable-width types are not storable yet -- see THE EXPANSION */
    const uint16_t rec = iotdata_config_rec(row->id, row->type);
    buf[0] = (uint8_t)(rec >> 8);
    buf[1] = (uint8_t)rec;
    uint64_t raw = iotdata_config_type_is_signed(row->type) ? (uint64_t)_iotdata_config_value[i].i : _iotdata_config_value[i].u;
    if (row->type == IOTDATA_CONFIG_TYPE_FLOAT) {
        uint32_t bits;
        memcpy(&bits, &_iotdata_config_value[i].f, sizeof(bits));
        raw = bits;
    }
    for (uint8_t b = 0; b < w; b++)
        buf[2u + b] = (uint8_t)(raw >> (8u * (w - 1u - b)));
    return (size_t)(2u + w);
}

/* Read one entry back. Skips silently when the id is unknown or its type or width disagrees with
   the table -- that entry then keeps its default, which is what lets a firmware change add or drop
   a config without wiping every other one. */
static inline size_t iotdata_config_decode(const uint8_t *const buf, const size_t len) {
    if (buf == NULL || len < 3u)
        return 0;
    const uint16_t rec = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    const uint8_t type = iotdata_config_rec_type(rec), w = iotdata_config_type_size(type);
    if (w == 0 || len < (size_t)(2u + w))
        return 0; /* unreadable: stop rather than guess where the next record begins */
    const int i = iotdata_config_index(iotdata_config_rec_id(rec));
    if (i >= 0 && iotdata_config_table[i].type == type) {
        uint64_t raw = 0;
        for (uint8_t b = 0; b < w; b++)
            raw = (raw << 8) | buf[2u + b];
        if (type == IOTDATA_CONFIG_TYPE_FLOAT) {
            const uint32_t bits = (uint32_t)raw;
            memcpy(&_iotdata_config_value[i].f, &bits, sizeof(float));
        } else if (iotdata_config_type_is_signed(type)) {
            const uint8_t shift = (uint8_t)(64u - 8u * w);
            _iotdata_config_value[i].i = (int64_t)(raw << shift) >> shift; /* sign-extend */
        } else if (type == IOTDATA_CONFIG_TYPE_BOOL)
            _iotdata_config_value[i].b = (raw != 0u);
        else
            _iotdata_config_value[i].u = raw;
    }
    return (size_t)(2u + w);
}

static inline bool iotdata_config_save(datastore_t *const ds) {
    if (ds == NULL)
        return false;
    uint8_t img[IOTDATA_CONFIG_IMAGE_MAX];
    size_t at = 0;
    for (int i = 0; i < (int)IOTDATA_CFG_COUNT; i++)
        at += iotdata_config_encode(i, img + at, sizeof(img) - at);
    return datastore_write(ds, IOTDATA_CONFIG_STORE_KEY, img, at);
}

/* Defaults first, then whatever the image says: an entry the image does not mention keeps its
   default rather than becoming zero, which is the same rule an unreadable record follows. */
static inline bool iotdata_config_load(datastore_t *const ds) {
    iotdata_config_defaults();
    if (ds == NULL)
        return false;
    uint8_t img[IOTDATA_CONFIG_IMAGE_MAX];
    size_t len = 0;
    if (!datastore_read(ds, IOTDATA_CONFIG_STORE_KEY, img, sizeof(img), &len))
        return false;
    for (size_t at = 0; at < len;) {
        const size_t n = iotdata_config_decode(img + at, len - at);
        if (n == 0)
            break;
        at += n;
    }
    return true;
}

#endif /* IOTDATA_CONFIG_ENTRIES && !IOTDATA_NODE_CONFIG_EXPANDED */
