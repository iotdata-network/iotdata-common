#ifndef D_CONSOLE_LINUX_H
#define D_CONSOLE_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_console_linux.h -- the console line on a host: stdin, exactly as the shell handed it over.
//
// See d_console.h for what this answers. A process started by a service manager has no terminal and
// simply never has a byte ready, which is the same answer as "nobody is typing" and needs no
// special case.
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// ------------------------------------------------------------------------------------------------------------------------

#include "d_console.h"

#define D_CONSOLE_PROVIDED 1

// ------------------------------------------------------------------------------------------------------------------------

static bool _console_open = false; /* there is a stdin worth looking at, and it has not ended */

// ------------------------------------------------------------------------------------------------------------------------

static inline bool console_line_begin(__attribute__((unused)) const unsigned rx_bytes, __attribute__((unused)) const unsigned tx_bytes) {
    _console_open = true;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline size_t console_line_read(uint8_t *const buf, const size_t max) {
    if (!_console_open)
        return 0u;
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
        return 0u;
    const ssize_t n = read(STDIN_FILENO, buf, max);
    if (n > 0)
        return (size_t)n;
    if (n == 0)
        _console_open = false; /* EOF: stop asking, or a closed stdin spins the loop */
    return 0u;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline uint32_t console_stamp_ms(void) {
    return hw_time_ms();
}

// ------------------------------------------------------------------------------------------------------------------------

/* NO HEAP FIGURE, AND SAYING SO IS THE HONEST ANSWER. malloc on a host does not have "free" and
   "largest free block" in any sense comparable to a device's allocator -- the arena grows from the
   OS on demand -- so a number here would invite a comparison that means nothing. */
static inline bool console_heap(__attribute__((unused)) uint32_t *const free_bytes, __attribute__((unused)) uint32_t *const min_bytes, __attribute__((unused)) uint32_t *const largest_bytes) {
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const char *console_platform_str(char *const buf, const size_t size) {
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    (void)snprintf(buf, size, "host pid=%ld cores=%ld", (long)getpid(), cores > 0 ? cores : 1L);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void console_restart(void) {
    fflush(stdout);
    fflush(stderr);
    exit(0);
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_CONSOLE_LINUX_H */
