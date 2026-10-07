#ifndef IOTDATA_NODE_DIAGNOSTICS_H
#define IOTDATA_NODE_DIAGNOSTICS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_diagnostics.h - what a node RECORDED: the single source of truth for DIAGNOSTICS.
//
// Two things live here, and the second is why this replaced iotdata_blackbox.h.
//
// THE ADAPTER binds the blackbox recorder (iotdata-depend/blackbox) to iotdata: the backend for
// this platform, the clock, and the one record type every node shares -- a lifecycle event. That
// part is unchanged; only the file name is.
//
// THE RECORDER is the scaffolding each app used to carry itself: a pool, a handle, a config, a
// did-it-start flag, start/event/flush, a tick, the node's DIAGNOSTICS and CONTROL hooks, and the
// console words. Three apps had ~60 near-identical lines of it, and they had drifted -- one flushed
// on a timer and two by hand, two declared the blackbox capability in VERSION and one did not, and
// none of them guarded a handle whose backend had failed to initialise. Turning the recorder on is
// now one #define.
//
//     #define IOTDATA_NODE_DIAGNOSTICS 2            /* before including this header */
//     ...
//     iotdata_node_diagnostics_begin(reason, cold); /* once, at startup */
//     iotdata_node_diagnostics_tick(now_ms);        /* each loop pass; flushes on its own schedule */
//
// and the node's hooks are iotdata_node_diagnostics_pull / _control / _control_actions, passed straight to
// node_begin or into an iotdata_node_params_t.
//
// EVERY app-facing call COMPILES TO NOTHING when IOTDATA_NODE_DIAGNOSTICS is 0. They are functions
// rather than macros so that the disabled build still type-checks its call sites -- a macro that
// expands to ((void)0) accepts arguments that no longer exist. Nothing in an application needs an
// #if around it.
//
// A NULL BACKEND IS NOT A DISABLED RECORDER. blackbox_init leaves a NULL backend inside a non-NULL
// handle when it fails -- no `diag` partition, an unwritable path -- and everything downstream
// dereferences it. That is a boot fault, not a degradation, and it is guarded once here instead of
// at each of the ten places that reach the store.
//
// SINGLETON, ON PURPOSE. One recorder per board, because the records are the board's: a simulator
// standing up eight virtual sensors has one log, and a request to any of its stations reads it.
// Include this in the app's unity TU, as its predecessor required.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Off unless an application says otherwise. The value chooses how far a record survives:
     0  off, compiled out entirely
     1  RAM only    -- an RTC_NOINIT ring: survives deep sleep and a watchdog or panic reset, but
                       not a power cycle
     2  RAM backed  -- the ring is written through to the `diag` partition on esp32, or to a file
                       on a host, so records outlive a power loss
   An application that manages its own handle -- the gateway, whose recorder is configured at run
   time from the command line -- leaves this at 0 and uses the adapter alone. */
#ifndef IOTDATA_NODE_DIAGNOSTICS
#define IOTDATA_NODE_DIAGNOSTICS 0
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ADAPTER: backend, sizes and clock
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(ESP_PLATFORM)
#ifndef BLACKBOX_PERSIST
#if IOTDATA_NODE_DIAGNOSTICS >= 2
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_ESP_FLASH
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE /* the RTC pool ring */
#endif
#endif
#ifndef IOTDATA_NODE_BLACKBOX_POOL_SZ
#define IOTDATA_NODE_BLACKBOX_POOL_SZ 2048u
#endif
#else /* host / linux */
#ifndef BLACKBOX_PERSIST
#if IOTDATA_NODE_DIAGNOSTICS == 1
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_FILE
#endif
#endif
#ifndef IOTDATA_NODE_BLACKBOX_POOL_SZ
#define IOTDATA_NODE_BLACKBOX_POOL_SZ 4096u
#endif
#endif

#ifndef BLACKBOX_CLOCK
#define BLACKBOX_CLOCK iotdata_node_blackbox_clock
#endif

/* stb-style: BLACKBOX_IMPLEMENTATION must be set BEFORE the first blackbox.h include. */
#ifdef IOTDATA_NODE_BLACKBOX_IMPLEMENTATION
#ifndef BLACKBOX_IMPLEMENTATION
#define BLACKBOX_IMPLEMENTATION
#endif
#endif
#include "blackbox.h"

/* -------- the one shared record: a lifecycle event -------------------------------------------- */
typedef enum {
    IOTDATA_NODE_BB_LC_BOOT = 0, /* cold boot / power-on                     */
    IOTDATA_NODE_BB_LC_START,    /* app started (post-init)                  */
    IOTDATA_NODE_BB_LC_STOP,     /* app stopping / shutdown                  */
    IOTDATA_NODE_BB_LC_SLEEP,    /* entering (deep) sleep                    */
    IOTDATA_NODE_BB_LC_WAKE,     /* woke from sleep                          */
    IOTDATA_NODE_BB_LC_RESET,    /* reset (reason in `reason`)               */
    IOTDATA_NODE_BB_LC_ERROR,    /* error / fault (code in `reason`)         */
} iotdata_node_bb_lc_event_t;

// @blackbox tag=LC
typedef struct {
    uint8_t event;
    uint8_t reason;
} iotdata_node_bb_lifecycle_t;

extern const blackbox_struct_config_t iotdata_node_blackbox_config_lifecycle;

/* -------- power: what is supplying this node, and what that supply is doing -------------------
 *
 * A node has RAILS, not "a power supply": a panel feeds a charger feeds a pack feeds a regulator
 * feeds the device, and each is a separate thing with separate facts. Every record below carries a
 * rail index, and PWS carries from_rail, so the log describes the actual graph rather than one
 * anonymous supply. Three records, by how often they have anything new to say:
 *
 *   PWS  what a rail IS, and how it is measured. Once per boot, and on change.
 *   PWD  one source-specific fact as key=value. The escape hatch: BMS topology today, per-cell
 *        voltages tomorrow, vendor detail forever -- no schema change, no length ceiling. A key
 *        that turns out permanent and load-bearing graduates to a field in PWS or PW.
 *   PWE  what a rail is DOING. The frequent one, and the event says why the line exists.
 *
 * Two kinds of rail, and the distinction decides which fields mean anything: a STORAGE source has
 * a volume (capacity_mah, pct), a FLOW source has only a rate (limit_mw, and pct is meaningless).
 * A REGULATED rail is neither -- it is an output, and what is interesting is what feeds it.
 *
 * Absent fields are EMPTY in the encoded line, not zero. A recorder that cannot say "not measured"
 * invites a reader to believe a zero, and most of these fields are unmeasurable on most hardware.
 * Initialise with IOTDATA_NODE_BB_POWER_EVENT_INIT / _SOURCE_INIT so a struct never claims 0V by omission. */

#define IOTDATA_NODE_BB_PW_NO_I32  INT32_MIN
#define IOTDATA_NODE_BB_PW_NO_U32  0u /* a capacity or a rating of zero is not a fact */
#define IOTDATA_NODE_BB_PW_NO_PCT  0xFFu
#define IOTDATA_NODE_BB_PW_NO_DEGC INT8_MIN
#define IOTDATA_NODE_BB_PW_NO_RAIL 0xFFu /* and, in from_rail, "nothing feeds this: it is a root" */

typedef enum {
    IOTDATA_NODE_BB_PW_SAMPLE = 0,    /* a periodic reading (emitted on change, see the note below) */
    IOTDATA_NODE_BB_PW_BOOT,          /* the first reading of this session                         */
    IOTDATA_NODE_BB_PW_LOW,           /* crossed into low                                          */
    IOTDATA_NODE_BB_PW_CRITICAL,      /* crossed into critical                                     */
    IOTDATA_NODE_BB_PW_MINIMUM,       /* a new lowest reading ever seen on this rail               */
    IOTDATA_NODE_BB_PW_BROWNOUT_PRE,  /* the last reading BEFORE a brownout: where the floor IS     */
    IOTDATA_NODE_BB_PW_CHARGE_START,  /* began taking charge                                       */
    IOTDATA_NODE_BB_PW_CHARGE_STOP,   /* stopped                                                   */
    IOTDATA_NODE_BB_PW_SOURCE_CHANGE, /* a rail appeared, vanished, or was swapped                 */
    IOTDATA_NODE_BB_PW_FAULT,         /* the source, or the measurement of it, is faulty           */
} iotdata_node_bb_pw_event_t;

typedef enum {
    IOTDATA_NODE_BB_PW_TYPE_UNKNOWN = 0,
    IOTDATA_NODE_BB_PW_TYPE_BATTERY,   /* storage */
    IOTDATA_NODE_BB_PW_TYPE_SUPERCAP,  /* storage */
    IOTDATA_NODE_BB_PW_TYPE_SOLAR,     /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_MAINS,     /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_USB,       /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_POE,       /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_GENERATOR, /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_REGULATED, /* neither: an output rail, fed by from_rail */
} iotdata_node_bb_pw_type_t;

#define IOTDATA_NODE_BB_PW_FORM_DC 0
#define IOTDATA_NODE_BB_PW_FORM_AC 1

/* Chemistry of a storage rail. Its own numbering, with 0 = "not a chemistry": a caller maps its
   own battery type onto this rather than assuming the two agree. */
typedef enum {
    IOTDATA_NODE_BB_PW_CHEM_NONE = 0,
    IOTDATA_NODE_BB_PW_CHEM_LIION,
    IOTDATA_NODE_BB_PW_CHEM_LIPO,
    IOTDATA_NODE_BB_PW_CHEM_LIFEPO4,
    IOTDATA_NODE_BB_PW_CHEM_NIMH,
    IOTDATA_NODE_BB_PW_CHEM_LEAD,
    IOTDATA_NODE_BB_PW_CHEM_ALKALINE,
} iotdata_node_bb_pw_chem_t;

#define IOTDATA_NODE_BB_PW_FLAG_PRESENT     (1U << 0) /* a source is actually attached                */
#define IOTDATA_NODE_BB_PW_FLAG_CHARGING    (1U << 1)
#define IOTDATA_NODE_BB_PW_FLAG_DISCHARGING (1U << 2)
#define IOTDATA_NODE_BB_PW_FLAG_EXTERNAL    (1U << 3) /* running on something other than the storage  */
#define IOTDATA_NODE_BB_PW_FLAG_LOW         (1U << 4)
#define IOTDATA_NODE_BB_PW_FLAG_CRITICAL    (1U << 5)
#define IOTDATA_NODE_BB_PW_FLAG_LIMITED     (1U << 6) /* at the source's RATING, not at its capacity  */
#define IOTDATA_NODE_BB_PW_FLAG_FAULT       (1U << 7)

// @blackbox tag=PWE
typedef struct {
    uint8_t event; /* iotdata_node_bb_pw_event_t                                                     */
    uint8_t rail;
    int32_t mv; /* signed, and wide: negative rails exist and 230V mains is 230000              */
    int32_t ua; /* signed: + flowing INTO the load, - INTO the source (charging). uA to 2147A   */
    int32_t mw; /* signed, same convention. Kept although V*I: on AC it is NOT derivable        */
    uint8_t pct;
    int8_t degc;   /* the SOURCE's temperature: its capacity and impedance both move with it    */
    uint8_t flags; /* IOTDATA_NODE_BB_PW_FLAG_*                                                      */
} iotdata_node_bb_power_event_t;

// @blackbox tag=PWS
typedef struct {
    uint8_t rail;
    uint8_t from_rail; /* what feeds this one, or IOTDATA_NODE_BB_PW_NO_RAIL for a root */
    uint8_t type;      /* iotdata_node_bb_pw_type_t */
    uint8_t form;      /* IOTDATA_NODE_BB_PW_FORM_* */
    uint8_t hz;        /* AC only */
    uint8_t chem;      /* storage only */
    uint8_t cells;     /* storage only: cells in series */
    int32_t nominal_mv, min_mv, max_mv;
    uint32_t capacity_mah; /* STORAGE: the volume  */
    uint32_t limit_mw;     /* FLOW: the rating     */
    uint16_t ratio_x100;   /* how it is measured.. */
    int16_t offset_mv;     /* ..and its calibration, so the numbers are still trustworthy later */
} iotdata_node_bb_power_source_t;

// @blackbox tag=PWD
typedef struct {
    uint8_t rail;
    char key[16];
    char value[48]; /* last in the line, so it may contain commas */
} iotdata_node_bb_power_detail_t;

#define IOTDATA_NODE_BB_POWER_EVENT_INIT(rail_) \
    { .event = IOTDATA_NODE_BB_PW_SAMPLE, .rail = (rail_), .mv = IOTDATA_NODE_BB_PW_NO_I32, .ua = IOTDATA_NODE_BB_PW_NO_I32, .mw = IOTDATA_NODE_BB_PW_NO_I32, .pct = IOTDATA_NODE_BB_PW_NO_PCT, .degc = IOTDATA_NODE_BB_PW_NO_DEGC, .flags = 0 }

#define IOTDATA_NODE_BB_POWER_SOURCE_INIT(rail_, type_) \
    { .rail = (rail_), \
      .from_rail = IOTDATA_NODE_BB_PW_NO_RAIL, \
      .type = (uint8_t)(type_), \
      .form = IOTDATA_NODE_BB_PW_FORM_DC, \
      .hz = 0, \
      .chem = IOTDATA_NODE_BB_PW_CHEM_NONE, \
      .cells = 0, \
      .nominal_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .min_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .max_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .capacity_mah = IOTDATA_NODE_BB_PW_NO_U32, \
      .limit_mw = IOTDATA_NODE_BB_PW_NO_U32, \
      .ratio_x100 = 0, \
      .offset_mv = 0 }

extern const blackbox_struct_config_t iotdata_node_blackbox_config_power;
extern const blackbox_struct_config_t iotdata_node_blackbox_config_power_source;
extern const blackbox_struct_config_t iotdata_node_blackbox_config_power_detail;

/* iotdata_node_blackbox_clock (the BLACKBOX_CLOCK hook) is already forward-declared by blackbox.h. */

/* Seed the clock. Call ONCE before blackbox_init(). On esp32 the clock's `seq` lives in RTC_NOINIT,
 * so this zeroes it on a true cold start (detected with a magic word -- see the implementation for
 * why esp_reset_reason() cannot be used for that). A no-op on the host. iotdata_node_diagnostics_begin
 * calls it, so an app driving the singleton never needs to. */
void iotdata_node_blackbox_begin(void);

/* Convenience: record a lifecycle event against any handle. */
static inline int iotdata_node_blackbox_lifecycle(blackbox_handle_t *h, iotdata_node_bb_lc_event_t ev, uint8_t reason) {
    const iotdata_node_bb_lifecycle_t r = { (uint8_t)ev, reason };
    return blackbox_insert(h, &iotdata_node_blackbox_config_lifecycle, &r);
}

/* Convenience: record a power state, a rail description, or one detail, against any handle. */
static inline int iotdata_node_blackbox_power(blackbox_handle_t *h, const iotdata_node_bb_power_event_t *const r) {
    return blackbox_insert(h, &iotdata_node_blackbox_config_power, r);
}

static inline int iotdata_node_blackbox_power_source(blackbox_handle_t *h, const iotdata_node_bb_power_source_t *const r) {
    return blackbox_insert(h, &iotdata_node_blackbox_config_power_source, r);
}

/* The strings are COPIED, bounded by the record: a key is a word and a value is a short phrase, and
   that bound is per attribute rather than per record-set, which is the point of one fact per line. */
static inline int iotdata_node_blackbox_power_detail(blackbox_handle_t *h, const uint8_t rail, const char *const key, const char *const value) {
    iotdata_node_bb_power_detail_t r = { .rail = rail, .key = { 0 }, .value = { 0 } };
    (void)snprintf(r.key, sizeof(r.key), "%s", key != NULL ? key : "");
    (void)snprintf(r.value, sizeof(r.value), "%s", value != NULL ? value : "");
    return blackbox_insert(h, &iotdata_node_blackbox_config_power_detail, &r);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE RECORDER
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Where a line of output goes. The same seam the rest of the framework uses: nothing here logs on
   its own, the application says where a line belongs. It is a CURRENT emitter rather than an
   argument on every call because that is how it is actually used -- a node points it at the log
   and a console points it at itself for the length of one command, and a store dumped a chunk per
   loop pass outlives whichever call started it. */
typedef void (*iotdata_node_diagnostics_emit_fn)(const char *line);

/* A lifecycle event's `reason` is ONE BYTE, and an application usually has a wider counter to put
   in it. Narrow it through here rather than with a cast: a cast turns 300 failures into 44, which
   reads as a smaller problem than it is, where a pegged 255 reads as "at least this many". The old
   BLACKBOX_EVENT macro cast internally, which is why no call site had to think about it. */
static inline uint8_t iotdata_node_diagnostics_reason(const uint32_t count) {
    return (uint8_t)((count > 255u) ? 255u : count);
}

#if IOTDATA_NODE_DIAGNOSTICS

#ifndef IOTDATA_NODE_DIAGNOSTICS_PARTITION
#define IOTDATA_NODE_DIAGNOSTICS_PARTITION "diag" /* esp32: the partition label. host: the file path. */
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_FLUSH
#define IOTDATA_NODE_DIAGNOSTICS_FLUSH BLACKBOX_FLUSH_BATCH_TIME
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS
#define IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS 60000u /* only consulted by the BATCH_TIME policy */
#endif
/* Whether this node services DIAGNOSTICS_DUMP, which prints the store on the node's own console.
   ON by default -- every node has one, and a sensor on the bench over USB is exactly when you want
   it. The obligation that comes with it is iotdata_node_diagnostics_pump() every loop pass: a dump is
   drained a chunk at a time, so a node that advertises DUMP and never pumps accepts the command
   and then produces nothing. Turn it OFF on a node that cannot pump -- and it then disappears from
   the advertised list as well as the handler, so the node is not promising what it will not do. */
#ifndef IOTDATA_NODE_DIAGNOSTICS_DUMP
#define IOTDATA_NODE_DIAGNOSTICS_DUMP 1
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK
#define IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK 16 /* records per loop pass, so a dump cannot stall the loop */
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_STAT_MAX
#define IOTDATA_NODE_DIAGNOSTICS_STAT_MAX 192
#endif

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define IOTDATA_NODE_DIAGNOSTICS_POOL_ATTR RTC_NOINIT_ATTR /* survives a reset, so a panic is still readable */
#else
#define IOTDATA_NODE_DIAGNOSTICS_POOL_ATTR
#endif

static IOTDATA_NODE_DIAGNOSTICS_POOL_ATTR char _iotdata_node_diagnostics_pool[IOTDATA_NODE_BLACKBOX_POOL_SZ];
static blackbox_handle_t _iotdata_node_diagnostics_handle;
static bool _iotdata_node_diagnostics_ready = false;
static char _iotdata_node_diagnostics_line[IOTDATA_NODE_DIAGNOSTICS_STAT_MAX];
static char _iotdata_node_diagnostics_rec[BLACKBOX_LINE_MAX];
static uint32_t _iotdata_node_diagnostics_ticked_ms = 0;
static iotdata_node_diagnostics_emit_fn _iotdata_node_diagnostics_emit = NULL;
static struct {
    bool active;
    size_t cursor;
    int count;
} _iotdata_node_diagnostics_dump;

static const blackbox_config_t _iotdata_node_diagnostics_config = {
    .pool = _iotdata_node_diagnostics_pool,
    .pool_sz = sizeof(_iotdata_node_diagnostics_pool),
    .flush = IOTDATA_NODE_DIAGNOSTICS_FLUSH,
    .flush_ms = IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS,
    .persist_arg = IOTDATA_NODE_DIAGNOSTICS_PARTITION,
    .enabled = true, /* compiled in == collecting; the compile-time knob is the gate */
};

/* True once the store exists. Everything below asks first, because a failed init leaves a handle
   whose backend is NULL, and reaching through it faults rather than doing nothing. */
/* Where output goes from now on; returns what it was, so a console can put it back. NULL discards,
   which is what a node with nowhere to print wants. */
static inline iotdata_node_diagnostics_emit_fn iotdata_node_diagnostics_emit_set(const iotdata_node_diagnostics_emit_fn fn) {
    const iotdata_node_diagnostics_emit_fn was = _iotdata_node_diagnostics_emit;
    _iotdata_node_diagnostics_emit = fn;
    return was;
}

static inline void _iotdata_node_diagnostics_say(const char *const line) {
    if (_iotdata_node_diagnostics_emit != NULL)
        _iotdata_node_diagnostics_emit(line);
}

static inline bool iotdata_node_diagnostics_ready(void) {
    return _iotdata_node_diagnostics_ready;
}

/* For records of an application's own kind: blackbox_insert(iotdata_node_diagnostics_handle(), ...).
   NULL when the recorder did not start, which a caller must check as it would any handle. */
static inline blackbox_handle_t *iotdata_node_diagnostics_handle(void) {
    return _iotdata_node_diagnostics_ready ? &_iotdata_node_diagnostics_handle : NULL;
}

/* Start the recorder and log why we are running. `cold` distinguishes a boot from a wake, which is
   the difference between the two lifecycle events a sleeping node alternates between. Returns
   false if the store could not be opened -- worth logging, but never fatal: a node with no
   recorder still answers a diagnostics request, emptily. */
static inline bool iotdata_node_diagnostics_begin(const uint8_t reason, const bool cold) {
    iotdata_node_blackbox_begin(); /* seed the clock BEFORE init: see the note on the magic word */
    if (blackbox_init(&_iotdata_node_diagnostics_handle, &_iotdata_node_diagnostics_config) != 0)
        return false;
    _iotdata_node_diagnostics_ready = true;
    (void)iotdata_node_blackbox_lifecycle(&_iotdata_node_diagnostics_handle, cold ? IOTDATA_NODE_BB_LC_BOOT : IOTDATA_NODE_BB_LC_WAKE, reason);
    return true;
}

static inline void iotdata_node_diagnostics_event(const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_lifecycle(&_iotdata_node_diagnostics_handle, ev, reason);
}

static inline void iotdata_node_diagnostics_power(const iotdata_node_bb_power_event_t *const r) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power(&_iotdata_node_diagnostics_handle, r);
}

static inline void iotdata_node_diagnostics_power_source(const iotdata_node_bb_power_source_t *const r) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power_source(&_iotdata_node_diagnostics_handle, r);
}

static inline void iotdata_node_diagnostics_power_detail(const uint8_t rail, const char *const key, const char *const value) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power_detail(&_iotdata_node_diagnostics_handle, rail, key, value);
}

static inline void iotdata_node_diagnostics_flush(void) {
    if (_iotdata_node_diagnostics_ready)
        (void)blackbox_flush(&_iotdata_node_diagnostics_handle);
}

static inline void iotdata_node_diagnostics_enable(const bool on) {
    if (_iotdata_node_diagnostics_ready)
        blackbox_enable(&_iotdata_node_diagnostics_handle, on);
}

static inline void iotdata_node_diagnostics_clear(void) {
    if (_iotdata_node_diagnostics_ready) {
        _iotdata_node_diagnostics_dump.active = false; /* a dump in flight has a cursor about to go stale */
        blackbox_clear(&_iotdata_node_diagnostics_handle);
    }
}

/* One record per call, from `cursor` (start it at 0), 0 when there are no more. This is the shape
   both node layers want for their diagnostics report: node_diag_fn and iotdata_node_diag_fn. */
static inline size_t iotdata_node_diagnostics_pull(size_t *const cursor, char *const out, const size_t outsize) {
    if (!_iotdata_node_diagnostics_ready)
        return 0;
    const int n = blackbox_pull(&_iotdata_node_diagnostics_handle, cursor, out, outsize);
    return (n > 0) ? strlen(out) : 0;
}

/* The recorder's own housekeeping: its flush policy runs on this. Give it the same millisecond
   clock the rest of the loop uses; it works out its own interval. */
static inline void iotdata_node_diagnostics_tick(const uint32_t now_ms) {
    if (!_iotdata_node_diagnostics_ready)
        return;
    if (_iotdata_node_diagnostics_ticked_ms == 0)
        _iotdata_node_diagnostics_ticked_ms = now_ms;
    blackbox_tick(&_iotdata_node_diagnostics_handle, now_ms - _iotdata_node_diagnostics_ticked_ms);
    _iotdata_node_diagnostics_ticked_ms = now_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING IT BACK
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_diagnostics_stat(void) {
    blackbox_status_t st;
    if (!_iotdata_node_diagnostics_ready)
        _iotdata_node_diagnostics_say("diag: unavailable (recorder did not start)");
    else if (blackbox_status(&_iotdata_node_diagnostics_handle, &st)) {
        (void)blackbox_status_str(&st, BLACKBOX_STATUS_ALL, _iotdata_node_diagnostics_line, sizeof(_iotdata_node_diagnostics_line));
        _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_line);
    }
}

/*
 * A dump is STARTED here and drained by iotdata_node_diagnostics_pump on later passes, a chunk at a
 * time: a full store is thousands of records and writing them in one call would stall the loop
 * long enough to lose frames. The flush first is what makes the dump cover the staged pool as well
 * as the store.
 */
static inline void iotdata_node_diagnostics_dump_start(void) {
    if (!_iotdata_node_diagnostics_ready) {
        _iotdata_node_diagnostics_say("diag: unavailable (recorder did not start)");
        return;
    }
    iotdata_node_diagnostics_flush();
    _iotdata_node_diagnostics_dump.cursor = 0;
    _iotdata_node_diagnostics_dump.count = 0;
    _iotdata_node_diagnostics_dump.active = true;
    _iotdata_node_diagnostics_say("diag: dump begin");
}

/* Call every loop pass. Emits at most IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK records and returns whether a
   dump is still running. `emit` is asked for on every pass rather than remembered from the start,
   because by the time the records flow the console's turn to speak is usually over. */
static inline bool iotdata_node_diagnostics_pump(void) {
    if (!_iotdata_node_diagnostics_dump.active || !_iotdata_node_diagnostics_ready)
        return false;
    for (int i = 0; i < IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK; i++) {
        if (blackbox_pull(&_iotdata_node_diagnostics_handle, &_iotdata_node_diagnostics_dump.cursor, _iotdata_node_diagnostics_rec, sizeof(_iotdata_node_diagnostics_rec)) <= 0) {
            (void)snprintf(_iotdata_node_diagnostics_line, sizeof(_iotdata_node_diagnostics_line), "diag: dump end (%d record(s))", _iotdata_node_diagnostics_dump.count);
            _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_line);
            _iotdata_node_diagnostics_dump.active = false;
            return false;
        }
        _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_rec);
        _iotdata_node_diagnostics_dump.count++;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE NODE'S CONTROL KEYS
//
// What a device with a recorder accepts over the air, and the one handler for it. ENABLE and CLEAR
// are always here; DUMP is too unless IOTDATA_NODE_DIAGNOSTICS_DUMP is off, in which case it is absent
// from BOTH the advertised list and the handler -- a node that advertises a key it will not service
// sends whoever asked on a wild goose chase.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* (subject, action) PAIRS, which is the shape the inventory emits and the shape a command takes on
   the wire -- so advertising one and sending one cannot drift apart. */
static const uint8_t iotdata_node_diagnostics_control_actions[] = {
    IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_ENABLE, IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_CLEAR,
#if IOTDATA_NODE_DIAGNOSTICS_DUMP
    IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_DUMP,
#endif
};
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT ((uint8_t)(sizeof(iotdata_node_diagnostics_control_actions) / (2 * sizeof(iotdata_node_diagnostics_control_actions[0]))))

/*
 * What to put in a node's config. These are the FUNCTIONS when the recorder is compiled in and
 * NULL when it is not, which is the distinction the node layer reads: a NULL diag hook means this
 * device keeps no diagnostics at all and its report is empty, while a present one that returns
 * nothing means it keeps a blackbox that happens to be empty. Passing the function unconditionally
 * would make every node claim a recorder. Both are usable with no #if at the call site.
 */
#define IOTDATA_NODE_DIAGNOSTICS_PULL                  iotdata_node_diagnostics_pull
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL               iotdata_node_diagnostics_node_control

/* Returns whether the command was one of ours. An app's own control hook offers everything it does
   not know to this, then falls through to its own. */
static inline bool iotdata_node_diagnostics_control(const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    if (!_iotdata_node_diagnostics_ready)
        return false; /* advertised, but not working today: counted unknown rather than pretended */
    if (subject != IOTDATA_NODE_TLV_DIAGNOSTICS)
        return false;
    switch (action) {
    case IOTDATA_NODE_ACTION_DIAGNOSTICS_ENABLE: {
        const bool on = (arglen >= 1) ? (args[0] != 0u) : true;
        iotdata_node_diagnostics_enable(on);
        _iotdata_node_diagnostics_say(on ? "diag: enabled" : "diag: disabled");
        return true;
    }
    case IOTDATA_NODE_ACTION_DIAGNOSTICS_CLEAR:
        iotdata_node_diagnostics_clear();
        _iotdata_node_diagnostics_say("diag: cleared");
        return true;
#if IOTDATA_NODE_DIAGNOSTICS_DUMP
    case IOTDATA_NODE_ACTION_DIAGNOSTICS_DUMP:
        iotdata_node_diagnostics_dump_start();
        return true;
#endif
    default:
        return false;
    }
}

/* The same handler in the shape an end device's node layer wants (iotdata_node_control_fn). The station is
   narration only: there is ONE recorder per board, so a request to any station a board stands up
   reads and manages the same log. A relay or gateway, whose hook carries a clock instead, calls
   iotdata_node_diagnostics_control directly from its own default branch. */
static inline bool iotdata_node_diagnostics_node_control(const uint16_t station, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    (void)station;
    return iotdata_node_diagnostics_control(subject, action, args, arglen);
}

#else /* !IOTDATA_NODE_DIAGNOSTICS -- compiled out, but every call site still type-checks */

static const uint8_t *const iotdata_node_diagnostics_control_actions = NULL;
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT ((uint8_t)0)
/* NULL, so a node with no recorder reports no diagnostics rather than an empty blackbox */
#define IOTDATA_NODE_DIAGNOSTICS_PULL                  NULL
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL               NULL

static inline iotdata_node_diagnostics_emit_fn iotdata_node_diagnostics_emit_set(const iotdata_node_diagnostics_emit_fn fn) {
    (void)fn;
    return NULL;
}
static inline bool iotdata_node_diagnostics_ready(void) {
    return false;
}
static inline blackbox_handle_t *iotdata_node_diagnostics_handle(void) {
    return NULL;
}
static inline bool iotdata_node_diagnostics_begin(const uint8_t reason, const bool cold) {
    (void)reason;
    (void)cold;
    return false;
}
static inline void iotdata_node_diagnostics_event(const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    (void)ev;
    (void)reason;
}
static inline void iotdata_node_diagnostics_power(const iotdata_node_bb_power_event_t *const r) {
    (void)r;
}
static inline void iotdata_node_diagnostics_power_source(const iotdata_node_bb_power_source_t *const r) {
    (void)r;
}
static inline void iotdata_node_diagnostics_power_detail(const uint8_t rail, const char *const key, const char *const value) {
    (void)rail;
    (void)key;
    (void)value;
}
static inline void iotdata_node_diagnostics_flush(void) {
}
static inline void iotdata_node_diagnostics_enable(const bool on) {
    (void)on;
}
static inline void iotdata_node_diagnostics_clear(void) {
}
static inline size_t iotdata_node_diagnostics_pull(size_t *const cursor, char *const out, const size_t outsize) {
    (void)cursor;
    (void)out;
    (void)outsize;
    return 0;
}
static inline void iotdata_node_diagnostics_tick(const uint32_t now_ms) {
    (void)now_ms;
}
static inline void iotdata_node_diagnostics_stat(void) {
}
static inline void iotdata_node_diagnostics_dump_start(void) {
}
static inline bool iotdata_node_diagnostics_pump(void) {
    return false;
}
static inline bool iotdata_node_diagnostics_control(const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    (void)subject;
    (void)action;
    (void)args;
    (void)arglen;
    return false;
}
static inline bool iotdata_node_diagnostics_node_control(const uint16_t station, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    (void)station;
    (void)subject;
    (void)action;
    (void)args;
    (void)arglen;
    return false;
}

#endif /* IOTDATA_NODE_DIAGNOSTICS */

// -----------------------------------------------------------------------------------------------------------------------------------------
// ADAPTER IMPLEMENTATION
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_NODE_BLACKBOX_IMPLEMENTATION

static int iotdata_node_bb__enc_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_lifecycle_t *r = (const iotdata_node_bb_lifecycle_t *)data;
    return snprintf(out, n, "%u,%u", (unsigned)r->event, (unsigned)r->reason);
}
static int iotdata_node_bb__dec_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_lifecycle_t *r = (iotdata_node_bb_lifecycle_t *)data;
    unsigned e = 0, rs = 0;
    if (sscanf(in, "%u,%u", &e, &rs) != 2)
        return -1;
    r->event = (uint8_t)e;
    r->reason = (uint8_t)rs;
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_lifecycle = { "LC", iotdata_node_bb__enc_lifecycle, iotdata_node_bb__dec_lifecycle };

/* Power. Fields are positional and MAY BE EMPTY, which sscanf cannot express, so the decoders walk
   to a field and parse from there -- strtol stops at the comma of its own accord, and an empty
   field yields the caller's "unknown". The value of a PWD is last so it may contain commas. */

static const char *iotdata_node_bb__field(const char *in, const int idx) {
    for (int i = 0; i < idx && in != NULL; i++)
        in = (in = strchr(in, ',')) != NULL ? in + 1 : NULL;
    return in;
}
static bool iotdata_node_bb__field_absent(const char *const f) {
    return f == NULL || *f == '\0' || *f == ',' || *f == '\n';
}
static long iotdata_node_bb__field_num(const char *const in, const int idx, const long absent) {
    const char *const f = iotdata_node_bb__field(in, idx);
    return iotdata_node_bb__field_absent(f) ? absent : strtol(f, NULL, 10);
}
static void iotdata_node_bb__field_str(const char *const in, const int idx, char *const out, const size_t outlen, const bool to_end) {
    const char *const f = iotdata_node_bb__field(in, idx);
    out[0] = '\0';
    if (iotdata_node_bb__field_absent(f))
        return;
    const char *end = to_end ? strchr(f, '\n') : strchr(f, ',');
    const size_t len = (end != NULL) ? (size_t)(end - f) : strlen(f);
    (void)snprintf(out, outlen, "%.*s", (int)(len < outlen - 1 ? len : outlen - 1), f);
}

/* An absent numeric field is printed as nothing at all: ",,". */
#define _IOTDATA_NODE_BB_PW_NUM(buf, val, absent) \
    do { \
        (buf)[0] = '\0'; \
        if ((val) != (absent)) \
            (void)snprintf((buf), sizeof(buf), "%ld", (long)(val)); \
    } while (0)

static int iotdata_node_bb__enc_power(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_event_t *r = (const iotdata_node_bb_power_event_t *)data;
    char mv[12], ua[12], mw[12], pct[5], degc[6];
    _IOTDATA_NODE_BB_PW_NUM(mv, r->mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(ua, r->ua, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(mw, r->mw, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(pct, r->pct, IOTDATA_NODE_BB_PW_NO_PCT);
    _IOTDATA_NODE_BB_PW_NUM(degc, r->degc, IOTDATA_NODE_BB_PW_NO_DEGC);
    return snprintf(out, n, "%u,%u,%s,%s,%s,%s,%s,%u", (unsigned)r->event, (unsigned)r->rail, mv, ua, mw, pct, degc, (unsigned)r->flags);
}
static int iotdata_node_bb__dec_power(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_event_t *r = (iotdata_node_bb_power_event_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 1)))
        return -1; /* event and rail are the only two that are never absent */
    r->event = (uint8_t)iotdata_node_bb__field_num(in, 0, 0);
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 1, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->mv = (int32_t)iotdata_node_bb__field_num(in, 2, IOTDATA_NODE_BB_PW_NO_I32);
    r->ua = (int32_t)iotdata_node_bb__field_num(in, 3, IOTDATA_NODE_BB_PW_NO_I32);
    r->mw = (int32_t)iotdata_node_bb__field_num(in, 4, IOTDATA_NODE_BB_PW_NO_I32);
    r->pct = (uint8_t)iotdata_node_bb__field_num(in, 5, IOTDATA_NODE_BB_PW_NO_PCT);
    r->degc = (int8_t)iotdata_node_bb__field_num(in, 6, IOTDATA_NODE_BB_PW_NO_DEGC);
    r->flags = (uint8_t)iotdata_node_bb__field_num(in, 7, 0);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power = { "PWE", iotdata_node_bb__enc_power, iotdata_node_bb__dec_power };

static int iotdata_node_bb__enc_power_source(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_source_t *r = (const iotdata_node_bb_power_source_t *)data;
    char nom[12], lo[12], hi[12], cap[12], lim[12], ratio[8], off[8];
    _IOTDATA_NODE_BB_PW_NUM(nom, r->nominal_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(lo, r->min_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(hi, r->max_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(cap, r->capacity_mah, IOTDATA_NODE_BB_PW_NO_U32);
    _IOTDATA_NODE_BB_PW_NUM(lim, r->limit_mw, IOTDATA_NODE_BB_PW_NO_U32);
    _IOTDATA_NODE_BB_PW_NUM(ratio, r->ratio_x100, 0);
    _IOTDATA_NODE_BB_PW_NUM(off, r->offset_mv, 0);
    return snprintf(out, n, "%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s", (unsigned)r->rail, (unsigned)r->from_rail, (unsigned)r->type, (unsigned)r->form, (unsigned)r->hz, (unsigned)r->chem, (unsigned)r->cells, nom, lo, hi, cap, lim, ratio,
                    off);
}
static int iotdata_node_bb__dec_power_source(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_source_t *r = (iotdata_node_bb_power_source_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 2)))
        return -1;
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 0, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->from_rail = (uint8_t)iotdata_node_bb__field_num(in, 1, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->type = (uint8_t)iotdata_node_bb__field_num(in, 2, IOTDATA_NODE_BB_PW_TYPE_UNKNOWN);
    r->form = (uint8_t)iotdata_node_bb__field_num(in, 3, IOTDATA_NODE_BB_PW_FORM_DC);
    r->hz = (uint8_t)iotdata_node_bb__field_num(in, 4, 0);
    r->chem = (uint8_t)iotdata_node_bb__field_num(in, 5, IOTDATA_NODE_BB_PW_CHEM_NONE);
    r->cells = (uint8_t)iotdata_node_bb__field_num(in, 6, 0);
    r->nominal_mv = (int32_t)iotdata_node_bb__field_num(in, 7, IOTDATA_NODE_BB_PW_NO_I32);
    r->min_mv = (int32_t)iotdata_node_bb__field_num(in, 8, IOTDATA_NODE_BB_PW_NO_I32);
    r->max_mv = (int32_t)iotdata_node_bb__field_num(in, 9, IOTDATA_NODE_BB_PW_NO_I32);
    r->capacity_mah = (uint32_t)iotdata_node_bb__field_num(in, 10, IOTDATA_NODE_BB_PW_NO_U32);
    r->limit_mw = (uint32_t)iotdata_node_bb__field_num(in, 11, IOTDATA_NODE_BB_PW_NO_U32);
    r->ratio_x100 = (uint16_t)iotdata_node_bb__field_num(in, 12, 0);
    r->offset_mv = (int16_t)iotdata_node_bb__field_num(in, 13, 0);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power_source = { "PWS", iotdata_node_bb__enc_power_source, iotdata_node_bb__dec_power_source };

static int iotdata_node_bb__enc_power_detail(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_detail_t *r = (const iotdata_node_bb_power_detail_t *)data;
    return snprintf(out, n, "%u,%s,%s", (unsigned)r->rail, r->key, r->value);
}
static int iotdata_node_bb__dec_power_detail(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_detail_t *r = (iotdata_node_bb_power_detail_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 1)))
        return -1;
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 0, IOTDATA_NODE_BB_PW_NO_RAIL);
    iotdata_node_bb__field_str(in, 1, r->key, sizeof(r->key), false);
    iotdata_node_bb__field_str(in, 2, r->value, sizeof(r->value), true);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power_detail = { "PWD", iotdata_node_bb__enc_power_detail, iotdata_node_bb__dec_power_detail };

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_timer.h"

/* esp32 clock = "seq:up_ms". `seq` is a monotonic counter in RTC_NOINIT so it survives deep sleep and
 * resets -- which also means it holds GARBAGE on a true cold start.
 *
 * You cannot detect that from esp_reset_reason(): a USB-powered ESP32-C3 reports ESP_RST_USB on a
 * genuine power-up, NOT ESP_RST_POWERON (the ROM logs it as rst:0x15 USB_UART_CHIP_RESET), so an
 * `if (reason == ESP_RST_POWERON) seq = 0;` guard silently never fires and the garbage persists
 * through every later reset. Pair it with a magic word instead, exactly as an RTC state struct is.
 *
 * Both variables are owned HERE rather than by each app, so this cannot be got wrong per project.
 * Call iotdata_node_blackbox_begin() once before blackbox_init(). */
#define IOTDATA_NODE_BLACKBOX_SEQ_MAGIC 0x1D5EC10Cu /* 'IDSEC-CLOC(k)' */

RTC_NOINIT_ATTR uint32_t iotdata_node_blackbox_seq;
RTC_NOINIT_ATTR static uint32_t iotdata_node_blackbox_seq_magic;

void iotdata_node_blackbox_begin(void) {
    if (iotdata_node_blackbox_seq_magic != IOTDATA_NODE_BLACKBOX_SEQ_MAGIC) { /* RTC was garbage: cold start */
        iotdata_node_blackbox_seq = 0;
        iotdata_node_blackbox_seq_magic = IOTDATA_NODE_BLACKBOX_SEQ_MAGIC;
    }
}

int iotdata_node_blackbox_clock(char *out, size_t n) {
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    return snprintf(out, n, "%u:%u", (unsigned)(++iotdata_node_blackbox_seq), (unsigned)up);
}
#else
void iotdata_node_blackbox_begin(void) { /* host: the clock is a real timestamp, nothing to seed */
}
#include <time.h>
int iotdata_node_blackbox_clock(char *out, size_t n) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC); /* C11 standard — no POSIX feature-test macro needed */
    const long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return snprintf(out, n, "%lld", ms);
}
#endif

#endif /* IOTDATA_NODE_BLACKBOX_IMPLEMENTATION */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_DIAGNOSTICS_H */
