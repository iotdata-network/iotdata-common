
#ifndef IOTDATA_NODE_SETTINGS_H
#define IOTDATA_NODE_SETTINGS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_settings.h - the PROTOCOL's own settings: station id, reporting schedule, receive
// schedule. Read and written as one TLV, persisted in the node state block.
//
// NOT CONFIG. Config is what the DEVICE is set to -- a sample interval, a threshold, a calibration
// coefficient -- and it is entirely the implementation's. This is what the PROTOCOL is set to, it
// is the same on every node, and the library owns it.
//
// NOT CONTROL EITHER. These used to be control actions, `set-station` and `set-trigger`, which is
// a write wearing a verb's clothes: an action has no defined answer, so a rejected or clamped value
// looked exactly like a successful one. As a mutable type the rule that already governs CONFIG
// applies -- a write assigns and then returns WHAT IS NOW TRUE -- and reading the whole schedule is
// one request rather than one round trip per subject.
//
// PERSISTENCE IS MANDATORY. A node whose station id cannot be changed and retained cannot be
// commissioned in the field. The store is the node state block, and a write FLUSHES immediately
// rather than deferring: iotdata_state_touch() is write-behind, and a promise is not a fact.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* How many subjects a node keeps a schedule for. The system types, plus room for a few of a
   vendor's own -- a dimensioning choice, not a protocol limit. */
#ifndef IOTDATA_SETTINGS_REPORT_MAX
#define IOTDATA_SETTINGS_REPORT_MAX (IOTDATA_NODE_TLV_SYSTEM_COUNT + 4)
#endif

#define IOTDATA_SETTINGS_STATE_TAG     0x53455431u /* "SET1" */
#define IOTDATA_SETTINGS_STATE_VERSION 1u

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint8_t subject;
    uint16_t flags;    /* IOTDATA_NODE_REPORT_* */
    uint16_t period_s; /* the ON_PERIOD cadence */
    uint16_t change_s; /* the MINIMUM interval between ON_CHANGE emissions */
    uint16_t events;   /* a bitmask the SUBJECT owns */
} iotdata_settings_report_t;

/* The persisted image. Kept plain and flat: iotdata_state_insert() takes a pointer to the live
   struct, so this IS what lands in the datastore, and a layout change must bump the version. */
typedef struct {
    uint16_t station;
    uint16_t window_ms;  /* how long the receiver stays on  */
    uint16_t interval_s; /* how often it opens              */
    uint16_t offset_s;   /* when the next one opens         */
    uint8_t report_count;
    iotdata_settings_report_t report[IOTDATA_SETTINGS_REPORT_MAX];
} iotdata_settings_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE DEFAULTS UNDER THE SETTINGS
//
// What a node comes up as before anything has been written to it -- the values iotdata_settings_defaults()
// SEEDS, not values a reader falls back to. Nothing reads them at run time: seeding copies them into
// the settings block once and from then on the block is the only answer to the question.
//
// They live here rather than in one node's own module because a default that each node picks
// separately is not a default, it is a coincidence. (They were local to the relay. The gateway,
// having no copy, came up with ZERO -- no startup burst, no periodic anything -- which nobody
// decided and nobody could see.)
//
// An application overrides any of them before including this header.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_SETTINGS_DEFAULT_STARTUP
#define IOTDATA_SETTINGS_DEFAULT_STARTUP ((1u << IOTDATA_NODE_TLV_VERSION) | (1u << IOTDATA_NODE_TLV_VARIANT))
#endif

/* Six hours. Slow on purpose: STATUS is a heartbeat, and on a shared 2.4kbps channel a fleet's
   heartbeats are the traffic nobody accounted for. */
#ifndef IOTDATA_SETTINGS_DEFAULT_PERIOD_STATUS_S
#define IOTDATA_SETTINGS_DEFAULT_PERIOD_STATUS_S 21600u
#endif

/* How long the receiver stays on, and how often it opens. An application that listens on some
   other cadence overrides these before including; what matters is that the node HAS an answer. */
#ifndef IOTDATA_SETTINGS_DEFAULT_WINDOW_MS
#define IOTDATA_SETTINGS_DEFAULT_WINDOW_MS 5000u
#endif
#ifndef IOTDATA_SETTINGS_DEFAULT_INTERVAL_S
#define IOTDATA_SETTINGS_DEFAULT_INTERVAL_S 60u
#endif

/* Spread the startup burst over this window. A fleet that lost power together comes back together,
   and without this every node in it would announce itself in the same second. */
#ifndef IOTDATA_SETTINGS_DEFAULT_STARTUP_BACKOFF_MS
#define IOTDATA_SETTINGS_DEFAULT_STARTUP_BACKOFF_MS 30000u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_settings_defaults(iotdata_settings_t *const s, const uint16_t derived) {
    if (s == NULL)
        return;
    *s = (iotdata_settings_t){ 0 };
    s->station = derived;
    s->window_ms = (uint16_t)IOTDATA_SETTINGS_DEFAULT_WINDOW_MS;
    s->interval_s = (uint16_t)IOTDATA_SETTINGS_DEFAULT_INTERVAL_S;
    s->offset_s = 0;
    for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT && s->report_count < (uint8_t)IOTDATA_SETTINGS_REPORT_MAX; t++)
        if (iotdata_node_tlv_is_reportable(t)) {
            const bool at_startup = ((IOTDATA_SETTINGS_DEFAULT_STARTUP >> t) & 1u) != 0u;
            const uint16_t period = (t == IOTDATA_NODE_TLV_STATUS) ? (uint16_t)IOTDATA_SETTINGS_DEFAULT_PERIOD_STATUS_S : 0u;
            s->report[s->report_count++] = (iotdata_settings_report_t){
                .subject = t,
                .flags = (uint16_t)((at_startup ? IOTDATA_NODE_REPORT_AT_STARTUP : 0u) | ((period > 0u) ? IOTDATA_NODE_REPORT_ON_PERIOD : 0u)),
                .period_s = period,
                .change_s = 0,
                .events = 0,
            };
        }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* The snapshot, kept as the state layer's restore-to image AND as the answer to "what would this
   have been": the only way to put a value back to its default is to know the default and state it,
   so something has to be able to say what it is. */
__attribute__((unused)) static iotdata_settings_t _iotdata_settings_default;

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_SETTINGS_PIN_STATION  0x01u
#define IOTDATA_SETTINGS_PIN_WINDOW   0x02u
#define IOTDATA_SETTINGS_PIN_INTERVAL 0x04u
#define IOTDATA_SETTINGS_PIN_OFFSET   0x08u

__attribute__((unused)) static uint8_t _iotdata_settings_pin;
__attribute__((unused)) static uint32_t _iotdata_settings_pin_report; /* bit N = subject N's schedule */

static inline void iotdata_settings_pin(const uint8_t which) {
    _iotdata_settings_pin |= which;
}
static inline bool iotdata_settings_is_pinned(const uint8_t which) {
    return (_iotdata_settings_pin & which) != 0u;
}
static inline void iotdata_settings_pin_report(const uint8_t subject) {
    if (subject < 32u)
        _iotdata_settings_pin_report |= (uint32_t)1u << subject;
}
static inline bool iotdata_settings_report_is_pinned(const uint8_t subject) {
    return (subject < 32u) && (((_iotdata_settings_pin_report >> subject) & 1u) != 0u);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef bool (*iotdata_settings_saver_fn)(const iotdata_settings_t *s);
__attribute__((unused)) static iotdata_settings_saver_fn _iotdata_settings_saver = NULL;
static inline void iotdata_settings_saver_attach(const iotdata_settings_saver_fn fn) {
    _iotdata_settings_saver = fn;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_settings_freeze(const iotdata_settings_t *const s) {
    if (s != NULL)
        _iotdata_settings_default = *s;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_settings_attach(iotdata_settings_t *const s, iotdata_node_state_t *const state) {
    if (s == NULL || state == NULL)
        return false;
    iotdata_settings_freeze(s);
    return iotdata_state_insert(state, IOTDATA_SETTINGS_STATE_TAG, IOTDATA_SETTINGS_STATE_VERSION, s, sizeof(*s), &_iotdata_settings_default);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_settings_report_t *iotdata_settings_report_find(const iotdata_settings_t *const s, const uint8_t subject) {
    if (s == NULL)
        return NULL;
    for (uint8_t i = 0; i < s->report_count; i++)
        if (s->report[i].subject == subject)
            return &s->report[i];
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_settings_subject_ok(const uint8_t subject) {
    return iotdata_node_tlv_is_reportable(subject) || !iotdata_tlv_type_is_system(subject);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_settings_report_set(iotdata_settings_t *const s, const iotdata_settings_report_t *const r) {
    if (s == NULL || r == NULL || !iotdata_settings_subject_ok(r->subject))
        return false;
    for (uint8_t i = 0; i < s->report_count; i++)
        if (s->report[i].subject == r->subject) {
            s->report[i] = *r;
            return true;
        }
    if (s->report_count >= (uint8_t)IOTDATA_SETTINGS_REPORT_MAX)
        return false;
    s->report[s->report_count++] = *r;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint16_t iotdata_settings_station(const iotdata_settings_t *const s) {
    return (s != NULL) ? s->station : 0u;
}

static inline uint16_t _iotdata_settings_flags(const iotdata_settings_t *const s, const uint8_t subject) {
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    return (r != NULL) ? r->flags : 0u;
}

static inline bool iotdata_settings_at_startup(const iotdata_settings_t *const s, const uint8_t subject) {
    return (_iotdata_settings_flags(s, subject) & IOTDATA_NODE_REPORT_AT_STARTUP) != 0u;
}

static inline uint16_t iotdata_settings_period_s(const iotdata_settings_t *const s, const uint8_t subject) {
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    if (r == NULL || (r->flags & IOTDATA_NODE_REPORT_ON_PERIOD) == 0u)
        return 0u;
    return r->period_s;
}

static inline uint16_t iotdata_settings_window_ms(const iotdata_settings_t *const s) {
    return (s != NULL) ? s->window_ms : 0u;
}

static inline uint32_t iotdata_settings_interval_ms(const iotdata_settings_t *const s) {
    return (s != NULL) ? (uint32_t)s->interval_s * 1000u : 0u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_settings_pack(iotdata_kvr_t *const kv, const iotdata_settings_t *const s, const uint8_t *const sel, const uint8_t sellen) {
    if (kv == NULL || s == NULL)
        return -1;
    bool want_station = (sellen == 0), want_report = (sellen == 0), want_receive = (sellen == 0);
    for (uint8_t i = 0; i < sellen; i++) {
        want_station = want_station || (sel[i] == IOTDATA_NODE_SETTINGS_STATION);
        want_report = want_report || (sel[i] == IOTDATA_NODE_SETTINGS_REPORT);
        want_receive = want_receive || (sel[i] == IOTDATA_NODE_SETTINGS_RECEIVE);
    }
    if (want_station)
        iotdata_kvr_add_u16(kv, IOTDATA_NODE_SETTINGS_STATION, s->station);
    if (want_report)
        for (uint8_t i = 0; i < s->report_count; i++) {
            uint8_t e[IOTDATA_NODE_SETTINGS_REPORT_SIZE];
            e[0] = s->report[i].subject;
            iotdata_node_settings_put_u16(e, 1, s->report[i].flags);
            iotdata_node_settings_put_u16(e, 3, s->report[i].period_s);
            iotdata_node_settings_put_u16(e, 5, s->report[i].change_s);
            iotdata_node_settings_put_u16(e, 7, s->report[i].events);
            iotdata_kvr_add(kv, IOTDATA_NODE_SETTINGS_REPORT, e, (uint8_t)sizeof(e));
        }
    if (want_receive) {
        uint8_t r[IOTDATA_NODE_SETTINGS_RECEIVE_SIZE];
        iotdata_node_settings_put_u16(r, 0, s->window_ms);
        iotdata_node_settings_put_u16(r, 2, s->interval_s);
        iotdata_node_settings_put_u16(r, 4, s->offset_s);
        iotdata_kvr_add(kv, IOTDATA_NODE_SETTINGS_RECEIVE, r, (uint8_t)sizeof(r));
    }
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_settings_apply(iotdata_settings_t *const s, const uint8_t *const kv, const size_t kvlen) {
    if (s == NULL || kv == NULL)
        return false;
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    bool changed = false;
    while (iotdata_kvr_next(kv, kvlen, &cur, &key, &val, &vlen)) {
        switch (key) {
        case IOTDATA_NODE_SETTINGS_STATION:
            if (vlen == 2) {
                const uint16_t want = iotdata_node_settings_get_u16(val, 0);
                if (want != s->station && want != 0u && want < IOTDATA_STATION_MAX && !iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_STATION))
                    s->station = want, changed = true;
            }
            break;
        case IOTDATA_NODE_SETTINGS_REPORT:
            if (vlen == IOTDATA_NODE_SETTINGS_REPORT_SIZE) {
                const iotdata_settings_report_t r = {
                    .subject = val[0],
                    .flags = iotdata_node_settings_get_u16(val, 1),
                    .period_s = iotdata_node_settings_get_u16(val, 3),
                    .change_s = iotdata_node_settings_get_u16(val, 5),
                    .events = iotdata_node_settings_get_u16(val, 7),
                };
                const iotdata_settings_report_t *const had = iotdata_settings_report_find(s, r.subject);
                if ((had == NULL || memcmp(had, &r, sizeof(r)) != 0) && !iotdata_settings_report_is_pinned(r.subject) && iotdata_settings_report_set(s, &r))
                    changed = true;
            }
            break;
        case IOTDATA_NODE_SETTINGS_RECEIVE:
            if (vlen == IOTDATA_NODE_SETTINGS_RECEIVE_SIZE) {
                const uint16_t w = iotdata_node_settings_get_u16(val, 0), i = iotdata_node_settings_get_u16(val, 2), o = iotdata_node_settings_get_u16(val, 4);
                if (w != s->window_ms && !iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_WINDOW))
                    s->window_ms = w, changed = true;
                if (i != s->interval_s && !iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_INTERVAL))
                    s->interval_s = i, changed = true;
                if (o != s->offset_s && !iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_OFFSET))
                    s->offset_s = o, changed = true;
            }
            break;
        default:
            break; /* unknown key: skipped, not fatal */
        }
    }
    return changed;
}

static inline bool iotdata_settings_persists(const iotdata_node_state_t *const state) {
    return (_iotdata_settings_saver != NULL) || (state != NULL);
}

static inline bool iotdata_settings_commit(iotdata_settings_t *const s, iotdata_node_state_t *const state) {
    if (_iotdata_settings_saver != NULL)
        return _iotdata_settings_saver(s);
    return (state != NULL) && iotdata_state_flush(state);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_SETTINGS_KEY_PREFIX "node-"
#define IOTDATA_SETTINGS_KEY_COUNT  (4 + 2 * IOTDATA_NODE_TLV_SYSTEM_COUNT)
#define IOTDATA_SETTINGS_KEY_MAX    64

static inline bool _iotdata_settings_cat(char *const out, const size_t size, size_t *const n, const char *add) {
    for (; *add != '\0'; add++) {
        if (*n + 1u >= size)
            return false;
        out[(*n)++] = *add;
    }
    out[*n] = '\0';
    return true;
}

static inline bool _iotdata_settings_u2a(uint32_t v, char *const out, const size_t size) {
    char rev[12];
    size_t n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < sizeof(rev));
    if (n + 1u > size)
        return false;
    for (size_t i = 0; i < n; i++)
        out[i] = rev[n - 1u - i];
    out[n] = '\0';
    return true;
}

static inline bool _iotdata_settings_a2u(const char *const t, uint32_t *const out) {
    const char *d = t;
    const bool hex = (d[0] == '0' && (d[1] | 0x20) == 'x');
    uint32_t v = 0;
    unsigned n = 0;
    for (d += hex ? 2 : 0; *d != '\0'; d++, n++) {
        uint32_t c;
        if (*d >= '0' && *d <= '9')
            c = (uint32_t)(*d - '0');
        else if (hex && (*d | 0x20) >= 'a' && (*d | 0x20) <= 'f')
            c = (uint32_t)((*d | 0x20) - 'a') + 10u;
        else
            return false;
        v = v * (hex ? 16u : 10u) + c;
    }
    *out = v;
    return n > 0;
}

static inline bool _iotdata_settings_a2b(const char *const t, bool *const out) {
    if (strcmp(t, "on") == 0 || strcmp(t, "true") == 0 || strcmp(t, "yes") == 0 || strcmp(t, "1") == 0)
        *out = true;
    else if (strcmp(t, "off") == 0 || strcmp(t, "false") == 0 || strcmp(t, "no") == 0 || strcmp(t, "0") == 0)
        *out = false;
    else
        return false;
    return true;
}

static inline uint8_t _iotdata_settings_key_subject(const int i) {
    return (uint8_t)((i - 4) / 2);
}
static inline bool _iotdata_settings_key_is_startup(const int i) {
    return ((i - 4) % 2) == 0;
}

static inline bool iotdata_settings_key_name(const int i, char *const out, const size_t size) {
    if (out == NULL || size == 0 || i < 0 || i >= IOTDATA_SETTINGS_KEY_COUNT)
        return false;
    size_t n = 0;
    out[0] = '\0';
    if (!_iotdata_settings_cat(out, size, &n, IOTDATA_SETTINGS_KEY_PREFIX))
        return false;
    if (i < 4) {
        static const char *const scalar[4] = { "station", "window", "interval", "offset" };
        return _iotdata_settings_cat(out, size, &n, scalar[i]);
    }
    const uint8_t subject = _iotdata_settings_key_subject(i);
    const char *const name = iotdata_node_tlv_name(subject);
    if (name == NULL || !iotdata_node_tlv_is_reportable(subject))
        return false;
    return _iotdata_settings_cat(out, size, &n, "report-") && _iotdata_settings_cat(out, size, &n, name) && _iotdata_settings_cat(out, size, &n, _iotdata_settings_key_is_startup(i) ? "-at-startup" : "-period");
}

static inline bool _iotdata_settings_same(const char *a, const char *b, const bool prefix) {
    while (*a != '\0' && *b != '\0' && (((*a | 0x20) == (*b | 0x20)) || ((*a == '_' || *a == '-') && (*b == '_' || *b == '-'))))
        a++, b++;
    return prefix ? (*b == '\0') : (*a == '\0' && *b == '\0');
}

static inline int iotdata_settings_key_index(const char *const key) {
    if (key == NULL)
        return -1;
    char name[IOTDATA_SETTINGS_KEY_MAX];
    for (int i = 0; i < IOTDATA_SETTINGS_KEY_COUNT; i++)
        if (iotdata_settings_key_name(i, name, sizeof(name)) && _iotdata_settings_same(key, name, false))
            return i;
    return -1;
}

static inline bool iotdata_settings_key_is_ours(const char *const key) {
    return (key != NULL) && _iotdata_settings_same(key, IOTDATA_SETTINGS_KEY_PREFIX, true);
}

static inline bool iotdata_settings_key_read(const iotdata_settings_t *const s, const int i, char *const out, const size_t size) {
    if (s == NULL || out == NULL || size == 0 || i < 0 || i >= IOTDATA_SETTINGS_KEY_COUNT)
        return false;
    if (i < 4) {
        static const size_t off[4] = { 0, 1, 2, 3 };
        const uint16_t v = (off[i] == 0) ? s->station : (off[i] == 1) ? s->window_ms : (off[i] == 2) ? s->interval_s : s->offset_s;
        return _iotdata_settings_u2a(v, out, size);
    }
    const uint8_t subject = _iotdata_settings_key_subject(i);
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    if (r == NULL)
        return false;
    if (_iotdata_settings_key_is_startup(i)) {
        size_t n = 0;
        out[0] = '\0';
        return _iotdata_settings_cat(out, size, &n, (r->flags & IOTDATA_NODE_REPORT_AT_STARTUP) ? "on" : "off");
    }
    if ((r->flags & IOTDATA_NODE_REPORT_ON_PERIOD) == 0u) {
        size_t n = 0;
        out[0] = '\0';
        return _iotdata_settings_cat(out, size, &n, "never"); /* a value, and it reads as one */
    }
    return _iotdata_settings_u2a(r->period_s, out, size);
}

static inline bool iotdata_settings_key_write(iotdata_settings_t *const s, const int i, const char *const text) {
    if (s == NULL || text == NULL || i < 0 || i >= IOTDATA_SETTINGS_KEY_COUNT)
        return false;
    uint32_t v = 0;
    if (i < 4) {
        if (!_iotdata_settings_a2u(text, &v) || v > 0xFFFFu)
            return false;
        if (i == 0 && !iotdata_station_is_assignable((uint16_t)v))
            return false;
        if (i == 0)
            s->station = (uint16_t)v;
        else if (i == 1)
            s->window_ms = (uint16_t)v;
        else if (i == 2)
            s->interval_s = (uint16_t)v;
        else
            s->offset_s = (uint16_t)v;
        return true;
    }
    const uint8_t subject = _iotdata_settings_key_subject(i);
    if (!iotdata_node_tlv_is_reportable(subject))
        return false;
    /* stated whole: start from what is already there, so setting one half keeps the other */
    const iotdata_settings_report_t *const cur = iotdata_settings_report_find(s, subject);
    iotdata_settings_report_t r = (cur != NULL) ? *cur : (iotdata_settings_report_t){ .subject = subject, .flags = 0, .period_s = 0, .change_s = 0, .events = 0 };
    r.subject = subject;
    if (_iotdata_settings_key_is_startup(i)) {
        bool on = false;
        if (!_iotdata_settings_a2b(text, &on))
            return false;
        r.flags = (uint16_t)(on ? (r.flags | IOTDATA_NODE_REPORT_AT_STARTUP) : (r.flags & (uint16_t)~IOTDATA_NODE_REPORT_AT_STARTUP));
    } else {
        if (strcmp(text, "never") == 0 || strcmp(text, "off") == 0)
            v = 0u;
        else if (!_iotdata_settings_a2u(text, &v) || v > 0xFFFFu)
            return false;
        r.period_s = (uint16_t)v;
        r.flags = (uint16_t)((r.period_s > 0u) ? (r.flags | IOTDATA_NODE_REPORT_ON_PERIOD) : (r.flags & (uint16_t)~IOTDATA_NODE_REPORT_ON_PERIOD));
    }
    return iotdata_settings_report_set(s, &r);
}

_Static_assert(IOTDATA_SETTINGS_PIN_STATION == (1u << 0) && IOTDATA_SETTINGS_PIN_WINDOW == (1u << 1) && IOTDATA_SETTINGS_PIN_INTERVAL == (1u << 2) && IOTDATA_SETTINGS_PIN_OFFSET == (1u << 3),
               "the scalar pin bits must match the scalar key indices");

static inline void iotdata_settings_key_pin(const int i) {
    if (i < 0 || i >= IOTDATA_SETTINGS_KEY_COUNT)
        return;
    if (i < 4)
        iotdata_settings_pin((uint8_t)(1u << i)); /* STATION, WINDOW, INTERVAL, OFFSET, in order */
    else
        iotdata_settings_pin_report(_iotdata_settings_key_subject(i));
}

static inline bool iotdata_settings_key_is_pinned(const int i) {
    if (i < 0 || i >= IOTDATA_SETTINGS_KEY_COUNT)
        return false;
    return (i < 4) ? iotdata_settings_is_pinned((uint8_t)(1u << i)) : iotdata_settings_report_is_pinned(_iotdata_settings_key_subject(i));
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_NODE_CONSOLE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE CONSOLE
//
// `node`, the twin of `conf`: same shape, same place, appearing by itself when a console exists.
// CONFIG is what the device can be set to; SETTINGS is what the PROTOCOL can be set to, and the two
// stay apart here for the same reason they stay apart on the wire.
//
// IT SAYS WHAT THE DEFAULT WAS, which is the whole reason to have it. Every value here is stated --
// there is no "unset" to put a field back with -- so getting back to a default means knowing what
// the default is and writing it. That is a deliberate trade: a fleet of mixed builds does not share
// one default, so "revert" would mean something different on each node, whereas a value does not.
// The listing prints the current value and, where they differ, the one this build came up with.
//
//     node                          everything, with defaults where they differ
//     node station [<id>]
//     node window  [<ms>]           how long the receiver stays on
//     node interval [<s>]           how often it opens
//     node report [<type> [...]]    at-startup on|off, period <s>|off
// -----------------------------------------------------------------------------------------------------------------------------------------

static iotdata_settings_t *_iotdata_settings_console = NULL;
static iotdata_node_state_t *_iotdata_settings_console_state = NULL;

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_settings_console_attach(iotdata_settings_t *const s, iotdata_node_state_t *const state) {
    _iotdata_settings_console = s;
    _iotdata_settings_console_state = state;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* A TLV type by name, for the subject of a report. Generated from the type table rather than a list
   of its own, so a type added there is addressable here without anyone remembering to say so. */
static inline int _iotdata_settings_subject(const char *const want) {
    for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++) {
        const char *const n = iotdata_node_tlv_name(t);
        if (n != NULL && iotdata_node_tlv_is_reportable(t)) {
            const char *a = n, *b = want;
            while (*a != '\0' && *b != '\0' && ((*a | 0x20) == (*b | 0x20)))
                a++, b++;
            if (*a == '\0' && *b == '\0')
                return (int)t;
        }
    }
    return -1;
}

static inline void _iotdata_settings_show_field(const iotdata_console_emit_fn emit, const char *const name, const unsigned value, const unsigned dflt, const char *const unit, const bool pinned) {
    if (pinned)
        emit("  %-14s %u%s PINNED on the command line%s\n", name, value, unit, (value == dflt) ? "" : " (default differs)");
    else if (value == dflt)
        emit("  %-14s %u%s\n", name, value, unit);
    else
        emit("  %-14s %u%s (default %u%s)\n", name, value, unit, dflt, unit);
}

static inline void _iotdata_settings_show_report(const iotdata_console_emit_fn emit, const iotdata_settings_t *const s, const uint8_t subject) {
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    const char *const name = iotdata_node_tlv_name(subject);
    if (r == NULL) { /* only a subject the table has no room for -- seeding gives every other one a record */
        emit("  %-14s (no record: the schedule is full)\n", (name != NULL) ? name : "?");
        return;
    }
    /* ON_PERIOD clear prints as "never" rather than as a zero with a caveat beside it: the zero is
       not a missing number, it is the answer. Two emits rather than a formatted buffer, so this
       file needs nothing of stdio beyond the callback it was handed. */
    emit("  %-14s at-startup=%-3s period(s)=", (name != NULL) ? name : "?", (r->flags & IOTDATA_NODE_REPORT_AT_STARTUP) ? "on" : "off");
    if ((r->flags & IOTDATA_NODE_REPORT_ON_PERIOD) != 0u)
        emit("%-8u", (unsigned)r->period_s);
    else
        emit("%-8s", "never");
    emit(" on-change=%-3s on-event=%s%s\n", (r->flags & IOTDATA_NODE_REPORT_ON_CHANGE) ? "on" : "off", (r->flags & IOTDATA_NODE_REPORT_ON_EVENT) ? "on" : "off",
         iotdata_settings_report_is_pinned(subject) ? "  PINNED" : "");
}

static void iotdata_settings_console(const iotdata_console_emit_fn emit, const int argc, char **const argv) {
    iotdata_settings_t *const s = _iotdata_settings_console;
    if (s == NULL) {
        emit("node: this node keeps no protocol settings\n");
        return;
    }
    const bool persists = iotdata_settings_persists(_iotdata_settings_console_state);

    if (argc < 2) {
        emit("node: protocol settings%s\n", persists ? "" : " (NOT PERSISTED: nowhere to write them)");
        _iotdata_settings_show_field(emit, "station", (unsigned)iotdata_settings_station(s), (unsigned)_iotdata_settings_default.station, "", iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_STATION));
        _iotdata_settings_show_field(emit, "window", (unsigned)iotdata_settings_window_ms(s), (unsigned)_iotdata_settings_default.window_ms, "ms", iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_WINDOW));
        _iotdata_settings_show_field(emit, "interval", (unsigned)(iotdata_settings_interval_ms(s) / 1000u), (unsigned)_iotdata_settings_default.interval_s, "s", iotdata_settings_is_pinned(IOTDATA_SETTINGS_PIN_INTERVAL));
        emit("  reports:\n");
        for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++)
            if (iotdata_node_tlv_is_reportable(t))
                _iotdata_settings_show_report(emit, s, t);
        return;
    }

    uint32_t v = 0;
    bool wrote = false;
    if (strcmp(argv[1], "station") == 0 || strcmp(argv[1], "window") == 0 || strcmp(argv[1], "interval") == 0) {
        uint16_t *const field = (strcmp(argv[1], "station") == 0) ? &s->station : (strcmp(argv[1], "window") == 0) ? &s->window_ms : &s->interval_s;
        const uint8_t pin = (uint8_t)((strcmp(argv[1], "station") == 0) ? IOTDATA_SETTINGS_PIN_STATION : (strcmp(argv[1], "window") == 0) ? IOTDATA_SETTINGS_PIN_WINDOW : IOTDATA_SETTINGS_PIN_INTERVAL);
        if (argc < 3) {
            const uint16_t d = (strcmp(argv[1], "station") == 0) ? _iotdata_settings_default.station : (strcmp(argv[1], "window") == 0) ? _iotdata_settings_default.window_ms : _iotdata_settings_default.interval_s;
            _iotdata_settings_show_field(emit, argv[1], (unsigned)*field, (unsigned)d, "", iotdata_settings_is_pinned(pin));
            return;
        }
        /* the console is refused too, not only the radio: the argument was somebody's decision about
           this box, and "unless you are sitting at it" is a weaker claim than the one they made */
        if (iotdata_settings_is_pinned(pin)) {
            emit("%s: PINNED on the command line -- restart without that argument to change it\n", argv[1]);
            return;
        }
        if (!_iotdata_settings_a2u(argv[2], &v) || (v > 0xFFFFu)) {
            emit("%s: '%s' is not a number\n", argv[1], argv[2]);
            return;
        }
        /* A station is checked against the protocol's own rule, not just the field's width: 0 is not
           a station and the broadcast id would make this node answer every broadcast as its own. */
        if (strcmp(argv[1], "station") == 0 && !iotdata_station_is_assignable((uint16_t)v)) {
            emit("station: %u is not assignable (1..%u)\n", (unsigned)v, (unsigned)IOTDATA_STATION_ASSIGNABLE_MAX);
            return;
        }
        *field = (uint16_t)v;
        wrote = true;
    } else if (strcmp(argv[1], "report") == 0) {
        if (argc < 3) {
            for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++)
                if (iotdata_node_tlv_is_reportable(t))
                    _iotdata_settings_show_report(emit, s, t);
            return;
        }
        const int subject = _iotdata_settings_subject(argv[2]);
        if (subject < 0) {
            emit("report: '%s' is not a reportable type\n", argv[2]);
            return;
        }
        if (argc < 5) {
            _iotdata_settings_show_report(emit, s, (uint8_t)subject);
            return;
        }
        if (iotdata_settings_report_is_pinned((uint8_t)subject)) {
            emit("report %s: PINNED on the command line -- restart without that argument to change it\n", argv[2]);
            return;
        }
        /* Stated whole: a report record that exists says everything about that subject, so a change
           to one part starts from what is already stated rather than from nothing. */
        const iotdata_settings_report_t *const cur = iotdata_settings_report_find(s, (uint8_t)subject);
        iotdata_settings_report_t r = (cur != NULL) ? *cur : (iotdata_settings_report_t){ .subject = (uint8_t)subject, .flags = 0, .period_s = 0, .change_s = 0, .events = 0 };
        r.subject = (uint8_t)subject;
        bool on = false;
        if (strcmp(argv[3], "at-startup") == 0 && _iotdata_settings_a2b(argv[4], &on))
            r.flags = (uint16_t)(on ? (r.flags | IOTDATA_NODE_REPORT_AT_STARTUP) : (r.flags & (uint16_t)~IOTDATA_NODE_REPORT_AT_STARTUP));
        else if (strcmp(argv[3], "period") == 0 && (strcmp(argv[4], "off") == 0 || (_iotdata_settings_a2u(argv[4], &v) && v <= 0xFFFFu))) {
            if (strcmp(argv[4], "off") == 0)
                v = 0u; /* "off" and "0" are the same answer, and it is NEVER rather than "no opinion" */
            /* "off" is period 0: a stated entry with ON_PERIOD clear says NEVER, which is a real
               answer and not the same as having no opinion. */
            r.period_s = (uint16_t)v;
            r.flags = (uint16_t)((r.period_s > 0u) ? (r.flags | IOTDATA_NODE_REPORT_ON_PERIOD) : (r.flags & (uint16_t)~IOTDATA_NODE_REPORT_ON_PERIOD));
        } else {
            emit("report: expected 'at-startup on|off' or 'period <seconds>|off'\n");
            return;
        }
        if (!iotdata_settings_report_set(s, &r)) {
            emit("report: no room for another subject\n");
            return;
        }
        wrote = true;
    } else {
        emit("node: '%s'? try `node` for what there is\n", argv[1]);
        return;
    }

    if (wrote) {
        /* Persisted NOW rather than on the tick, for the reason a settings write over the air is:
           a station id that is only in RAM is one a power cut takes back. */
        const bool ok = persists && iotdata_settings_commit(s, _iotdata_settings_console_state);
        /* the answer is what is now true, and ONLY about what was asked: re-entering with the same
           subject and no value is the read that pairs with the write just made */
        char *nargv[3] = { argv[0], argv[1], (argc > 2) ? argv[2] : NULL };
        iotdata_settings_console(emit, (strcmp(argv[1], "report") == 0) ? 3 : 2, nargv);
        if (!ok)
            emit("  (not persisted: this node has nowhere to write it)\n");
    }
}

/*
 * The settings as command-line options, for a --help -- the twin of iotdata_config_help().
 *
 * A host that reads "node-" lines out of its configuration file accepts them as arguments too, and
 * an argument is the ONLY way to pin one. A feature nobody can find is not a feature, so the list
 * that already knows every key generates this rather than a hand-kept paragraph going stale.
 */
static inline void iotdata_settings_help(const iotdata_console_emit_fn emit, const iotdata_settings_t *const s) {
    char key[IOTDATA_SETTINGS_KEY_MAX], val[32];
    emit("\n  the protocol's own settings -- stating one here PINS it for the run:\n");
    for (int k = 0; k < IOTDATA_SETTINGS_KEY_COUNT; k++) {
        if (!iotdata_settings_key_name(k, key, sizeof(key)))
            continue;
        char opt[IOTDATA_SETTINGS_KEY_MAX + 4];
        opt[0] = opt[1] = '-';
        size_t at = 2;
        for (const char *p = key; *p != '\0' && at + 1u < sizeof(opt); p++)
            opt[at++] = *p;
        opt[at] = '\0';
        if (s != NULL && iotdata_settings_key_read(s, k, val, sizeof(val)))
            emit("  %-34s default %s\n", opt, val);
        else
            emit("  %-34s\n", opt);
    }
}

/* Drop this into the application's iotdata_console_t array, beside IOTDATA_CONFIG_CONSOLE_COMMAND. */
#define IOTDATA_SETTINGS_CONSOLE_COMMAND { "node", iotdata_settings_console, "show or set protocol settings: node [station|window|interval|report] ..." }

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_SETTINGS_H */
