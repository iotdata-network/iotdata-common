#ifndef IOTDATA_NODE_CONSOLE_DIAGNOSTICS_H
#define IOTDATA_NODE_CONSOLE_DIAGNOSTICS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_console_diagnostics.h - the recorder's console words: `diag stat | flush | filter`.
//
// Split out of iotdata_node_diagnostics.h to sit beside its config and settings twins. Unlike those
// two it needs nothing from iotdata_node_console.h -- it takes a plain (argc, argv) and answers
// through the recorder's own _say -- so it has no ordering trap to fix. It is here for symmetry: all
// three console adapters in one place means the next person looks in one place.
//
// THE WORDS HERE HAVE NO WIRE EQUIVALENT, which is the whole basis for the split between this file
// and the CONTROL handler that stays behind. Status, a manual flush and the record filter exist only
// at a console; enable, disable, clear and dump are CONTROL vocabulary and reach the handler by the
// same route an MQTT request does, so they cannot drift from it.
//
// BOTH BRANCHES LIVE HERE, enabled and compiled-out, because every call site must still type-check
// when the recorder is off -- the same contract diagnostics.h keeps for the rest of its surface.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE RECORDER ITSELF is a hard dependency and is included here, so this header works wherever it
   is put. A header that still demanded a particular include order would have moved the trap this
   split exists to remove, rather than removing it. */
#include "iotdata_node_diagnostics.h"

#if IOTDATA_NODE_DIAGNOSTICS

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE CONSOLE WORDS
//
// The recorder's words that have NO wire equivalent, and so cannot drift from one: its own status,
// a manual flush, and the record filter. Everything else an operator types -- enable, disable,
// clear, dump -- is in the CONTROL vocabulary and reaches the handler above by the same route an
// MQTT request does.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_diagnostics_filter(const int argc, char **const argv) {
    if (!_iotdata_node_diagnostics_ready) {
        _iotdata_node_diagnostics_say("diag: unavailable (recorder did not start)");
        return;
    }
    blackbox_handle_t *const h = &_iotdata_node_diagnostics_handle;
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
        _iotdata_node_diagnostics_say("diag filter: off|include|exclude|add <tag>|remove <tag>|clear");
        return;
    }
    iotdata_node_diagnostics_stat(); /* echo the resulting state, whichever way we got here */
}

/* Returns whether this was a recorder word. `argv[0]` is the verb the console dispatched on. */
static inline bool iotdata_node_diagnostics_console(const int argc, char **const argv) {
    if (argc < 2 || argv == NULL)
        return false;
    if (strcmp(argv[1], "stat") == 0)
        iotdata_node_diagnostics_stat();
    else if (strcmp(argv[1], "flush") == 0) {
        iotdata_node_diagnostics_flush();
        iotdata_node_diagnostics_stat();
    } else if (strcmp(argv[1], "filter") == 0)
        iotdata_node_diagnostics_filter(argc, argv);
    else
        return false;
    return true;
}

#define IOTDATA_NODE_DIAGNOSTICS_CONSOLE_HELP "stat | flush | filter [off|include|exclude|add <tag>|remove <tag>|clear]"

#else /* !IOTDATA_NODE_DIAGNOSTICS -- compiled out, but every call site still type-checks */

#define IOTDATA_NODE_DIAGNOSTICS_CONSOLE_HELP ""

static inline bool iotdata_node_diagnostics_console(const int argc, char **const argv) {
    (void)argc;
    (void)argv;
    return false;
}

#endif /* IOTDATA_NODE_DIAGNOSTICS */

#endif /* IOTDATA_NODE_CONSOLE_DIAGNOSTICS_H */
