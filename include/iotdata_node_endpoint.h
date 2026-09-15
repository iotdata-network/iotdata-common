
#ifndef IOTDATA_NODE_ENDPOINT_H
#define IOTDATA_NODE_ENDPOINT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_endpoint.h - the NODE personality of an end device: a sensor, or one of the virtual
// sensors a simulator stands up. The counterpart to relay_node.h and iotdata_gateway_node.h, for
// the nodes at the leaves.
//
// An end device is different from a relay or a gateway in the one way that matters: it is asleep
// almost always. It cannot be commanded whenever a manager feels like it, because there is nothing
// listening. So it advertises: every IDEP_RECEIVE_EVERY_MS it puts a RECEIVE TLV in its next
// outbound frame and then holds its receiver on for IDEP_RECEIVE_WINDOW_MS. Whoever is holding a
// downstream frame for it -- gateway or relay, see iotdata_down.h -- sends it in that window.
//
// THE DEVICE DECIDES. Only it knows its power budget. A node that never advertises is never sent
// to, and that is the default, costing nothing. Everything above is the node choosing to be
// reachable, not something done to it.
//
// SPLIT IN TWO, because a simulator is a fleet in one box: idep_config_t is per application (what
// the firmware is, how to transmit), idep_node_t is per station (identity, sequence, where its
// window has got to). A sensor has one node; a simulator has one per virtual sensor, each with its
// own window, so the simulated fleet behaves like a real one rather than all waking together.
//
// NO PERSISTENCE YET. The periods below are compile-time, and the window state lives whereever the
// caller puts idep_node_t -- RTC memory on a deep-sleeping sensor, plain RAM on a simulator. When
// config persistence arrives these become settable through CONFIG and this comment goes away.
//
// REQUIRES A DECODER. An end device that only ever transmitted could build with IOTDATA_NO_DECODE;
// one that accepts commands cannot.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

#include "iotdata_node.h"

#ifndef IDEP_RECEIVE_EVERY_MS
#define IDEP_RECEIVE_EVERY_MS (6u * 60u * 60u * 1000u) /* 6 hours between windows */
#endif
#ifndef IDEP_RECEIVE_WINDOW_MS
#define IDEP_RECEIVE_WINDOW_MS 30000u /* how long the receiver stays on: generous, to be tuned */
#endif
#ifndef IDEP_DIAG_RECORD_MAX
#define IDEP_DIAG_RECORD_MAX 128 /* one diagnostic record: only allocated when a device HAS a recorder */
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* The app adds its own STATUS keys -- battery, temperature, whatever it has. Called with the
   builder mid-flight, so it just appends. */
/* Fill in what is true of this device; iotdata_status_pack does the encoding. The app used to
   write keys into the payload itself, which is how three nodes came to emit three subsets of the
   same list in three orders. */
typedef void (*idep_status_fn)(uint16_t station, iotdata_node_status_t *out);
typedef bool (*idep_tx_fn)(const uint8_t *packet, size_t len);

/* A CONTROL key this layer does not know, offered to the application. Returns whether it was
   the app's; false means "not implemented", counted as unknown and skipped rather than failing
   the frame. */
typedef bool (*idep_control_fn)(uint16_t station, uint8_t key, const uint8_t *val, uint8_t vlen);

/* One diagnostic record per call, from `cursor` (start it at 0), returning its length and 0 when
   there are no more -- the same shape the relay's node layer uses, so a device that gains a
   recorder plugs it in the same way wherever it runs. */
typedef size_t (*idep_diag_fn)(size_t *cursor, char *out, size_t outsize);

typedef struct {
    /* What this instance HAS -- the rest of VERSION (chip, IDF, stamp, eFuse serial) is detected
       by iotdata_node_version.h and needs nothing from the app. May be NULL. */
    const iotdata_version_caps_t *caps;
    /* Fills iotdata_node_status_t. A plain end device leaves .mesh.present false -- it is in
       nobody's mesh and must OMIT the group rather than report a zeroed one. A node that both
       senses and relays, which is coming, fills the group in and answers the mesh scope as a
       relay does, without needing a second callback to do it. */
    idep_status_fn status;
    idep_tx_fn tx;
    /* Optional: the same app-control seam the relay and the gateway have. `control_keys` is what
       it implements, so the CONTROL report can advertise it -- this layer cannot know, and a
       hardcoded list here would go stale the first time the app changed. */
    idep_control_fn control;
    const uint8_t *control_keys;
    uint8_t control_keys_count;
    /* Optional: a recorder. Most end devices have none, and then a diagnostics request is still
       ANSWERED -- see idep_build_diagnostics. */
    idep_diag_fn diag;
    /*
     * The receiver is NEVER off: a simulator on the bench, or any mains-powered end device.
     *
     * Such a node does not advertise at all, and the other two fields are ignored. A DOWN frame is
     * always TRANSMITTED first and only then held (see iotdata_down.h), so a node that is always
     * listening hears the immediate transmission -- the advertisement exists purely to tell a
     * holder when a SLEEPING node is briefly awake, and there is nothing to tell about a node that
     * never sleeps. The held copy simply goes unused and expires.
     *
     * The one thing given up is the retry path: if that immediate transmission is lost on the air,
     * nothing prompts the holder to try again, so the command has to be reissued. For a bench
     * instrument that is the right trade; a field node that wants the retry should advertise
     * occasionally instead, which is what receive_every_ms is for.
     */
    bool receive_always;
    uint32_t receive_every_ms;  /* 0 = never open a window: this node cannot be reached */
    uint16_t receive_window_ms; /* advertised, so a sender can skip one too short to use */
    uint16_t packet_max;
} idep_config_t;

static inline size_t idep_packet_budget(const idep_config_t *const cfg) {
    const size_t want = (cfg->packet_max != 0u) ? (size_t)cfg->packet_max : (size_t)IOTDATA_MAX_PACKET_SIZE;
    return want < (size_t)IOTDATA_MAX_PACKET_SIZE ? want : (size_t)IOTDATA_MAX_PACKET_SIZE;
}

/* Per station. On a deep-sleeping sensor this must live in RTC memory to survive the sleep. */
typedef struct {
    uint16_t station;
    uint16_t sequence; /* our own, for frames we originate */
    /* Where `sequence` persists. NULL is fine and simply means it does not. Station id is
       MAC-derived for now; persisting THAT is for when it becomes settable. */
    iotdata_node_state_t *state;
    uint32_t elapsed_ms;    /* since the last window closed */
    uint32_t window_end_ms; /* meaningful only while window_open */
    bool window_open;
    bool window_pending; /* due, and to be advertised in the next frame we send */
    uint32_t stat_rx, stat_requests, stat_reports, stat_commands, stat_unknown, stat_windows;
} idep_node_t;

/*
 * ONE STATION, ONE SEQUENCE, and this is where it lives.
 *
 * (station, sequence) is the protocol's whole identity for a packet, so a station that both relays
 * and reports still has a SINGLE stream: two counters under one id give a receiver two interleaved
 * sequences, and then dedup can drop a good frame because the other stream already used that
 * number, while counting gaps to measure loss becomes meaningless. Every module that emits a frame
 * as us takes a pointer to this and none keeps a counter of its own.
 *
 * `state` may be NULL -- an unpersisted node still counts, it just starts from 0 after a reboot.
 */
/* Registration, split out from init because the two happen at DIFFERENT times on a sleeping node.
   The struct lives in RTC memory and must survive a deep sleep, so init -- which memsets -- runs
   only on a cold boot; but the store has to be told about the block on EVERY boot or it has
   nothing to flush or load. An always-on node calls init alone and never notices the difference. */
static inline bool idep_node_bind(idep_node_t *const n, iotdata_node_state_t *const state, const uint32_t tag) {
    n->state = state;
    return iotdata_state_insert(state, tag, 1u, &n->sequence, sizeof(n->sequence), NULL);
}

static inline void idep_node_init(idep_node_t *const n, const uint16_t station, iotdata_node_state_t *const state, const uint32_t tag) {
    memset(n, 0, sizeof(*n));
    n->station = station;
    (void)idep_node_bind(n, state, tag);
}

static inline uint16_t idep_station(const idep_node_t *const n) {
    return n->station;
}
static inline uint16_t idep_sequence(const idep_node_t *const n) {
    return n->sequence;
}

/* It went out. Advances past the DOWN sentinel -- a plain ++ wraps 65535 -> 0 and 65535 is the
   sentinel -- and marks the store dirty, so no call site has to remember either.
   Separate from peek because the iotdata rule is that a sequence advances only if there WAS a
   transmission: a frame that failed to build, or that the radio refused, must not consume one, or a
   gap would mean "never sent" instead of "lost". */
static inline void idep_sequence_used(idep_node_t *const n) {
    n->sequence = iotdata_sequence_next(n->sequence);
    iotdata_state_touch(n->state);
}

/* peek + used, for a caller whose transmission IS handing the frame to a queue that owns it from
   then on -- the mesh packers, where there is no later success to wait for. */
static inline uint16_t idep_sequence_take(idep_node_t *const n) {
    const uint16_t seq = idep_sequence(n);
    idep_sequence_used(n);
    return seq;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// The receive window
//
// Two clocks, because an end device has two. A deep-sleeping sensor advances `elapsed` by the
// cycle period it just slept through; an always-awake simulator advances it by wall time. Both
// then ask the same question.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Account for time passing. Returns true when a window has become due, at which point the next
   frame this node sends should carry a RECEIVE TLV. */
static inline bool idep_window_advance(const idep_config_t *const cfg, idep_node_t *const n, const uint32_t delta_ms) {
    if (cfg->receive_always)
        return false; /* always listening: nothing to schedule and nothing to announce */
    if (cfg->receive_every_ms == 0)
        return false;
    if (n->window_pending)
        return true; /* already due and not yet advertised */
    n->elapsed_ms += delta_ms;
    if (n->elapsed_ms < cfg->receive_every_ms)
        return false;
    n->window_pending = true;
    return true;
}

static inline bool idep_window_pending(const idep_node_t *const n) {
    return n->window_pending;
}

/* Called just after transmitting the frame that advertised the window: the receiver is on from
   now. The advertisement and the window have to line up, which is why this is a separate step from
   advancing the clock. */
static inline void idep_window_begin(const idep_config_t *const cfg, idep_node_t *const n, const uint32_t now_ms) {
    n->window_pending = false;
    n->window_open = true;
    n->window_end_ms = now_ms + cfg->receive_window_ms;
    n->elapsed_ms = 0;
    n->stat_windows++;
}

static inline bool idep_window_active(const idep_config_t *const cfg, const idep_node_t *const n, const uint32_t now_ms) {
    if (cfg->receive_always)
        return true; /* nothing to expire */
    return n->window_open && (int32_t)(n->window_end_ms - now_ms) > 0;
}

static inline void idep_window_end(idep_node_t *const n) {
    n->window_open = false;
}

/* Append the advertisement to a frame being built. Empty payload: "listening, for anything", with
   the duration stated so a sender holding something too big for the window can skip it.

   Riding an outbound telemetry frame is the cheap way -- two bytes on a frame already being sent. */
static inline bool idep_receive_append(const idep_config_t *const cfg, iotdata_encoder_t *const enc) {
    uint8_t kv[8];
    /* 0 = unstated = "listening, for anything", which is the whole truth for an always-on node */
    const size_t n = iotdata_node_receive_build(kv, sizeof(kv), cfg->receive_always ? 0u : cfg->receive_window_ms, 0);
    return iotdata_encode_tlv(enc, IOTDATA_NODE_TLV_RECEIVE, kv, (uint8_t)n) == IOTDATA_OK;
}

/* The same advertisement as a frame of its own, for a node that cannot reach inside the telemetry
   frame it is about to send. Costs a whole frame instead of two bytes, so prefer the append. */
static inline bool idep_receive_announce(const idep_config_t *const cfg, idep_node_t *const n) {
    if (cfg->tx == NULL)
        return false;
    static iotdata_encoder_t enc;
    static uint8_t packet[IOTDATA_MAX_PACKET_SIZE]; // XXX
    size_t len = 0;
    if (iotdata_encode_begin(&enc, packet, idep_packet_budget(cfg), 0, n->station, n->sequence) != IOTDATA_OK)
        return false;
    if (!idep_receive_append(cfg, &enc))
        return false;
    if (iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return false;
    idep_sequence_used(n);
    return cfg->tx(packet, len);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// Builders. Same contract as the relay and gateway: the length, or -1 if it could not be built.
// Length 0 is a valid answer, so callers test for -1 and never for 0.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void idep_add_str(iotdata_kvr_t *const kv, const uint8_t key, const char *const s) {
    if (s != NULL && *s != '\0')
        iotdata_kvr_add_str(kv, key, s);
}

static inline int idep_build_version(const idep_config_t *const cfg, uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    /* Every field from iotdata_node_version.h, so a sensor answers this in exactly the form a
       relay and a gateway do -- one grammar for a reader to parse, whatever it is talking to. */
    return iotdata_version_pack(&kv, cfg->caps);
}

/* Unlike a relay or a gateway, an end device DOES produce telemetry, so this is the one place the
   variant suite is actually reported. The manifest goes in every chunk (see iotdata_node.h) and
   the cursor resumes where the last frame stopped -- a whole suite does not fit one frame.
 *
 * This is the builder PARTIAL was built for: it has always broken a suite across frames and until
 * now said nothing, so a reader had no way to tell a truncated suite from a short one. With a
 * partial it reports index/total and the marker goes out beside it; with NULL it behaves exactly
 * as before, which is what a caller that does not page still gets. */
static inline int idep_build_variant(uint8_t *const buf, const size_t size, iotdata_partial_t *const p) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_VARIANT_MANIFEST, iotdata_node_variant_manifest());
    /* Counted over the whole suite, not the chunk, so every chunk of one report agrees on it. */
    uint8_t total = 0;
    for (int v = 0; v <= IOTDATA_VARIANT_MAX; v++)
        if (iotdata_get_variant((uint8_t)v) != NULL)
            total++;
    int v = (p != NULL) ? (int)p->cursor : 0;
    uint8_t packed = 0;
    for (; v <= IOTDATA_VARIANT_MAX; v++) {
        const iotdata_variant_def_t *const d = iotdata_get_variant((uint8_t)v);
        if (d == NULL)
            continue;
        static uint8_t val[IOTDATA_MAX_PACKET_SIZE];
        const size_t n = iotdata_node_variant_encode(d, val, sizeof(val));
        if (n == 0)
            continue;
        if (kv.len + 2u + n > size)
            break; /* leave it for the next frame */
        iotdata_kvr_add(&kv, (uint8_t)v, val, (uint8_t)n);
        packed++;
    }
    if (p != NULL) {
        p->total = total;
        p->chunk = packed;
        p->more = (v <= IOTDATA_VARIANT_MAX);
        p->cursor = p->more ? (uint32_t)v : 0u; /* 0: the suite is done, a later request starts it again */
    }
    return kv.overflow ? -1 : (int)kv.len;
}

static inline int idep_build_control(const idep_config_t *const cfg, uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    /* an end device keeps no mesh tables */
    return iotdata_control_pack(&kv, &(const iotdata_control_report_t){ .tables = false, .keys = cfg->control_keys, .keys_count = cfg->control_keys_count });
}

/*
 * Every node answers a diagnostics request, because "nothing recorded" is an answer -- the rule
 * VARIANT already follows, and the CONTROL report advertises DIAGNOSTICS unconditionally on that
 * basis. An end device usually has no recorder at all, and then the report is EMPTY rather than
 * absent: absent is indistinguishable from a node that ignored the request, which is precisely
 * what its own CONTROL report promised it would not do.
 */
static inline int idep_build_diagnostics(const idep_config_t *const cfg, uint8_t *const buf, const size_t size, iotdata_partial_t *const p) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    uint8_t packed = 0;
    bool more = false;
    if (cfg->diag != NULL) {
        static char rec[IDEP_DIAG_RECORD_MAX]; /* static: too large for a sensor's stack frame */
        size_t cursor = 0, n;
        iotdata_kvr_add_u8(&kv, IOTDATA_NODE_DIAGNOSTICS_TYPE, IOTDATA_NODE_DIAG_BLACKBOX);
        while ((n = cfg->diag(&cursor, rec, sizeof(rec))) > 0) {
            if (n > 255u || kv.len + 2u + n > size) {
                more = true; /* what does not fit waits for the next request */
                break;
            }
            iotdata_kvr_add(&kv, IOTDATA_NODE_DIAGNOSTICS_DATA, rec, (uint8_t)n);
            packed++;
        }
    }
    /* The recorder is a ring being written while it is read, so a total counted up front would be
       a guess. What this CAN say honestly is how many went out and whether more were waiting --
       total is the running tally, which is why a diagnostics dump is assembled by identifier
       rather than by counting down to a promised total. */
    if (p != NULL) {
        p->chunk = packed;
        p->more = more;
        p->total = (uint8_t)(p->index + packed + (more ? 1u : 0u));
    }
    return kv.overflow ? -1 : (int)kv.len;
}

/* `scope` is the STATUS_REQUEST value: which groups to report, 0 (absent) meaning all of them.
   Asking a plain end device for the MESH group correctly yields an EMPTY status -- it is in no
   mesh -- rather than the node group it did not ask for. */
static inline int idep_build_status(const idep_config_t *const cfg, const idep_node_t *const n, uint8_t *const buf, const size_t size, const uint8_t scope) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_node_status_t s;
    memset(&s, 0, sizeof(s));
    if (cfg->status != NULL)
        cfg->status(n->station, &s); /* the app knows its own uptime, battery, heap */
    return iotdata_status_pack(&kv, &s, scope);
}

/* What a manager can see of our schedule. Read-only until config persistence exists. */
static inline int idep_build_config(const idep_config_t *const cfg, uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_RECEIVE_DURATION, cfg->receive_window_ms);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_CONFIG_PERIOD_STATUS, (uint16_t)(cfg->receive_every_ms / 1000u));
    return kv.overflow ? -1 : (int)kv.len;
}

/* `partial` may be NULL, and NULL is the whole of the no-cost path: every builder then behaves as
   it always did -- pack what fits, fail if it does not. The state used to be a file-static cursor
   in here, which meant one variant walk shared by every node a simulator stood up. */
static inline int idep_build(const idep_config_t *const cfg, const idep_node_t *const n, const uint8_t type, uint8_t *const buf, const size_t size, const uint8_t scope, iotdata_partial_t *const partial) {
    switch (type) {
    case IOTDATA_NODE_TLV_VERSION:
        return idep_build_version(cfg, buf, size);
    case IOTDATA_NODE_TLV_VARIANT:
        return idep_build_variant(buf, size, partial);
    case IOTDATA_NODE_TLV_CONTROL:
        return idep_build_control(cfg, buf, size);
    case IOTDATA_NODE_TLV_STATUS:
        return idep_build_status(cfg, n, buf, size, scope);
    case IOTDATA_NODE_TLV_CONFIG:
        return idep_build_config(cfg, buf, size);
    case IOTDATA_NODE_TLV_DIAGNOSTICS:
        return idep_build_diagnostics(cfg, buf, size, partial);
    default:
        return -1;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// Reporting and control
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Reply with one system TLV, as an ordinary frame from us. A report says who SENT it, so there is
   no addressing to do -- whoever is listening republishes it. */
/* `scope` only means anything to STATUS; 0 is "every group", which is what every caller that is
   not answering an explicit request wants. */
/* One chunk of a report. `partial` NULL is the old behaviour exactly; non-NULL carries the walk
   across calls and comes back with `more` set while records remain.
 *
 * The CALLER decides when to send the next one. Looping in here would put a whole report on the
 * air back-to-back, and at 2.4kbps against a 180ms inter-frame floor that is a node colliding with
 * itself -- so a paged report goes out on the application's own transmit cadence, one chunk a
 * cycle. */
static inline bool idep_report_paged(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t type, const uint8_t scope, iotdata_partial_t *const partial) {
    if (cfg->tx == NULL)
        return false;
    /* Sized by what the PROTOCOL can express; filled only as far as the caller's radio will carry
       (idep_packet_budget). The kv buffer cannot need more than the packet it has to fit inside. */
    static uint8_t kvbuf[IOTDATA_MAX_PACKET_SIZE]; /* static: too large for a sensor's stack frame */ // XXX
    static uint8_t packet[IOTDATA_MAX_PACKET_SIZE];                                                   // XXX
    static iotdata_encoder_t enc;

    iotdata_partial_begin(partial, n->sequence);
    const size_t budget = idep_packet_budget(cfg);
    const int kvlen = idep_build(cfg, n, type, kvbuf, budget < sizeof(kvbuf) ? budget : sizeof(kvbuf), scope, partial);
    if (kvlen < 0)
        return false;

    size_t len = 0;
    if (iotdata_encode_begin(&enc, packet, budget, 0, n->station, n->sequence) != IOTDATA_OK)
        return false;
    /* Immediately before its target: the binding is positional. */
    if (iotdata_partial_needed(partial) && !iotdata_partial_emit(&enc, partial))
        return false;
    if (iotdata_encode_tlv(&enc, type, kvbuf, (uint8_t)kvlen) != IOTDATA_OK)
        return false;
    if (iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return false;
    n->stat_reports++;
    if (!cfg->tx(packet, len))
        return false;
    idep_sequence_used(n);         /* only now: the rule is that a refused frame consumes nothing */
    iotdata_partial_sent(partial); /* only now: a refused frame must not skip its records */
    return true;
}

static inline bool idep_report_scoped(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t type, const uint8_t scope) {
    iotdata_partial_t partial = { 0 };
    return idep_report_paged(cfg, n, type, scope, &partial); /* one chunk; the rest needs a caller that loops */
}

static inline bool idep_report(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t type) {
    return idep_report_scoped(cfg, n, type, 0); /* 0 = every group */
}

static inline void idep_process_control(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t *const kv, const size_t kvlen, bool *const reboot_out) {
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(kv, kvlen, &cur, &key, &val, &vlen)) {
        const uint8_t want = iotdata_node_tlv_control_type(key);
        if (want != IOTDATA_NODE_TLV_NONE) {
            n->stat_requests++;
            /* STATUS is the one request with a value: which groups to report. Anything else that
               carries a value is answered in full, as if it had carried none. */
            const uint8_t scope = (want == IOTDATA_NODE_TLV_STATUS && vlen >= 1) ? val[0] : 0u;
            (void)idep_report_scoped(cfg, n, want, scope);
            continue;
        }
        switch (key) {
        case IOTDATA_NODE_CONTROL_REBOOT:
            n->stat_commands++;
            if (reboot_out != NULL)
                *reboot_out = true;
            break;
        default:
            /* not ours: offer it to the app before giving up. A mesh command reaching a node with
               no mesh lands here and is correctly counted unknown. */
            if (cfg->control != NULL && cfg->control(n->station, key, val, vlen))
                n->stat_commands++;
            else
                n->stat_unknown++; /* skipped, not fatal: an older node meeting a newer manager */
            break;
        }
    }
}

/* A frame arrived while our window was open. Returns true if it was a downstream CONTROL for us
   and we acted on it. `station`/`sequence` are the peeked header fields. */
static inline bool idep_on_frame(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t *const buf, const size_t len, bool *const reboot_out) {
    uint8_t variant = 0;
    uint16_t station = 0, sequence = 0;
    if (len < 4)
        return false;
    variant = (uint8_t)((buf[0] >> 4) & 0x0F);
    station = (uint16_t)(((uint16_t)(buf[0] & 0x0F) << 8) | buf[1]);
    sequence = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);

    /* mesh frames are not ours to read, and an ordinary sequence means someone else's telemetry */
    if (variant == IOTDATA_VARIANT_MESH || !iotdata_node_is_down(sequence))
        return false;
    if (!iotdata_node_addressed_to(station, n->station))
        return false;

    static iotdata_decoder_t dec; /* ~2KB: never on an end device's stack */
    if (iotdata_decode(buf, len, &dec) != IOTDATA_OK || dec.tlv_count == 0)
        return false;
    n->stat_rx++;
    bool acted = false;
    for (uint8_t i = 0; i < dec.tlv_count; i++) {
        const iotdata_decoder_tlv_t *const t = &dec.tlv[i];
        if (t->type != IOTDATA_NODE_TLV_CONTROL || t->format != IOTDATA_TLV_FMT_RAW) {
            n->stat_unknown++;
            continue;
        }
        idep_process_control(cfg, n, t->raw, t->length, reboot_out);
        acted = true;
    }
    return acted;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_ENDPOINT_H */
