
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

/* Unset everywhere, so a node that has never been written reads back as "nothing stated" rather
   than as a schedule of zeroes. A station of UNSET means "use what I derived from my hardware". */
static inline void iotdata_settings_defaults(iotdata_settings_t *const s) {
    if (s == NULL)
        return;
    *s = (iotdata_settings_t){ 0 };
    s->station = IOTDATA_NODE_SETTINGS_UNSET;
    s->window_ms = IOTDATA_NODE_SETTINGS_UNSET;
    s->interval_s = IOTDATA_NODE_SETTINGS_UNSET;
    s->offset_s = IOTDATA_NODE_SETTINGS_UNSET;
}

/* Register as a state block. Must be called before iotdata_state_load(), like any other block. */
static inline bool iotdata_settings_attach(iotdata_settings_t *const s, iotdata_node_state_t *const state) {
    if (s == NULL || state == NULL)
        return false;
    static iotdata_settings_t defaults;
    iotdata_settings_defaults(&defaults);
    iotdata_settings_defaults(s);
    return iotdata_state_insert(state, IOTDATA_SETTINGS_STATE_TAG, IOTDATA_SETTINGS_STATE_VERSION, s, sizeof(*s), &defaults);
}

/* The schedule for one subject, or NULL if none was ever set for it. */
static inline const iotdata_settings_report_t *iotdata_settings_report_find(const iotdata_settings_t *const s, const uint8_t subject) {
    if (s == NULL)
        return NULL;
    for (uint8_t i = 0; i < s->report_count; i++)
        if (s->report[i].subject == subject)
            return &s->report[i];
    return NULL;
}

/* A subject worth scheduling: one this node could actually report. A proprietary type counts --
   that is the whole reason the subject is carried in the value rather than baked into a key. */
static inline bool iotdata_settings_subject_ok(const uint8_t subject) {
    return iotdata_node_tlv_is_reportable(subject) || !iotdata_tlv_type_is_system(subject);
}

/* Upsert one subject's schedule. Returns false for a subject that cannot be reported, which is not
   an error to propagate: the write simply does not take, and the read-back says so. */
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
// WHAT THE NODE ASKS
//
// Every reader takes the node's own default and returns it when the setting is NOT STATED. That is
// the whole composition rule: a written entry overrides, an absent one defers, and a node with no
// settings block at all behaves exactly as it did before there was one. `s` may be NULL throughout.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The station to come up as. A written one wins; otherwise whatever the hardware derived, which is
   what an uncommissioned node has. */
static inline uint16_t iotdata_settings_station(const iotdata_settings_t *const s, const uint16_t derived) {
    if (s == NULL || s->station == IOTDATA_NODE_SETTINGS_UNSET)
        return derived;
    return s->station;
}

static inline uint16_t _iotdata_settings_flags(const iotdata_settings_t *const s, const uint8_t subject) {
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    return (r != NULL) ? r->flags : 0u;
}

/* Emit this subject once at boot? */
static inline bool iotdata_settings_at_startup(const iotdata_settings_t *const s, const uint8_t subject, const bool dflt) {
    if (iotdata_settings_report_find(s, subject) == NULL)
        return dflt;
    return (_iotdata_settings_flags(s, subject) & IOTDATA_NODE_REPORT_AT_STARTUP) != 0u;
}

/* The on-period cadence in seconds; 0 means never. A stated entry with ON_PERIOD clear says NEVER,
   which is a real answer and not an absence -- that is the difference between "do not schedule
   this" and "I have no opinion". */
static inline uint16_t iotdata_settings_period_s(const iotdata_settings_t *const s, const uint8_t subject, const uint16_t dflt) {
    const iotdata_settings_report_t *const r = iotdata_settings_report_find(s, subject);
    if (r == NULL)
        return dflt;
    if ((r->flags & IOTDATA_NODE_REPORT_ON_PERIOD) == 0u || r->period_s == IOTDATA_NODE_SETTINGS_UNSET)
        return 0u;
    return r->period_s;
}

static inline uint16_t iotdata_settings_window_ms(const iotdata_settings_t *const s, const uint16_t dflt) {
    return (s == NULL || s->window_ms == IOTDATA_NODE_SETTINGS_UNSET) ? dflt : s->window_ms;
}

/* How often the window opens, in MILLISECONDS to match what a scheduler counts in. */
static inline uint32_t iotdata_settings_interval_ms(const iotdata_settings_t *const s, const uint32_t dflt) {
    return (s == NULL || s->interval_s == IOTDATA_NODE_SETTINGS_UNSET) ? dflt : (uint32_t)s->interval_s * 1000u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE WIRE
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
                /* a station is 12 bits, and BOTH ends are reserved by the framing: 0 is
                   unassignable and IOTDATA_STATION_MAX is broadcast, so the usable range is
                   strictly between them. A write outside it does not take, and the read-back says
                   so -- which is the whole error channel. */
                if (want != s->station && (want == IOTDATA_NODE_SETTINGS_UNSET || (want != 0u && want < IOTDATA_STATION_MAX)))
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
                if ((had == NULL || memcmp(had, &r, sizeof(r)) != 0) && iotdata_settings_report_set(s, &r))
                    changed = true;
            }
            break;
        case IOTDATA_NODE_SETTINGS_RECEIVE:
            if (vlen == IOTDATA_NODE_SETTINGS_RECEIVE_SIZE) {
                const uint16_t w = iotdata_node_settings_get_u16(val, 0), i = iotdata_node_settings_get_u16(val, 2), o = iotdata_node_settings_get_u16(val, 4);
                if (w != s->window_ms || i != s->interval_s || o != s->offset_s)
                    s->window_ms = w, s->interval_s = i, s->offset_s = o, changed = true;
            }
            break;
        default:
            break; /* unknown key: skipped, not fatal */
        }
    }
    return changed;
}

/* Persist NOW rather than on the tick. Write-behind would make the response a promise: a power cut
   between the answer and the flush leaves the node disagreeing with what it just told a manager. */
static inline bool iotdata_settings_commit(iotdata_node_state_t *const state) {
    return (state != NULL) && iotdata_state_flush(state);
}

#endif /* IOTDATA_NODE_SETTINGS_H */
