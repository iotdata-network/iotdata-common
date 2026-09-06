/*
 * iotdata_command.h — a small, generic USB-serial-JTAG command line for esp32 apps.
 *
 * Non-blocking and verb-based: a set of built-in commands (help, vers, stat, logl, boot) plus any
 * commands the app plugs in on top. Designed to be driven from a cooperative main loop — call
 * iotdata_command_poll() each pass; it drains whatever the console has and dispatches a full line.
 * Output is plain printf (over the same USB-serial-JTAG console the app logs on), so records/status
 * come straight back over USB.
 *
 * Single-header: in ONE translation unit (the app's unity TU) define IOTDATA_COMMAND_IMPLEMENTATION
 * before including; other TUs just include it for the declarations.
 *
 *   static void cmd_foo(int argc, char **argv) { (void)argc; (void)argv; iotdata_command_reply("foo!\n"); }
 *   static const iotdata_command_t app_cmds[] = { { "foo", cmd_foo, "do the foo thing" } };
 *   iotdata_command_init(app_cmds, sizeof app_cmds / sizeof app_cmds[0]);   // after boot
 *   ...
 *   for (;;) { ...; iotdata_command_poll(); }                                // non-blocking, each pass
 *
 * Pass (NULL, 0) for no app commands. The cmds array must live for the program's lifetime (static).
 * On non-esp32 (host) builds this compiles to no-ops, so shared code can include it unconditionally.
 *
 * esp32 component requirements (the TU that defines the implementation must be in a component that
 * REQUIRES these): esp_driver_usb_serial_jtag, vfs, esp_app_format. (esp_timer / esp_hw_support /
 * heap / log / esp_system come in as common dependencies.)
 */
#ifndef IOTDATA_COMMAND_H
#define IOTDATA_COMMAND_H

#include <stddef.h>

typedef void (*iotdata_command_fn)(int argc, char **argv); /* argv[0] is the command name */

typedef struct {
    const char *name;      /* the verb typed at the console            */
    iotdata_command_fn fn; /* handler                              */
    const char *help;      /* one-line description for `help` (or NULL) */
} iotdata_command_t;

/* Bring up the console reader and register the app's extra commands (layered on top of the built-ins,
 * which always win a name clash). Call once, after the console is up. */
void iotdata_command_init(const iotdata_command_t *cmds, size_t count);

/* Poll the console once — NON-BLOCKING. Call every main-loop pass. Reads any pending bytes and, when a
 * full line has arrived, tokenises it (whitespace) and dispatches to the matching command. */
void iotdata_command_poll(void);

/* Emit one line of command output as a pseudo log line "C (<ms>) <message>" (see IOTDATA_COMMAND_TAG)
 * so a host-side monitor can tell command I/O apart from interleaved ESP_LOG lines — match/strip/
 * capture on the leading 'C', just like it does the I/W/E/D levels. ALL command handlers — built-in
 * and app — should print through this, one line per call (include the trailing '\n' in fmt). Plain
 * printf still works but its output won't carry the tag. */
void iotdata_command_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ============================ implementation ============================ */
#ifdef IOTDATA_COMMAND_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/* Command output is emitted as a pseudo log line "<TAG> (<ms>) <message>" — the same shape as an
 * ESP_LOG "<LEVEL> (<ms>) tag: msg" line — so a host monitor can grep/strip it by the leading level
 * letter (default 'C'), consistent with the I/W/E/D log lines it's interleaved with. Override the
 * letter before including if it clashes. */
#ifndef IOTDATA_COMMAND_TAG
#define IOTDATA_COMMAND_TAG "C"
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

void iotdata_command_reply(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(ESP_PLATFORM)
    printf(IOTDATA_COMMAND_TAG " (%u) ", (unsigned)esp_log_timestamp()); /* ms since boot, like ESP_LOG */
#else
    fputs(IOTDATA_COMMAND_TAG " (0) ", stdout);
#endif
    (void)vprintf(fmt, ap);
    va_end(ap);
}

#ifndef IOTDATA_COMMAND_LINE_MAX
#define IOTDATA_COMMAND_LINE_MAX 128
#endif
#ifndef IOTDATA_COMMAND_ARGC_MAX
#define IOTDATA_COMMAND_ARGC_MAX 8
#endif

static const iotdata_command_t *iotdata__cmd_app = NULL;
static size_t iotdata__cmd_app_n = 0;
static char iotdata__cmd_line[IOTDATA_COMMAND_LINE_MAX];
static size_t iotdata__cmd_len = 0;

#if defined(ESP_PLATFORM)

static esp_log_level_t iotdata__log_level = (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;

static const char *iotdata__log_name(esp_log_level_t l) {
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
static bool iotdata__log_parse(const char *s, esp_log_level_t *out) {
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
static const char *iotdata__reset_name(int r) {
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

static void iotdata__cmd_vers(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    const esp_app_desc_t *const d = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    iotdata_command_reply("app=%s vers=%s built=%s %s\n", d ? d->project_name : "?", d ? d->version : "?", d ? d->date : "?", d ? d->time : "?");
    iotdata_command_reply("platform=%s chip-rev=%d cores=%d idf=%s\n", CONFIG_IDF_TARGET, (int)chip.revision, (int)chip.cores, esp_get_idf_version());
}

static void iotdata__cmd_stat(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    iotdata_command_reply("uptime=%us reset=%s\n", (unsigned)(esp_timer_get_time() / 1000000), iotdata__reset_name((int)esp_reset_reason()));
    iotdata_command_reply("heap: free=%u min=%u largest=%u\n", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static void iotdata__cmd_logl(int argc, char **argv) {
    if (argc < 2) {
        iotdata_command_reply("log level = %s\n", iotdata__log_name(iotdata__log_level));
        return;
    }
    esp_log_level_t lvl;
    if (!iotdata__log_parse(argv[1], &lvl)) {
        iotdata_command_reply("logl: unknown level '%s' (none|error|warn|info|debug|verbose | 0..5)\n", argv[1]);
        return;
    }
    esp_log_level_set("*", lvl); /* dynamic ceiling is CONFIG_LOG_MAXIMUM_LEVEL — can't raise past it */
    iotdata__log_level = lvl;
    iotdata_command_reply("log level = %s\n", iotdata__log_name(lvl));
}

static void iotdata__cmd_boot(__attribute__ ((unused)) int argc, __attribute__ ((unused)) char **argv) {
    iotdata_command_reply("rebooting\n");
    fflush(stdout);
    esp_rom_delay_us(50000); /* let the reply drain out of the USB TX buffer before the reset */
    esp_restart();
}

#endif /* ESP_PLATFORM */

/* Built-ins mirror the iotdata telemetry TLVs, restricted to what is knowable generically:
 *   vers ~ VERSION TLV  (app name/version/build, platform/chip, idf)
 *   stat ~ STATUS + HEALTH TLVs, generic subset (session uptime, reset reason, heap)
 * The board/persistent HEALTH+STATUS fields — cpu_temp, supply_mv, restarts, lifetime uptime — need
 * hardware or cross-boot state the generic layer doesn't own, so an app plugs those in as its own
 * command (e.g. a `health`). */
static const iotdata_command_t iotdata__cmd_builtin[] = {
#if defined(ESP_PLATFORM)
    { "vers", iotdata__cmd_vers, "firmware / platform / build version" },
    { "stat", iotdata__cmd_stat, "runtime stats: uptime, reset reason, heap" },
    { "logl", iotdata__cmd_logl, "log level — 'logl' shows, 'logl <lvl>' sets" },
    { "boot", iotdata__cmd_boot, "restart the device" },
#endif
};
#define IOTDATA__BUILTIN_N (sizeof iotdata__cmd_builtin / sizeof iotdata__cmd_builtin[0])

static void iotdata__print_help(void) {
    iotdata_command_reply("commands:\n");
    iotdata_command_reply("  %-8s %s\n", "help", "list commands");
    for (size_t i = 0; i < IOTDATA__BUILTIN_N; i++)
        iotdata_command_reply("  %-8s %s\n", iotdata__cmd_builtin[i].name, iotdata__cmd_builtin[i].help ? iotdata__cmd_builtin[i].help : "");
    for (size_t i = 0; i < iotdata__cmd_app_n; i++)
        iotdata_command_reply("  %-8s %s\n", iotdata__cmd_app[i].name, iotdata__cmd_app[i].help ? iotdata__cmd_app[i].help : "");
}

static void iotdata__dispatch(char *line) {
    char *argv[IOTDATA_COMMAND_ARGC_MAX];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok != NULL && argc < IOTDATA_COMMAND_ARGC_MAX; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    if (argc == 0)
        return; /* blank line */
    if (strcmp(argv[0], "help") == 0) {
        iotdata__print_help();
        return;
    }
    for (size_t i = 0; i < IOTDATA__BUILTIN_N; i++) /* built-ins win a name clash */
        if (strcmp(argv[0], iotdata__cmd_builtin[i].name) == 0) {
            iotdata__cmd_builtin[i].fn(argc, argv);
            return;
        }
    for (size_t i = 0; i < iotdata__cmd_app_n; i++)
        if (strcmp(argv[0], iotdata__cmd_app[i].name) == 0) {
            iotdata__cmd_app[i].fn(argc, argv);
            return;
        }
    iotdata_command_reply("unknown command '%s' (try 'help')\n", argv[0]);
}

void iotdata_command_init(const iotdata_command_t *cmds, size_t count) {
    iotdata__cmd_app = cmds;
    iotdata__cmd_app_n = count;
    iotdata__cmd_len = 0;
#if defined(ESP_PLATFORM)
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 256, .rx_buffer_size = 256 };
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK)
        usb_serial_jtag_vfs_use_driver(); /* route stdio through the driver so reads + printf agree */
    iotdata_command_reply("iotdata-command: ready (type 'help')\n");
#endif
}

void iotdata_command_poll(void) {
#if defined(ESP_PLATFORM)
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) { /* timeout 0 → drain the FIFO, never block */
        if (c == '\r')
            continue;
        if (c == '\n') {
            iotdata__cmd_line[iotdata__cmd_len] = '\0';
            iotdata__dispatch(iotdata__cmd_line);
            iotdata__cmd_len = 0;
        } else if (iotdata__cmd_len < sizeof(iotdata__cmd_line) - 1) {
            iotdata__cmd_line[iotdata__cmd_len++] = (char)c;
        } else {
            iotdata__cmd_len = 0; /* overlong line → drop it */
        }
    }
#endif
}

#endif /* IOTDATA_COMMAND_IMPLEMENTATION */

#endif /* IOTDATA_COMMAND_H */
