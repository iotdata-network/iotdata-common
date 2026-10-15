#ifndef D_CONSOLE_H
#define D_CONSOLE_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_console.h -- what a console LINE is, per platform, and nothing about what is typed on it.
//
// The verbs, the line editor, the dispatch and the sink all belong to iotdata_node_console.h, which
// knows nothing about USB, stdin or restarts. What a platform owes it is here:
//
//   console_line_begin(rx, tx)       take the line; false when this build has none to take
//   console_line_read(buf, max)      whatever is waiting, NEVER blocking, 0 when nothing is
//   console_stamp_ms()               the number a reply line carries, in the platform's own terms
//   console_heap(&free, &min, &big)  true where a heap figure means something, false where it does not
//   console_platform_str(buf, size)  the one line of detail the version strings do not carry
//   console_restart()                never returns
//
// The two sizes are passed IN rather than defined here, because the receive ring is a contract with
// the uploader above (iotdata_node_console_content.h asserts a data line fits in half of it), and a
// contract belongs with the party that depends on it.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "d_platform.h"

// ------------------------------------------------------------------------------------------------------------------------

static inline const char *console_log_level_name(const platform_log_level_t l) {
    switch (l) {
    case PLATFORM_LOG_NONE:
        return "none";
    case PLATFORM_LOG_ERROR:
        return "error";
    case PLATFORM_LOG_WARN:
        return "warn";
    case PLATFORM_LOG_INFO:
        return "info";
    case PLATFORM_LOG_DEBUG:
        return "debug";
    case PLATFORM_LOG_VERBOSE:
        return "verbose";
    default:
        return "?";
    }
}

static inline bool console_log_level_parse(const char *const s, platform_log_level_t *const out) {
    if (s == NULL || out == NULL)
        return false;
    if (!strcmp(s, "none") || !strcmp(s, "0"))
        *out = PLATFORM_LOG_NONE;
    else if (!strcmp(s, "error") || !strcmp(s, "1"))
        *out = PLATFORM_LOG_ERROR;
    else if (!strcmp(s, "warn") || !strcmp(s, "warning") || !strcmp(s, "2"))
        *out = PLATFORM_LOG_WARN;
    else if (!strcmp(s, "info") || !strcmp(s, "3"))
        *out = PLATFORM_LOG_INFO;
    else if (!strcmp(s, "debug") || !strcmp(s, "4"))
        *out = PLATFORM_LOG_DEBUG;
    else if (!strcmp(s, "verbose") || !strcmp(s, "5"))
        *out = PLATFORM_LOG_VERBOSE;
    else
        return false;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_CONSOLE_H */
