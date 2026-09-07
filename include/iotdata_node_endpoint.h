
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
#ifndef IDEP_KV_MAX
#define IDEP_KV_MAX 160
#endif
#ifndef IDEP_PACKET_MAX
#define IDEP_PACKET_MAX 240
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const char *firmware, *hardware, *platform, *application, *build, *serial;
} idep_version_t;

/* The app adds its own STATUS keys -- battery, temperature, whatever it has. Called with the
   builder mid-flight, so it just appends. */
typedef void (*idep_status_fn)(uint16_t station, iotdata_kvr_t *kv);
typedef bool (*idep_tx_fn)(const uint8_t *packet, size_t len);

typedef struct {
    const idep_version_t *version;
    idep_status_fn status;
    idep_tx_fn tx;
    uint32_t receive_every_ms;  /* 0 = never open a window: this node cannot be reached */
    uint16_t receive_window_ms; /* advertised, so a sender can skip one too short to use */
} idep_config_t;

/* Per station. On a deep-sleeping sensor this must live in RTC memory to survive the sleep. */
typedef struct {
    uint16_t station_id;
    uint16_t sequence;      /* our own, for frames we originate */
    uint32_t elapsed_ms;    /* since the last window closed */
    uint32_t window_end_ms; /* meaningful only while window_open */
    bool window_open;
    bool window_pending; /* due, and to be advertised in the next frame we send */
    uint32_t stat_rx, stat_requests, stat_reports, stat_commands, stat_unknown, stat_windows;
} idep_node_t;

static inline void idep_node_init(idep_node_t *const n, const uint16_t station_id) {
    memset(n, 0, sizeof(*n));
    n->station_id = station_id;
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

static inline bool idep_window_active(const idep_node_t *const n, const uint32_t now_ms) {
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
    const size_t n = iotdata_node_receive_build(kv, sizeof(kv), cfg->receive_window_ms, 0);
    return iotdata_encode_tlv(enc, IOTDATA_NODE_TLV_RECEIVE, kv, (uint8_t)n) == IOTDATA_OK;
}

/* The same advertisement as a frame of its own, for a node that cannot reach inside the telemetry
   frame it is about to send. Costs a whole frame instead of two bytes, so prefer the append. */
static inline bool idep_receive_announce(const idep_config_t *const cfg, idep_node_t *const n) {
    if (cfg->tx == NULL)
        return false;
    static iotdata_encoder_t enc;
    static uint8_t packet[IDEP_PACKET_MAX];
    size_t len = 0;
    if (iotdata_encode_begin(&enc, packet, sizeof(packet), 0, n->station_id, n->sequence) != IOTDATA_OK)
        return false;
    if (!idep_receive_append(cfg, &enc))
        return false;
    if (iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return false;
    n->sequence = iotdata_sequence_next(n->sequence);
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
    if (cfg->version != NULL) {
        idep_add_str(&kv, IOTDATA_NODE_VERSION_FIRMWARE, cfg->version->firmware);
        idep_add_str(&kv, IOTDATA_NODE_VERSION_HARDWARE, cfg->version->hardware);
        idep_add_str(&kv, IOTDATA_NODE_VERSION_PLATFORM, cfg->version->platform);
        idep_add_str(&kv, IOTDATA_NODE_VERSION_APPLICATION, cfg->version->application);
        idep_add_str(&kv, IOTDATA_NODE_VERSION_BUILD, cfg->version->build);
        idep_add_str(&kv, IOTDATA_NODE_VERSION_SERIAL, cfg->version->serial);
    }
    return kv.overflow ? -1 : (int)kv.len;
}

/* Unlike a relay or a gateway, an end device DOES produce telemetry, so this is the one place the
   variant suite is actually reported. The manifest goes in every chunk (see iotdata_node.h) and
   the cursor resumes where the last frame stopped -- a whole suite does not fit one frame. */
static inline int idep_build_variant(uint8_t *const buf, const size_t size, uint8_t *const cursor) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_VARIANT_MANIFEST, iotdata_node_variant_manifest());
    for (; *cursor <= IOTDATA_VARIANT_MAX; (*cursor)++) {
        const iotdata_variant_def_t *const d = iotdata_get_variant(*cursor);
        if (d == NULL)
            continue;
        static uint8_t val[IDEP_KV_MAX];
        const size_t n = iotdata_node_variant_encode(d, val, sizeof(val));
        if (n == 0)
            continue;
        if (kv.len + 2u + n > size)
            break; /* leave it for the next frame */
        iotdata_kvr_add(&kv, *cursor, val, (uint8_t)n);
    }
    if (*cursor > IOTDATA_VARIANT_MAX)
        *cursor = 0; /* wrapped: a later request starts the suite again */
    return kv.overflow ? -1 : (int)kv.len;
}

static inline int idep_build_control(uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REQUEST_VERSION);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REQUEST_VARIANT);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REQUEST_CONTROL);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REQUEST_STATUS);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REQUEST_CONFIG);
    iotdata_kvr_add_flag(&kv, IOTDATA_NODE_CONTROL_REBOOT);
    return kv.overflow ? -1 : (int)kv.len;
}

static inline int idep_build_status(const idep_config_t *const cfg, const idep_node_t *const n, uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    if (cfg->status != NULL)
        cfg->status(n->station_id, &kv); /* the app knows its own uptime, battery, heap */
    return kv.overflow ? -1 : (int)kv.len;
}

/* What a manager can see of our schedule. Read-only until config persistence exists. */
static inline int idep_build_config(const idep_config_t *const cfg, uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_RECEIVE_DURATION, cfg->receive_window_ms);
    iotdata_kvr_add_u16(&kv, IOTDATA_NODE_CONFIG_PERIOD_STATUS, (uint16_t)(cfg->receive_every_ms / 1000u));
    return kv.overflow ? -1 : (int)kv.len;
}

static inline int idep_build(const idep_config_t *const cfg, const idep_node_t *const n, const uint8_t type, uint8_t *const buf, const size_t size) {
    static uint8_t variant_cursor = 0;
    switch (type) {
    case IOTDATA_NODE_TLV_VERSION:
        return idep_build_version(cfg, buf, size);
    case IOTDATA_NODE_TLV_VARIANT:
        return idep_build_variant(buf, size, &variant_cursor);
    case IOTDATA_NODE_TLV_CONTROL:
        return idep_build_control(buf, size);
    case IOTDATA_NODE_TLV_STATUS:
        return idep_build_status(cfg, n, buf, size);
    case IOTDATA_NODE_TLV_CONFIG:
        return idep_build_config(cfg, buf, size);
    default:
        return -1;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// Reporting and control
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Reply with one system TLV, as an ordinary frame from us. A report says who SENT it, so there is
   no addressing to do -- whoever is listening republishes it. */
static inline bool idep_report(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t type) {
    if (cfg->tx == NULL)
        return false;
    static uint8_t kvbuf[IDEP_KV_MAX]; /* static: too large for a sensor's stack frame */
    const int kvlen = idep_build(cfg, n, type, kvbuf, sizeof(kvbuf));
    if (kvlen < 0)
        return false;
    static iotdata_encoder_t enc;
    static uint8_t packet[IDEP_PACKET_MAX];
    size_t len = 0;
    if (iotdata_encode_begin(&enc, packet, sizeof(packet), 0, n->station_id, n->sequence) != IOTDATA_OK)
        return false;
    if (iotdata_encode_tlv(&enc, type, kvbuf, (uint8_t)kvlen) != IOTDATA_OK)
        return false;
    if (iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return false;
    n->sequence = iotdata_sequence_next(n->sequence);
    n->stat_reports++;
    return cfg->tx(packet, len);
}

static inline void idep_process_control(const idep_config_t *const cfg, idep_node_t *const n, const uint8_t *const kv, const size_t kvlen, bool *const reboot_out) {
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(kv, kvlen, &cur, &key, &val, &vlen)) {
        const uint8_t want = iotdata_node_tlv_control_type(key);
        if (want != IOTDATA_NODE_TLV_NONE) {
            n->stat_requests++;
            (void)idep_report(cfg, n, want);
            continue;
        }
        switch (key) {
        case IOTDATA_NODE_CONTROL_REBOOT:
            n->stat_commands++;
            if (reboot_out != NULL)
                *reboot_out = true;
            break;
        default:
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
    if (!iotdata_node_addressed_to(station, n->station_id))
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
