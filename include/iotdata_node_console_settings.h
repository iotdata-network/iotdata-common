#ifndef IOTDATA_NODE_CONSOLE_SETTINGS_H
#define IOTDATA_NODE_CONSOLE_SETTINGS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_console_settings.h - the `node` console command: show and set the node's settings.
//
// Split out of iotdata_node_settings.h because it needs TWO headers that header does not: the
// console's emit signature, and nothing else. Leaving it inside meant settings.h had to be included
// after iotdata_node_console.h or the command silently did not exist -- an ordering rule nothing
// enforced and nothing reported, which is exactly how it was found (an aggregating header put
// settings first, and `node` disappeared).
//
// Still wrapped in #ifdef IOTDATA_NODE_CONSOLE_H: a node with no console compiles none of it, which
// was the original intent and stays true. The difference is that now the dependency is a file you
// either include or do not, rather than an order you have to know.
//
// THE SETTINGS THEMSELVES are a hard dependency and are included below, so this header works
// wherever it is put. iotdata_node_console.h deliberately is NOT included: whether a console exists
// is the application's decision, and the #ifdef is how that decision is read. A header that still
// demanded an order would have moved the trap rather than removed it.
// -----------------------------------------------------------------------------------------------------------------------------------------

#include "iotdata_node_settings.h"

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

static iotdata_node_settings_t *_iotdata_node_settings_console = NULL;
static iotdata_node_state_t *_iotdata_node_settings_console_state = NULL;

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_settings_console_attach(iotdata_node_settings_t *const s, iotdata_node_state_t *const state) {
    _iotdata_node_settings_console = s;
    _iotdata_node_settings_console_state = state;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* A TLV type by name, for the subject of a report. Generated from the type table rather than a list
   of its own, so a type added there is addressable here without anyone remembering to say so. */
static inline int _iotdata_node_settings_subject(const char *const want) {
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

static inline void _iotdata_node_settings_show_field(const iotdata_node_console_emit_fn emit, const char *const name, const unsigned value, const unsigned dflt, const char *const unit, const bool pinned) {
    if (pinned)
        emit("  %-14s %u%s PINNED on the command line%s\n", name, value, unit, (value == dflt) ? "" : " (default differs)");
    else if (value == dflt)
        emit("  %-14s %u%s\n", name, value, unit);
    else
        emit("  %-14s %u%s (default %u%s)\n", name, value, unit, dflt, unit);
}

static inline void _iotdata_node_settings_show_report(const iotdata_node_console_emit_fn emit, const iotdata_node_settings_t *const s, const uint8_t subject) {
    const iotdata_node_settings_report_t *const r = iotdata_node_settings_report_find(s, subject);
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
    emit(" on-change=%-3s on-event=%s%s\n", (r->flags & IOTDATA_NODE_REPORT_ON_CHANGE) ? "on" : "off", (r->flags & IOTDATA_NODE_REPORT_ON_EVENT) ? "on" : "off", iotdata_node_settings_report_is_pinned(subject) ? "  PINNED" : "");
}

/* UNUSED IS NOT AN ERROR HERE: including the adapter and registering the command are separate
   decisions. A node with a console that does not offer this one still compiles it, and that is
   cheaper to allow than to make every application prove it wired the command up. */
__attribute__((unused)) static void iotdata_node_settings_console(const iotdata_node_console_emit_fn emit, const int argc, char **const argv) {
    iotdata_node_settings_t *const s = _iotdata_node_settings_console;
    if (s == NULL) {
        emit("node: this node keeps no protocol settings\n");
        return;
    }
    const bool persists = iotdata_node_settings_persists(_iotdata_node_settings_console_state);

    if (argc < 2) {
        emit("node: protocol settings%s\n", persists ? "" : " (NOT PERSISTED: nowhere to write them)");
        _iotdata_node_settings_show_field(emit, "station", (unsigned)iotdata_node_settings_station(s), (unsigned)_iotdata_node_settings_default.station, "", iotdata_node_settings_is_pinned(IOTDATA_NODE_SETTINGS_PIN_STATION));
        _iotdata_node_settings_show_field(emit, "window", (unsigned)iotdata_node_settings_window_ms(s), (unsigned)_iotdata_node_settings_default.window_ms, "ms", iotdata_node_settings_is_pinned(IOTDATA_NODE_SETTINGS_PIN_WINDOW));
        _iotdata_node_settings_show_field(emit, "interval", (unsigned)(iotdata_node_settings_interval_ms(s) / 60000u), (unsigned)_iotdata_node_settings_default.interval_m, "m",
                                          iotdata_node_settings_is_pinned(IOTDATA_NODE_SETTINGS_PIN_INTERVAL));
        emit("  reports:\n");
        for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++)
            if (iotdata_node_tlv_is_reportable(t))
                _iotdata_node_settings_show_report(emit, s, t);
        return;
    }

    uint32_t v = 0;
    bool wrote = false;
    if (strcmp(argv[1], "station") == 0 || strcmp(argv[1], "window") == 0 || strcmp(argv[1], "interval") == 0) {
        uint16_t *const field = (strcmp(argv[1], "station") == 0) ? &s->station : (strcmp(argv[1], "window") == 0) ? &s->window_ms : &s->interval_m;
        const uint8_t pin = (uint8_t)((strcmp(argv[1], "station") == 0) ? IOTDATA_NODE_SETTINGS_PIN_STATION : (strcmp(argv[1], "window") == 0) ? IOTDATA_NODE_SETTINGS_PIN_WINDOW : IOTDATA_NODE_SETTINGS_PIN_INTERVAL);
        if (argc < 3) {
            const uint16_t d = (strcmp(argv[1], "station") == 0) ? _iotdata_node_settings_default.station : (strcmp(argv[1], "window") == 0) ? _iotdata_node_settings_default.window_ms : _iotdata_node_settings_default.interval_m;
            _iotdata_node_settings_show_field(emit, argv[1], (unsigned)*field, (unsigned)d, "", iotdata_node_settings_is_pinned(pin));
            return;
        }
        /* the console is refused too, not only the radio: the argument was somebody's decision about
           this box, and "unless you are sitting at it" is a weaker claim than the one they made */
        if (iotdata_node_settings_is_pinned(pin)) {
            emit("%s: PINNED on the command line -- restart without that argument to change it\n", argv[1]);
            return;
        }
        if (!_iotdata_node_settings_a2u(argv[2], &v) || (v > 0xFFFFu)) {
            emit("%s: '%s' is not a number\n", argv[1], argv[2]);
            return;
        }
        /* A station is checked against the protocol's own rule, not just the field's width: 0 is not
           a station and the broadcast id would make this node answer every broadcast as its own. */
        if (strcmp(argv[1], "station") == 0 && !iotdata_station_is_assignable((uint16_t)v)) {
            emit("station: %u is not assignable (1..%u)\n", (unsigned)v, (unsigned)IOTDATA_STATION_ASSIGNABLE_MAX);
            return;
        }
        /* Same reasoning one field over: the cap is what stops a slip parking this node out of
           reach, and the console can make that slip as easily as the radio. */
        if (strcmp(argv[1], "interval") == 0 && !iotdata_node_settings_interval_is_valid((uint16_t)v)) {
            emit("interval: %u minutes is past the cap (0..%u, one week)\n", (unsigned)v, (unsigned)IOTDATA_NODE_SETTINGS_INTERVAL_M_MAX);
            return;
        }
        *field = (uint16_t)v;
        wrote = true;
    } else if (strcmp(argv[1], "report") == 0) {
        if (argc < 3) {
            for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++)
                if (iotdata_node_tlv_is_reportable(t))
                    _iotdata_node_settings_show_report(emit, s, t);
            return;
        }
        const int subject = _iotdata_node_settings_subject(argv[2]);
        if (subject < 0) {
            emit("report: '%s' is not a reportable type\n", argv[2]);
            return;
        }
        if (argc < 5) {
            _iotdata_node_settings_show_report(emit, s, (uint8_t)subject);
            return;
        }
        if (iotdata_node_settings_report_is_pinned((uint8_t)subject)) {
            emit("report %s: PINNED on the command line -- restart without that argument to change it\n", argv[2]);
            return;
        }
        /* Stated whole: a report record that exists says everything about that subject, so a change
           to one part starts from what is already stated rather than from nothing. */
        const iotdata_node_settings_report_t *const cur = iotdata_node_settings_report_find(s, (uint8_t)subject);
        iotdata_node_settings_report_t r = (cur != NULL) ? *cur : (iotdata_node_settings_report_t){ .subject = (uint8_t)subject, .flags = 0, .period_s = 0, .change_s = 0, .events = 0 };
        r.subject = (uint8_t)subject;
        bool on = false;
        if (strcmp(argv[3], "at-startup") == 0 && _iotdata_node_settings_a2b(argv[4], &on))
            r.flags = (uint16_t)(on ? (r.flags | IOTDATA_NODE_REPORT_AT_STARTUP) : (r.flags & (uint16_t)~IOTDATA_NODE_REPORT_AT_STARTUP));
        else if (strcmp(argv[3], "period") == 0 && (strcmp(argv[4], "off") == 0 || (_iotdata_node_settings_a2u(argv[4], &v) && v <= 0xFFFFu))) {
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
        if (!iotdata_node_settings_report_set(s, &r)) {
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
        const bool ok = persists && iotdata_node_settings_commit(s, _iotdata_node_settings_console_state);
        /* the answer is what is now true, and ONLY about what was asked: re-entering with the same
           subject and no value is the read that pairs with the write just made */
        char *nargv[3] = { argv[0], argv[1], (argc > 2) ? argv[2] : NULL };
        iotdata_node_settings_console(emit, (strcmp(argv[1], "report") == 0) ? 3 : 2, nargv);
        if (!ok)
            emit("  (not persisted: this node has nowhere to write it)\n");
    }
}

/*
 * The settings as command-line options, for a --help -- the twin of iotdata_node_config_help().
 *
 * A host that reads "node-" lines out of its configuration file accepts them as arguments too, and
 * an argument is the ONLY way to pin one. A feature nobody can find is not a feature, so the list
 * that already knows every key generates this rather than a hand-kept paragraph going stale.
 */
static inline void iotdata_node_settings_help(const iotdata_node_console_emit_fn emit, const iotdata_node_settings_t *const s) {
    char key[IOTDATA_NODE_SETTINGS_KEY_MAX], val[32];
    emit("\n  the protocol's own settings -- stating one here PINS it for the run:\n");
    for (int k = 0; k < IOTDATA_NODE_SETTINGS_KEY_COUNT; k++)
        if (iotdata_node_settings_key_name(k, key, sizeof(key))) {
            char opt[IOTDATA_NODE_SETTINGS_KEY_MAX + 4];
            opt[0] = opt[1] = '-';
            size_t at = 2;
            for (const char *p = key; *p != '\0' && at + 1u < sizeof(opt); p++)
                opt[at++] = *p;
            opt[at] = '\0';
            if (s != NULL && iotdata_node_settings_key_read(s, k, val, sizeof(val)))
                emit("  %-34s default %s\n", opt, val);
            else
                emit("  %-34s\n", opt);
        }
}

/* Drop this into the application's iotdata_node_console_t array, beside IOTDATA_NODE_CONFIG_CONSOLE_COMMAND. */
#define IOTDATA_NODE_SETTINGS_CONSOLE_COMMAND { "node", iotdata_node_settings_console, "show or set protocol settings: node [station|window|interval|report] ..." }

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_H */

#endif /* IOTDATA_NODE_CONSOLE_SETTINGS_H */
