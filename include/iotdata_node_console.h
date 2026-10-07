
#ifndef IOTDATA_NODE_CONSOLE_H
#define IOTDATA_NODE_CONSOLE_H

/*
 * iotdata_node_console.h — a small, generic USB-serial-JTAG console line for esp32 apps.
 *
 * Non-blocking and verb-based: a set of built-in consoles (help, vers, stat, logl, boot) plus any
 * consoles the app plugs in on top. Designed to be driven from a cooperative main loop — call
 * iotdata_node_console_poll() each pass; it drains whatever the console has and dispatches a full line.
 * Output is plain printf (over the same USB-serial-JTAG console the app logs on), so records/status
 * come straight back over USB.
 *
 * Single-header: in ONE translation unit (the app's unity TU) define IOTDATA_NODE_CONSOLE_IMPLEMENTATION
 * before including; other TUs just include it for the declarations.
 *
 *   static void con_foo(iotdata_node_console_emit_fn emit, int argc, char **argv) { (void)argc; (void)argv; emit("foo!\n"); }
 *   static const iotdata_node_console_t app_cons[] = { { "foo", con_foo, "do the foo thing" } };
 *   iotdata_node_console_init(app_cons, sizeof app_cons / sizeof app_cons[0]);   // after boot
 *   ...
 *   for (;;) { ...; iotdata_node_console_poll(); }                                // non-blocking, each pass
 *
 * Pass (NULL, 0) for no app consoles. The cons array must live for the program's lifetime (static).
 * On non-esp32 (host) builds this compiles to no-ops, so shared code can include it unconditionally.
 *
 * esp32 component requirements (the TU that defines the implementation must be in a component that
 * REQUIRES these): esp_driver_usb_serial_jtag, vfs, esp_app_format. (esp_timer / esp_hw_support /
 * heap / log / esp_system come in as common dependencies.)
 */

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stddef.h>

typedef void (*iotdata_node_console_emit_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* argv[0] is the console name. */
typedef void (*iotdata_node_console_fn)(iotdata_node_console_emit_fn emit, int argc, char **argv);

typedef struct {
    const char *name;           /* the verb typed at the console            */
    iotdata_node_console_fn fn; /* handler                              */
    const char *help;           /* one-line description for `help` (or NULL) */
} iotdata_node_console_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#if !defined(ESP_PLATFORM)
#include <poll.h>
#include <unistd.h>
#endif

/* Command output is emitted as a pseudo log line "<TAG> (<ms>) <message>" — the same shape as an
 * ESP_LOG "<LEVEL> (<ms>) tag: msg" line — so a host monitor can grep/strip it by the leading level
 * letter (default 'C'), consistent with the I/W/E/D log lines it's interleaved with. Override the
 * letter before including if it clashes. */
#ifndef IOTDATA_NODE_CONSOLE_TAG
#define IOTDATA_NODE_CONSOLE_TAG "C"
#endif

#if defined(ESP_PLATFORM)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

void iotdata_node_console_reply(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(ESP_PLATFORM)
    printf(IOTDATA_NODE_CONSOLE_TAG " (%u) ", (unsigned)esp_log_timestamp()); /* ms since boot, like ESP_LOG */
#else
    fputs(IOTDATA_NODE_CONSOLE_TAG " (0) ", stdout);
#endif
    (void)vprintf(fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONSOLE_LINE_MAX
#define IOTDATA_NODE_CONSOLE_LINE_MAX 128
#endif
#ifndef IOTDATA_NODE_CONSOLE_ARGC_MAX
#define IOTDATA_NODE_CONSOLE_ARGC_MAX 8
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#if !defined(ESP_PLATFORM)
static bool iotdata_node_con_open = false; /* there is a stdin worth looking at, and it has not ended */
#endif
static const iotdata_node_console_t *iotdata_node_con_app = NULL;
static size_t iotdata_node_con_app_n = 0;
static char iotdata_node_con_line[IOTDATA_NODE_CONSOLE_LINE_MAX];
static size_t iotdata_node_con_len = 0;

// ------------------------------------------------------------------------------------------------------------------------

#if defined(ESP_PLATFORM)

static esp_log_level_t iotdata_node_con_log_level = (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;

static const char *iotdata_node_con_log_name(esp_log_level_t l) {
    switch (l) {
    case ESP_LOG_NONE:
        return "none";
    case ESP_LOG_ERROR:
        return "error";
    case ESP_LOG_WARN:
        return "warn";
    case ESP_LOG_INFO:
        return "info";
    case ESP_LOG_DEBUG:
        return "debug";
    case ESP_LOG_VERBOSE:
        return "verbose";
    default:
        return "?";
    }
}
static bool iotdata_node_con_log_parse(const char *s, esp_log_level_t *out) {
    if (!strcmp(s, "none") || !strcmp(s, "0"))
        *out = ESP_LOG_NONE;
    else if (!strcmp(s, "error") || !strcmp(s, "1"))
        *out = ESP_LOG_ERROR;
    else if (!strcmp(s, "warn") || !strcmp(s, "warning") || !strcmp(s, "2"))
        *out = ESP_LOG_WARN;
    else if (!strcmp(s, "info") || !strcmp(s, "3"))
        *out = ESP_LOG_INFO;
    else if (!strcmp(s, "debug") || !strcmp(s, "4"))
        *out = ESP_LOG_DEBUG;
    else if (!strcmp(s, "verbose") || !strcmp(s, "5"))
        *out = ESP_LOG_VERBOSE;
    else
        return false;
    return true;
}

static const char *iotdata_node_con_reset_name(int r) {
    switch (r) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_SW:
        return "sw";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    default:
        return "other";
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_vers(const iotdata_node_console_emit_fn emit, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    const esp_app_desc_t *const d = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    emit("app=%s vers=%s built=%s %s\n", d ? d->project_name : "?", d ? d->version : "?", d ? d->date : "?", d ? d->time : "?");
    emit("platform=%s chip-rev=%d cores=%d idf=%s\n", CONFIG_IDF_TARGET, (int)chip.revision, (int)chip.cores, esp_get_idf_version());
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_stat(const iotdata_node_console_emit_fn emit, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    emit("uptime=%us reset=%s\n", (unsigned)(esp_timer_get_time() / 1000000), iotdata_node_con_reset_name((int)esp_reset_reason()));
    emit("heap: free=%u min=%u largest=%u\n", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_logl(const iotdata_node_console_emit_fn emit, int argc, char **argv) {
    if (argc < 2) {
        emit("log level = %s\n", iotdata_node_con_log_name(iotdata_node_con_log_level));
        return;
    }
    esp_log_level_t lvl;
    if (!iotdata_node_con_log_parse(argv[1], &lvl)) {
        emit("logl: unknown level '%s' (none|error|warn|info|debug|verbose | 0..5)\n", argv[1]);
        return;
    }
    esp_log_level_set("*", lvl); /* dynamic ceiling is CONFIG_LOG_MAXIMUM_LEVEL — can't raise past it */
    iotdata_node_con_log_level = lvl;
    emit("log level = %s\n", iotdata_node_con_log_name(lvl));
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_boot(const iotdata_node_console_emit_fn emit, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    emit("rebooting\n");
    fflush(stdout);
    esp_rom_delay_us(50000); /* let the reply drain out of the USB TX buffer before the reset */
    esp_restart();
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* ESP_PLATFORM */

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static const iotdata_node_console_t iotdata_node_con_builtin[] = {
#if defined(ESP_PLATFORM)
    { "vers", iotdata_node_con_cmd_vers, "device firmware / platform / build version" },
    { "stat", iotdata_node_con_cmd_stat, "device uptime, reset reason, heap" },
    { "logl", iotdata_node_con_cmd_logl, "device loglevel — 'logl' shows, 'logl <lvl>' sets" },
    { "boot", iotdata_node_con_cmd_boot, "device restart" },
#endif
    /* A terminator, so the array is never empty -- every built-in above is ESP-only, and a host
       build would otherwise declare `= { }`, which is not C before C23. */
    { NULL, NULL, NULL },
};
/* Walked to the NULL rather than by a count: on a host every built-in above is compiled out, and
   `i < 0` on a size_t is a warning as well as a pointless loop. */

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_help(const iotdata_node_console_emit_fn emit) {
    emit("commands:\n");
    emit("  %-8s %s\n", "help", "list commands");
    for (size_t i = 0; iotdata_node_con_builtin[i].name != NULL; i++)
        emit("  %-8s %s\n", iotdata_node_con_builtin[i].name, iotdata_node_con_builtin[i].help ? iotdata_node_con_builtin[i].help : "");
    for (size_t i = 0; i < iotdata_node_con_app_n; i++)
        emit("  %-8s %s\n", iotdata_node_con_app[i].name, iotdata_node_con_app[i].help ? iotdata_node_con_app[i].help : "");
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_console_dispatch(char *line) {
    char *argv[IOTDATA_NODE_CONSOLE_ARGC_MAX];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok != NULL && argc < IOTDATA_NODE_CONSOLE_ARGC_MAX; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return; /* blank line */
    if (strcmp(argv[0], "help") == 0) {
        iotdata_node_con_cmd_help(iotdata_node_console_reply);
        return;
    }
    for (size_t i = 0; iotdata_node_con_builtin[i].name != NULL; i++) /* built-ins win a name clash */
        if (strcmp(argv[0], iotdata_node_con_builtin[i].name) == 0) {
            iotdata_node_con_builtin[i].fn(iotdata_node_console_reply, argc, argv);
            return;
        }
    for (size_t i = 0; i < iotdata_node_con_app_n; i++)
        if (strcmp(argv[0], iotdata_node_con_app[i].name) == 0) {
            iotdata_node_con_app[i].fn(iotdata_node_console_reply, argc, argv);
            return;
        }
    iotdata_node_console_reply("unknown command '%s' (try 'help')\n", argv[0]);
}

// ------------------------------------------------------------------------------------------------------------------------

void iotdata_node_console_init(const iotdata_node_console_t *cons, size_t count) {
    iotdata_node_con_app = cons;
    iotdata_node_con_app_n = count;
    iotdata_node_con_len = 0;
#if defined(ESP_PLATFORM)
    if (usb_serial_jtag_driver_install(&(usb_serial_jtag_driver_config_t){ .tx_buffer_size = 256, .rx_buffer_size = 256 }) == ESP_OK)
        usb_serial_jtag_vfs_use_driver(); /* route stdio through the driver so reads + printf agree */
#else
    /* Nothing to set up: the poll in getc() asks before it reads, so stdin is left exactly as the
       shell handed it over. A process with no terminal simply never has a byte ready. */
    iotdata_node_con_open = true;
#endif
    iotdata_node_console_reply("iotdata-console: ready (try 'help')\n");
}

// ------------------------------------------------------------------------------------------------------------------------

/* One byte if there is one, -1 if there is not. NEVER BLOCKS on either platform, because this is
   called from the middle of a main loop that has a radio to service. */
static int iotdata_node_con_getc(void) {
#if defined(ESP_PLATFORM)
    uint8_t c;
    return (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) ? (int)c : -1; /* timeout 0 -> drain the FIFO */
#else
    if (!iotdata_node_con_open)
        return -1;
    /* ASKED FIRST, and only read once the answer is yes.
     *
     * The obvious alternative is O_NONBLOCK on stdin, and it is a trap on the host this runs on:
     * stdin and stdout of a process started from a terminal usually share one open file
     * description, so setting it there sets it for OUTPUT too -- and a gateway that logs every
     * frame would then start losing writes to EAGAIN the moment it got busy. poll() asks about the
     * one descriptor we mean, changes nothing, and cannot block with a zero timeout. */
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
        return -1;
    unsigned char c;
    const ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1)
        return (int)c;
    if (n == 0)
        iotdata_node_con_open = false; /* EOF: stop asking, or a closed stdin spins the loop */
    return -1;
#endif
}

void iotdata_node_console_poll(void) {
    int ch;
    while ((ch = iotdata_node_con_getc()) >= 0) {
        const char c = (char)ch;
        if (c == '\r') {
            //
        } else if (c == '\n') {
            iotdata_node_con_line[iotdata_node_con_len] = '\0';
            iotdata_node_console_dispatch(iotdata_node_con_line);
            iotdata_node_con_len = 0;
        } else if (iotdata_node_con_len < sizeof(iotdata_node_con_line) - 1) {
            iotdata_node_con_line[iotdata_node_con_len++] = c;
        } else {
            iotdata_node_con_len = 0; /* overlong line -> drop it */
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_H */
