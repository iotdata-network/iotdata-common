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
//     #define IOTDATA_DIAGNOSTICS 2            /* before including this header */
//     ...
//     iotdata_diagnostics_begin(reason, cold); /* once, at startup */
//     iotdata_diagnostics_tick(now_ms);        /* each loop pass; flushes on its own schedule */
//
// and the node's hooks are iotdata_diagnostics_pull / _control / _control_keys, passed straight to
// node_begin or into an idep_config_t.
//
// EVERY app-facing call COMPILES TO NOTHING when IOTDATA_DIAGNOSTICS is 0. They are functions
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
#ifndef IOTDATA_DIAGNOSTICS
#define IOTDATA_DIAGNOSTICS 0
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ADAPTER: backend, sizes and clock
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(ESP_PLATFORM)
#ifndef BLACKBOX_PERSIST
#if IOTDATA_DIAGNOSTICS >= 2
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_ESP_FLASH
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE /* the RTC pool ring */
#endif
#endif
#ifndef IOTDATA_BLACKBOX_POOL_SZ
#define IOTDATA_BLACKBOX_POOL_SZ 2048u
#endif
#else /* host / linux */
#ifndef BLACKBOX_PERSIST
#if IOTDATA_DIAGNOSTICS == 1
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_FILE
#endif
#endif
#ifndef IOTDATA_BLACKBOX_POOL_SZ
#define IOTDATA_BLACKBOX_POOL_SZ 4096u
#endif
#endif

#ifndef BLACKBOX_CLOCK
#define BLACKBOX_CLOCK iotdata_blackbox_clock
#endif

/* stb-style: BLACKBOX_IMPLEMENTATION must be set BEFORE the first blackbox.h include. */
#ifdef IOTDATA_BLACKBOX_IMPLEMENTATION
#ifndef BLACKBOX_IMPLEMENTATION
#define BLACKBOX_IMPLEMENTATION
#endif
#endif
#include "blackbox.h"

/* -------- the one shared record: a lifecycle event -------------------------------------------- */
typedef enum {
    IOTDATA_BB_LC_BOOT = 0, /* cold boot / power-on                     */
    IOTDATA_BB_LC_START,    /* app started (post-init)                  */
    IOTDATA_BB_LC_STOP,     /* app stopping / shutdown                  */
    IOTDATA_BB_LC_SLEEP,    /* entering (deep) sleep                    */
    IOTDATA_BB_LC_WAKE,     /* woke from sleep                          */
    IOTDATA_BB_LC_RESET,    /* reset (reason in `reason`)               */
    IOTDATA_BB_LC_ERROR,    /* error / fault (code in `reason`)         */
} iotdata_bb_lc_event_t;

// @blackbox tag=LC
typedef struct {
    uint8_t event;
    uint8_t reason;
} iotdata_bb_lifecycle_t;

extern const blackbox_struct_config_t iotdata_blackbox_config_lifecycle;

/* iotdata_blackbox_clock (the BLACKBOX_CLOCK hook) is already forward-declared by blackbox.h. */

/* Seed the clock. Call ONCE before blackbox_init(). On esp32 the clock's `seq` lives in RTC_NOINIT,
 * so this zeroes it on a true cold start (detected with a magic word -- see the implementation for
 * why esp_reset_reason() cannot be used for that). A no-op on the host. iotdata_diagnostics_begin
 * calls it, so an app driving the singleton never needs to. */
void iotdata_blackbox_begin(void);

/* Convenience: record a lifecycle event against any handle. */
static inline int iotdata_blackbox_lifecycle(blackbox_handle_t *h, iotdata_bb_lc_event_t ev, uint8_t reason) {
    const iotdata_bb_lifecycle_t r = { (uint8_t)ev, reason };
    return blackbox_insert(h, &iotdata_blackbox_config_lifecycle, &r);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE RECORDER
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Where a line of output goes. The same seam the rest of the framework uses: nothing here logs on
   its own, the application says where a line belongs. It is a CURRENT emitter rather than an
   argument on every call because that is how it is actually used -- a node points it at the log
   and a console points it at itself for the length of one command, and a store dumped a chunk per
   loop pass outlives whichever call started it. */
typedef void (*iotdata_diagnostics_emit_fn)(const char *line);

/* A lifecycle event's `reason` is ONE BYTE, and an application usually has a wider counter to put
   in it. Narrow it through here rather than with a cast: a cast turns 300 failures into 44, which
   reads as a smaller problem than it is, where a pegged 255 reads as "at least this many". The old
   BLACKBOX_EVENT macro cast internally, which is why no call site had to think about it. */
static inline uint8_t iotdata_diagnostics_reason(const uint32_t count) {
    return (uint8_t)((count > 255u) ? 255u : count);
}

#if IOTDATA_DIAGNOSTICS

#ifndef IOTDATA_DIAGNOSTICS_PARTITION
#define IOTDATA_DIAGNOSTICS_PARTITION "diag" /* esp32: the partition label. host: the file path. */
#endif
#ifndef IOTDATA_DIAGNOSTICS_FLUSH
#define IOTDATA_DIAGNOSTICS_FLUSH BLACKBOX_FLUSH_BATCH_TIME
#endif
#ifndef IOTDATA_DIAGNOSTICS_FLUSH_MS
#define IOTDATA_DIAGNOSTICS_FLUSH_MS 60000u /* only consulted by the BATCH_TIME policy */
#endif
/* Whether this node services DIAGNOSTICS_DUMP, which prints the store on the node's own console.
   ON by default -- every node has one, and a sensor on the bench over USB is exactly when you want
   it. The obligation that comes with it is iotdata_diagnostics_pump() every loop pass: a dump is
   drained a chunk at a time, so a node that advertises DUMP and never pumps accepts the command
   and then produces nothing. Turn it OFF on a node that cannot pump -- and it then disappears from
   the advertised list as well as the handler, so the node is not promising what it will not do. */
#ifndef IOTDATA_DIAGNOSTICS_DUMP
#define IOTDATA_DIAGNOSTICS_DUMP 1
#endif
#ifndef IOTDATA_DIAGNOSTICS_DUMP_CHUNK
#define IOTDATA_DIAGNOSTICS_DUMP_CHUNK 16 /* records per loop pass, so a dump cannot stall the loop */
#endif
#ifndef IOTDATA_DIAGNOSTICS_STAT_MAX
#define IOTDATA_DIAGNOSTICS_STAT_MAX 192
#endif

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define IOTDATA_DIAGNOSTICS_POOL_ATTR RTC_NOINIT_ATTR /* survives a reset, so a panic is still readable */
#else
#define IOTDATA_DIAGNOSTICS_POOL_ATTR
#endif

static IOTDATA_DIAGNOSTICS_POOL_ATTR char _iotdata_diagnostics_pool[IOTDATA_BLACKBOX_POOL_SZ];
static blackbox_handle_t _iotdata_diagnostics_handle;
static bool _iotdata_diagnostics_ready = false;
static char _iotdata_diagnostics_line[IOTDATA_DIAGNOSTICS_STAT_MAX];
static char _iotdata_diagnostics_rec[BLACKBOX_LINE_MAX];
static uint32_t _iotdata_diagnostics_ticked_ms = 0;
static iotdata_diagnostics_emit_fn _iotdata_diagnostics_emit = NULL;
static struct {
    bool active;
    size_t cursor;
    int count;
} _iotdata_diagnostics_dump;

static const blackbox_config_t _iotdata_diagnostics_config = {
    .pool = _iotdata_diagnostics_pool,
    .pool_sz = sizeof(_iotdata_diagnostics_pool),
    .flush = IOTDATA_DIAGNOSTICS_FLUSH,
    .flush_ms = IOTDATA_DIAGNOSTICS_FLUSH_MS,
    .persist_arg = IOTDATA_DIAGNOSTICS_PARTITION,
    .enabled = true, /* compiled in == collecting; the compile-time knob is the gate */
};

/* True once the store exists. Everything below asks first, because a failed init leaves a handle
   whose backend is NULL, and reaching through it faults rather than doing nothing. */
/* Where output goes from now on; returns what it was, so a console can put it back. NULL discards,
   which is what a node with nowhere to print wants. */
static inline iotdata_diagnostics_emit_fn iotdata_diagnostics_emit_set(const iotdata_diagnostics_emit_fn fn) {
    const iotdata_diagnostics_emit_fn was = _iotdata_diagnostics_emit;
    _iotdata_diagnostics_emit = fn;
    return was;
}

static inline void _iotdata_diagnostics_say(const char *const line) {
    if (_iotdata_diagnostics_emit != NULL)
        _iotdata_diagnostics_emit(line);
}

static inline bool iotdata_diagnostics_ready(void) {
    return _iotdata_diagnostics_ready;
}

/* For records of an application's own kind: blackbox_insert(iotdata_diagnostics_handle(), ...).
   NULL when the recorder did not start, which a caller must check as it would any handle. */
static inline blackbox_handle_t *iotdata_diagnostics_handle(void) {
    return _iotdata_diagnostics_ready ? &_iotdata_diagnostics_handle : NULL;
}

/* Start the recorder and log why we are running. `cold` distinguishes a boot from a wake, which is
   the difference between the two lifecycle events a sleeping node alternates between. Returns
   false if the store could not be opened -- worth logging, but never fatal: a node with no
   recorder still answers a diagnostics request, emptily. */
static inline bool iotdata_diagnostics_begin(const uint8_t reason, const bool cold) {
    iotdata_blackbox_begin(); /* seed the clock BEFORE init: see the note on the magic word */
    if (blackbox_init(&_iotdata_diagnostics_handle, &_iotdata_diagnostics_config) != 0)
        return false;
    _iotdata_diagnostics_ready = true;
    (void)iotdata_blackbox_lifecycle(&_iotdata_diagnostics_handle, cold ? IOTDATA_BB_LC_BOOT : IOTDATA_BB_LC_WAKE, reason);
    return true;
}

static inline void iotdata_diagnostics_event(const iotdata_bb_lc_event_t ev, const uint8_t reason) {
    if (_iotdata_diagnostics_ready)
        (void)iotdata_blackbox_lifecycle(&_iotdata_diagnostics_handle, ev, reason);
}

static inline void iotdata_diagnostics_flush(void) {
    if (_iotdata_diagnostics_ready)
        (void)blackbox_flush(&_iotdata_diagnostics_handle);
}

static inline void iotdata_diagnostics_enable(const bool on) {
    if (_iotdata_diagnostics_ready)
        blackbox_enable(&_iotdata_diagnostics_handle, on);
}

static inline void iotdata_diagnostics_clear(void) {
    if (_iotdata_diagnostics_ready) {
        _iotdata_diagnostics_dump.active = false; /* a dump in flight has a cursor about to go stale */
        blackbox_clear(&_iotdata_diagnostics_handle);
    }
}

/* One record per call, from `cursor` (start it at 0), 0 when there are no more. This is the shape
   both node layers want for their diagnostics report: node_diag_fn and idep_diag_fn. */
static inline size_t iotdata_diagnostics_pull(size_t *const cursor, char *const out, const size_t outsize) {
    if (!_iotdata_diagnostics_ready)
        return 0;
    const int n = blackbox_pull(&_iotdata_diagnostics_handle, cursor, out, outsize);
    return (n > 0) ? strlen(out) : 0;
}

/* The recorder's own housekeeping: its flush policy runs on this. Give it the same millisecond
   clock the rest of the loop uses; it works out its own interval. */
static inline void iotdata_diagnostics_tick(const uint32_t now_ms) {
    if (!_iotdata_diagnostics_ready)
        return;
    if (_iotdata_diagnostics_ticked_ms == 0)
        _iotdata_diagnostics_ticked_ms = now_ms;
    blackbox_tick(&_iotdata_diagnostics_handle, now_ms - _iotdata_diagnostics_ticked_ms);
    _iotdata_diagnostics_ticked_ms = now_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING IT BACK
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_diagnostics_stat(void) {
    blackbox_status_t st;
    if (!_iotdata_diagnostics_ready)
        _iotdata_diagnostics_say("diag: unavailable (recorder did not start)");
    else if (blackbox_status(&_iotdata_diagnostics_handle, &st)) {
        (void)blackbox_status_str(&st, BLACKBOX_STATUS_ALL, _iotdata_diagnostics_line, sizeof(_iotdata_diagnostics_line));
        _iotdata_diagnostics_say(_iotdata_diagnostics_line);
    }
}

/*
 * A dump is STARTED here and drained by iotdata_diagnostics_pump on later passes, a chunk at a
 * time: a full store is thousands of records and writing them in one call would stall the loop
 * long enough to lose frames. The flush first is what makes the dump cover the staged pool as well
 * as the store.
 */
static inline void iotdata_diagnostics_dump_start(void) {
    if (!_iotdata_diagnostics_ready) {
        _iotdata_diagnostics_say("diag: unavailable (recorder did not start)");
        return;
    }
    iotdata_diagnostics_flush();
    _iotdata_diagnostics_dump.cursor = 0;
    _iotdata_diagnostics_dump.count = 0;
    _iotdata_diagnostics_dump.active = true;
    _iotdata_diagnostics_say("diag: dump begin");
}

/* Call every loop pass. Emits at most IOTDATA_DIAGNOSTICS_DUMP_CHUNK records and returns whether a
   dump is still running. `emit` is asked for on every pass rather than remembered from the start,
   because by the time the records flow the console's turn to speak is usually over. */
static inline bool iotdata_diagnostics_pump(void) {
    if (!_iotdata_diagnostics_dump.active || !_iotdata_diagnostics_ready)
        return false;
    for (int i = 0; i < IOTDATA_DIAGNOSTICS_DUMP_CHUNK; i++) {
        if (blackbox_pull(&_iotdata_diagnostics_handle, &_iotdata_diagnostics_dump.cursor, _iotdata_diagnostics_rec, sizeof(_iotdata_diagnostics_rec)) <= 0) {
            (void)snprintf(_iotdata_diagnostics_line, sizeof(_iotdata_diagnostics_line), "diag: dump end (%d record(s))", _iotdata_diagnostics_dump.count);
            _iotdata_diagnostics_say(_iotdata_diagnostics_line);
            _iotdata_diagnostics_dump.active = false;
            return false;
        }
        _iotdata_diagnostics_say(_iotdata_diagnostics_rec);
        _iotdata_diagnostics_dump.count++;
    }
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE NODE'S CONTROL KEYS
//
// What a device with a recorder accepts over the air, and the one handler for it. ENABLE and CLEAR
// are always here; DUMP is too unless IOTDATA_DIAGNOSTICS_DUMP is off, in which case it is absent
// from BOTH the advertised list and the handler -- a node that advertises a key it will not service
// sends whoever asked on a wild goose chase.
// -----------------------------------------------------------------------------------------------------------------------------------------

static const uint8_t iotdata_diagnostics_control_keys[] = {
    IOTDATA_NODE_CONTROL_DIAGNOSTICS_ENABLE,
    IOTDATA_NODE_CONTROL_DIAGNOSTICS_CLEAR,
#if IOTDATA_DIAGNOSTICS_DUMP
    IOTDATA_NODE_CONTROL_DIAGNOSTICS_DUMP,
#endif
};
#define IOTDATA_DIAGNOSTICS_CONTROL_KEYS_COUNT ((uint8_t)(sizeof(iotdata_diagnostics_control_keys) / sizeof(iotdata_diagnostics_control_keys[0])))

/*
 * What to put in a node's config. These are the FUNCTIONS when the recorder is compiled in and
 * NULL when it is not, which is the distinction the node layer reads: a NULL diag hook means this
 * device keeps no diagnostics at all and its report is empty, while a present one that returns
 * nothing means it keeps a blackbox that happens to be empty. Passing the function unconditionally
 * would make every node claim a recorder. Both are usable with no #if at the call site.
 */
#define IOTDATA_DIAGNOSTICS_PULL               iotdata_diagnostics_pull
#define IOTDATA_DIAGNOSTICS_CONTROL            iotdata_diagnostics_node_control

/* Returns whether the key was one of ours. An app's own control hook offers every key it does not
   know to this, then falls through to its own. */
static inline bool iotdata_diagnostics_control(const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    if (!_iotdata_diagnostics_ready)
        return false; /* advertised, but not working today: counted unknown rather than pretended */
    switch (key) {
    case IOTDATA_NODE_CONTROL_DIAGNOSTICS_ENABLE: {
        const bool on = (vlen >= 1) ? (val[0] != 0u) : true;
        iotdata_diagnostics_enable(on);
        _iotdata_diagnostics_say(on ? "diag: enabled" : "diag: disabled");
        return true;
    }
    case IOTDATA_NODE_CONTROL_DIAGNOSTICS_CLEAR:
        iotdata_diagnostics_clear();
        _iotdata_diagnostics_say("diag: cleared");
        return true;
#if IOTDATA_DIAGNOSTICS_DUMP
    case IOTDATA_NODE_CONTROL_DIAGNOSTICS_DUMP:
        iotdata_diagnostics_dump_start();
        return true;
#endif
    default:
        return false;
    }
}

/* The same handler in the shape an end device's node layer wants (idep_control_fn). The station is
   narration only: there is ONE recorder per board, so a request to any station a board stands up
   reads and manages the same log. A relay or gateway, whose hook carries a clock instead, calls
   iotdata_diagnostics_control directly from its own default branch. */
static inline bool iotdata_diagnostics_node_control(const uint16_t station, const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    (void)station;
    return iotdata_diagnostics_control(key, val, vlen);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: A CONSOLE
//
// The recorder's words that have NO wire equivalent, and so cannot drift from one: its own status,
// a manual flush, and the record filter. Everything else an operator types -- enable, disable,
// clear, dump -- is in the CONTROL vocabulary and reaches the handler above by the same route an
// MQTT request does.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_diagnostics_filter(const int argc, char **const argv) {
    if (!_iotdata_diagnostics_ready) {
        _iotdata_diagnostics_say("diag: unavailable (recorder did not start)");
        return;
    }
    blackbox_handle_t *const h = &_iotdata_diagnostics_handle;
    const char *const sub = (argc >= 3) ? argv[2] : "";
    if (argc < 3)
        ; /* no subcommand: just show where the filter stands */
    else if (strcmp(sub, "off") == 0)
        blackbox_filter_mode(h, BLACKBOX_FILTER_OFF);
    else if (strcmp(sub, "include") == 0)
        blackbox_filter_mode(h, BLACKBOX_FILTER_INCLUDE);
    else if (strcmp(sub, "exclude") == 0)
        blackbox_filter_mode(h, BLACKBOX_FILTER_EXCLUDE);
    else if (strcmp(sub, "clear") == 0)
        blackbox_filter_clear(h);
    else if (strcmp(sub, "add") == 0 && argc >= 4)
        (void)blackbox_filter_add(h, argv[3]);
    else if (strcmp(sub, "remove") == 0 && argc >= 4)
        blackbox_filter_remove(h, argv[3]);
    else {
        _iotdata_diagnostics_say("diag filter: off|include|exclude|add <tag>|remove <tag>|clear");
        return;
    }
    iotdata_diagnostics_stat(); /* echo the resulting state, whichever way we got here */
}

/* Returns whether this was a recorder word. `argv[0]` is the verb the console dispatched on. */
static inline bool iotdata_diagnostics_console(const int argc, char **const argv) {
    if (argc < 2 || argv == NULL)
        return false;
    if (strcmp(argv[1], "stat") == 0)
        iotdata_diagnostics_stat();
    else if (strcmp(argv[1], "flush") == 0) {
        iotdata_diagnostics_flush();
        iotdata_diagnostics_stat();
    } else if (strcmp(argv[1], "filter") == 0)
        iotdata_diagnostics_filter(argc, argv);
    else
        return false;
    return true;
}

#define IOTDATA_DIAGNOSTICS_CONSOLE_HELP "stat | flush | filter [off|include|exclude|add <tag>|remove <tag>|clear]"

#else /* !IOTDATA_DIAGNOSTICS -- compiled out, but every call site still type-checks */

static const uint8_t *const iotdata_diagnostics_control_keys = NULL;
#define IOTDATA_DIAGNOSTICS_CONTROL_KEYS_COUNT ((uint8_t)0)
/* NULL, so a node with no recorder reports no diagnostics rather than an empty blackbox */
#define IOTDATA_DIAGNOSTICS_PULL               NULL
#define IOTDATA_DIAGNOSTICS_CONTROL            NULL
#define IOTDATA_DIAGNOSTICS_CONSOLE_HELP       ""

static inline iotdata_diagnostics_emit_fn iotdata_diagnostics_emit_set(const iotdata_diagnostics_emit_fn fn) {
    (void)fn;
    return NULL;
}
static inline bool iotdata_diagnostics_ready(void) {
    return false;
}
static inline blackbox_handle_t *iotdata_diagnostics_handle(void) {
    return NULL;
}
static inline bool iotdata_diagnostics_begin(const uint8_t reason, const bool cold) {
    (void)reason;
    (void)cold;
    return false;
}
static inline void iotdata_diagnostics_event(const iotdata_bb_lc_event_t ev, const uint8_t reason) {
    (void)ev;
    (void)reason;
}
static inline void iotdata_diagnostics_flush(void) {
}
static inline void iotdata_diagnostics_enable(const bool on) {
    (void)on;
}
static inline void iotdata_diagnostics_clear(void) {
}
static inline size_t iotdata_diagnostics_pull(size_t *const cursor, char *const out, const size_t outsize) {
    (void)cursor;
    (void)out;
    (void)outsize;
    return 0;
}
static inline void iotdata_diagnostics_tick(const uint32_t now_ms) {
    (void)now_ms;
}
static inline void iotdata_diagnostics_stat(void) {
}
static inline void iotdata_diagnostics_dump_start(void) {
}
static inline bool iotdata_diagnostics_pump(void) {
    return false;
}
static inline bool iotdata_diagnostics_control(const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    (void)key;
    (void)val;
    (void)vlen;
    return false;
}
static inline bool iotdata_diagnostics_console(const int argc, char **const argv) {
    (void)argc;
    (void)argv;
    return false;
}
static inline bool iotdata_diagnostics_node_control(const uint16_t station, const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    (void)station;
    (void)key;
    (void)val;
    (void)vlen;
    return false;
}

#endif /* IOTDATA_DIAGNOSTICS */

// -----------------------------------------------------------------------------------------------------------------------------------------
// ADAPTER IMPLEMENTATION
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_BLACKBOX_IMPLEMENTATION

static int iotdata_bb__enc_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_bb_lifecycle_t *r = (const iotdata_bb_lifecycle_t *)data;
    return snprintf(out, n, "%u,%u", (unsigned)r->event, (unsigned)r->reason);
}
static int iotdata_bb__dec_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_bb_lifecycle_t *r = (iotdata_bb_lifecycle_t *)data;
    unsigned e = 0, rs = 0;
    if (sscanf(in, "%u,%u", &e, &rs) != 2)
        return -1;
    r->event = (uint8_t)e;
    r->reason = (uint8_t)rs;
    return 0;
}
const blackbox_struct_config_t iotdata_blackbox_config_lifecycle = { "LC", iotdata_bb__enc_lifecycle, iotdata_bb__dec_lifecycle };

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
 * Call iotdata_blackbox_begin() once before blackbox_init(). */
#define IOTDATA_BLACKBOX_SEQ_MAGIC 0x1D5EC10Cu /* 'IDSEC-CLOC(k)' */

RTC_NOINIT_ATTR uint32_t iotdata_blackbox_seq;
RTC_NOINIT_ATTR static uint32_t iotdata_blackbox_seq_magic;

void iotdata_blackbox_begin(void) {
    if (iotdata_blackbox_seq_magic != IOTDATA_BLACKBOX_SEQ_MAGIC) { /* RTC was garbage: cold start */
        iotdata_blackbox_seq = 0;
        iotdata_blackbox_seq_magic = IOTDATA_BLACKBOX_SEQ_MAGIC;
    }
}

int iotdata_blackbox_clock(char *out, size_t n) {
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    return snprintf(out, n, "%u:%u", (unsigned)(++iotdata_blackbox_seq), (unsigned)up);
}
#else
void iotdata_blackbox_begin(void) { /* host: the clock is a real timestamp, nothing to seed */
}
#include <time.h>
int iotdata_blackbox_clock(char *out, size_t n) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC); /* C11 standard — no POSIX feature-test macro needed */
    const long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return snprintf(out, n, "%lld", ms);
}
#endif

#endif /* IOTDATA_BLACKBOX_IMPLEMENTATION */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_DIAGNOSTICS_H */
