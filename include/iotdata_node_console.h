
#ifndef IOTDATA_NODE_CONSOLE_H
#define IOTDATA_NODE_CONSOLE_H

/*
 * iotdata_node_console.h — a small, generic USB-serial-JTAG console line for esp32 apps.
 *
 * Non-blocking and verb-based: a set of built-in consoles (help, vers, stat, logl, boot) plus any
 * consoles the app plugs in on top. Designed to be driven from a cooperative main loop — call
 * iotdata_node_console_tick() each pass; it drains whatever the console has and dispatches a full line.
 * Output is plain printf (over the same USB-serial-JTAG console the app logs on), so records/status
 * come straight back over USB.
 *
 * Single-header: in ONE translation unit (the app's unity TU) define IOTDATA_NODE_CONSOLE_IMPLEMENTATION
 * before including; other TUs just include it for the declarations.
 *
 *   static void con_foo(iotdata_node_console_emit_fn emit, uint32_t now_ms, int argc, char **argv) { emit("foo!\n"); }
 *   static const iotdata_node_console_t app_cons[] = { { "foo", con_foo, "do the foo thing" } };
 *   iotdata_node_console_init(app_cons, sizeof app_cons / sizeof app_cons[0]);   // after boot
 *   ...
 *   for (;;) { ...; iotdata_node_console_tick(hw_time_ms()); }                     // non-blocking, each pass
 *
 * Pass (NULL, 0) for no app consoles. The cons array must live for the program's lifetime (static).
 * On non-esp32 (host) builds this compiles to no-ops, so shared code can include it unconditionally.
 *
 * esp32 component requirements (the TU that defines the implementation must be in a component that
 * REQUIRES these): esp_driver_usb_serial_jtag, vfs, esp_app_format, app_update (`vers` reports the
 * running slot through iotdata_node_firmware.h). (esp_timer / esp_hw_support /
 * heap / log / esp_system come in as common dependencies.)
 */

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#if !defined(ESP_PLATFORM)
#include <poll.h>
#include <unistd.h>
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
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* Command output is emitted as a pseudo log line "<TAG> (<ms>) <message>" — the same shape as an
 * ESP_LOG "<LEVEL> (<ms>) tag: msg" line — so a host monitor can grep/strip it by the leading level
 * letter (default 'C'), consistent with the I/W/E/D log lines it's interleaved with. Override the
 * letter before including if it clashes. */
#ifndef IOTDATA_NODE_CONSOLE_TAG
#define IOTDATA_NODE_CONSOLE_TAG "C"
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef void (*iotdata_node_console_emit_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* argv[0] is the console name.
 *
 * IT IS HANDED THE PASS'S CLOCK, AND MUST NOT READ ITS OWN. A handler that calls hw_time_ms()
 * itself gets a value LATER than the now_ms the rest of the pass is using -- the dispatch runs
 * after the radio work, so the two differ by however long that took. Hand such a stamp to anything
 * that then has a deadline checked against the pass clock and the unsigned difference underflows to
 * ~4.29e9, which clears every timeout at once. That is not a theoretical ordering: it is what made
 * `content insert` fail instantly on a relay, the insert stamped a few milliseconds ahead of the
 * tick that was to time it out. So the clock arrives as an argument, from the one read at the top
 * of the pass, and a handler that wants the time has no reason to look for it anywhere else. */
typedef void (*iotdata_node_console_fn)(iotdata_node_console_emit_fn emit, uint32_t now_ms, int argc, char **argv);

typedef struct {
    const char *name;           /* the verb typed at the console            */
    iotdata_node_console_fn fn; /* handler                              */
    const char *help;           /* one-line description for `help` (or NULL) */
} iotdata_node_console_t;

// ------------------------------------------------------------------------------------------------------------------------
// A SINK TAKES THE BYTES AWAY FROM THE VERB TABLE
//
// A verb is a line, and a line here is at most IOTDATA_NODE_CONSOLE_LINE_MAX bytes -- plenty for a
// command and far too little for a transfer. Rather than size that buffer for the largest thing
// anyone might ever send, a verb that is about to be sent bulk data installs a SINK: while one is
// installed every byte goes straight to it, and the line buffer and the verb table are not involved
// at all. The sink brings its own buffer, sized for its own framing, and only where it is used.
//
// It takes BYTES and not lines deliberately. The console then needs to know nothing about how the
// transfer is framed -- base64 lines, hex, offsets, acks, none of it -- and whoever adds a second
// bulk verb does not come back here to change this file.
//
// IT SAYS WHEN IT IS DONE BY TAKING LESS THAN IT WAS OFFERED, which is also what a sink does with
// the last line of its transfer: it consumes to the end of the data it was promised and leaves the
// rest, so a command typed on the far side of a transfer is dispatched instead of swallowed.
// Returning 0 is therefore the ordinary way to finish.
//
// NOTHING HERE TIMES IT OUT. A sink that is never finished with would hold the console shut, so
// whatever installs one is responsible for clearing it -- which in practice means its own tick
// noticing that the transfer it belongs to has been abandoned. The console cannot know that.
// ------------------------------------------------------------------------------------------------------------------------

typedef size_t (*iotdata_node_console_sink_fn)(void *ctx, const uint32_t now_ms, const uint8_t *buf, size_t len);

void iotdata_node_console_sink_set(iotdata_node_console_sink_fn fn, void *ctx);
void iotdata_node_console_sink_clear(void);
bool iotdata_node_console_sink_active(void);

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

/*
 * THE RECEIVE RING, AND WHY IT IS FOUR TIMES A COMMAND LINE.
 *
 * This is the esp32 driver's buffer, and it is the real limit on how much can be written at the
 * console in one go -- not LINE_MAX above, which only bounds a typed command.
 *
 * IT IS NOT FLOW CONTROLLED, AND THIS WAS MEASURED NOT ASSUMED. USB-serial-JTAG looks as though it
 * should NAK a host that is ahead of the device, and it does not: what will not fit here is
 * dropped. At 256 bytes a single 261-byte line arrived short every time. At 1024 a burst of ten
 * such lines landed as four -- with logging off, so this is the receive path and not a device
 * blocked in printf. The symptom is a base64 line whose length is no longer a multiple of four,
 * which the decoder refuses; the transport lost the bytes and the layers above merely noticed.
 *
 * Two things follow, and a module that streams through a SINK needs both. One LINE must fit here
 * with margin, which is a build-time property and is asserted in iotdata_node_console_content.h.
 * And the host must be told how far ahead it may run, because no error check at the far end can
 * recover bytes that were never received -- which is why the upload has a credit window.
 *
 * 2 KB of RAM is nothing on a node with a quarter of a megabyte free, and it buys a window several
 * lines wide: one round trip per kilobyte and a half instead of one per line.
 */
#ifndef IOTDATA_NODE_CONSOLE_RX_BYTES
#define IOTDATA_NODE_CONSOLE_RX_BYTES 2048
#endif
#ifndef IOTDATA_NODE_CONSOLE_TX_BYTES
#define IOTDATA_NODE_CONSOLE_TX_BYTES 256
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
static iotdata_node_console_sink_fn iotdata_node_con_sink = NULL;
static void *iotdata_node_con_sink_ctx = NULL;

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

/* THE IDENTITY THIS REPORTS IS THE ONE THE REST OF THE SYSTEM USES, which it did not used to be.
   It printed esp_app_desc_t and nothing else, and that descriptor answers a different question:
   project_name is the CMake project (iotdata_relay), version is a git describe (5b7555d-dirty), and
   date/time are deliberately BLANK under CONFIG_APP_REPRODUCIBLE_BUILD -- so a box running
   relay 0.9.10 reported `app=iotdata_relay vers=5b7555d-dirty built=`, with the release nowhere in
   it. An OTA blob is matched on app/semver/stamp (iotdata_node_content_ota.h) and the fleet reports
   VERSION over the air in the same grammar, so those are what "which version is this?" means.

   The first two lines are the boot banner's own, built by the same two calls, so what a reboot
   logged and what the console answers now cannot drift -- and after an update the one that matters
   is `firmware:`, which says which slot is running and whether it is still ON PROBATION.
   (Caps are the one thing the banner has and this does not: they live in the platform state, which
   is declared after this header. They are a build-time inventory, not an answer to this question.)

   iotdata_node_version.h and iotdata_node_firmware.h are both included ahead of this file by
   iotdata_node_platform.h -- the same arrangement iotdata_node_console_content.h relies on. */
static void iotdata_node_con_cmd_vers(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    const esp_app_desc_t *const d = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char vbuf[IOTDATA_NODE_VERSION_STR_MAX + 1], ibuf[IOTDATA_NODE_FIRMWARE_STR_MAX + 1];
    emit("version: %s\n", iotdata_node_version_str(vbuf, sizeof(vbuf), NULL));
    emit("firmware: %s\n", iotdata_node_firmware_str(ibuf, sizeof(ibuf)));
    emit("image: %s %s\n", d ? d->project_name : "?", d ? d->version : "?");
    emit("platform=%s chip-rev=%d cores=%d idf=%s\n", CONFIG_IDF_TARGET, (int)chip.revision, (int)chip.cores, esp_get_idf_version());
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_stat(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    emit("uptime=%us reset=%s\n", (unsigned)(hw_time_ms() / 1000), iotdata_node_con_reset_name((int)esp_reset_reason()));
    emit("heap: free=%u min=%u largest=%u\n", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_logl(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, int argc, char **argv) {
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

static void iotdata_node_con_cmd_boot(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
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

static void iotdata_node_console_dispatch(const uint32_t now_ms, char *line) {
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
            iotdata_node_con_builtin[i].fn(iotdata_node_console_reply, now_ms, argc, argv);
            return;
        }
    for (size_t i = 0; i < iotdata_node_con_app_n; i++)
        if (strcmp(argv[0], iotdata_node_con_app[i].name) == 0) {
            iotdata_node_con_app[i].fn(iotdata_node_console_reply, now_ms, argc, argv);
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
    if (usb_serial_jtag_driver_install(&(usb_serial_jtag_driver_config_t){ .tx_buffer_size = IOTDATA_NODE_CONSOLE_TX_BYTES, .rx_buffer_size = IOTDATA_NODE_CONSOLE_RX_BYTES }) == ESP_OK)
        usb_serial_jtag_vfs_use_driver(); /* route stdio through the driver so reads + printf agree */
#else
    /* Nothing to set up: the poll in getc() asks before it reads, so stdin is left exactly as the
       shell handed it over. A process with no terminal simply never has a byte ready. */
    iotdata_node_con_open = true;
#endif
    iotdata_node_console_reply("iotdata-console: ready (try 'help')\n");
}

// ------------------------------------------------------------------------------------------------------------------------

/* Whatever is waiting, up to `max`, or 0 if nothing is. NEVER BLOCKS on either platform, because
   this is called from the middle of a main loop that has a radio to service.
 *
 * A RUN AND NOT A BYTE, because a sink handed 200 KB one byte at a time through a function pointer
 * is doing a quarter of a million calls to copy a buffer it could have been given whole. The line
 * path walks the run itself, which costs it nothing. */
static size_t iotdata_node_con_read(uint8_t *const buf, const size_t max) {
#if defined(ESP_PLATFORM)
    const int n = usb_serial_jtag_read_bytes(buf, max, 0); /* timeout 0 -> drain the FIFO */
    return (n > 0) ? (size_t)n : 0u;
#else
    if (!iotdata_node_con_open)
        return 0u;
    /* ASKED FIRST, and only read once the answer is yes.
     *
     * The obvious alternative is O_NONBLOCK on stdin, and it is a trap on the host this runs on:
     * stdin and stdout of a process started from a terminal usually share one open file
     * description, so setting it there sets it for OUTPUT too -- and a gateway that logs every
     * frame would then start losing writes to EAGAIN the moment it got busy. poll() asks about the
     * one descriptor we mean, changes nothing, and cannot block with a zero timeout. */
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
        return 0u;
    const ssize_t n = read(STDIN_FILENO, buf, max);
    if (n > 0)
        return (size_t)n;
    if (n == 0)
        iotdata_node_con_open = false; /* EOF: stop asking, or a closed stdin spins the loop */
    return 0u;
#endif
}

// ------------------------------------------------------------------------------------------------------------------------

void iotdata_node_console_sink_set(const iotdata_node_console_sink_fn fn, void *const ctx) {
    iotdata_node_con_sink = fn;
    iotdata_node_con_sink_ctx = ctx;
    iotdata_node_con_len = 0; /* a part-typed line is not part of the transfer */
}

void iotdata_node_console_sink_clear(void) {
    iotdata_node_con_sink = NULL;
    iotdata_node_con_sink_ctx = NULL;
}

bool iotdata_node_console_sink_active(void) {
    return iotdata_node_con_sink != NULL;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The sink is re-checked every byte rather than once per run, because both transitions happen in
   the middle of one: a verb installs a sink and the data follows it in the same USB packet, and a
   sink finishes and leaves the next command behind it in the same buffer. */
void iotdata_node_console_tick(const uint32_t now_ms) {
    uint8_t chunk[64];
    size_t n;
    while ((n = iotdata_node_con_read(chunk, sizeof(chunk))) > 0u) {
        for (size_t i = 0; i < n;)
            if (iotdata_node_con_sink != NULL) {
                const size_t avail = n - i;
                size_t took = iotdata_node_con_sink(iotdata_node_con_sink_ctx, now_ms, chunk + i, avail);
                if (took > avail)
                    took = avail; /* a sink cannot have taken more than it was shown */
                i += took;
                if (took < avail)
                    iotdata_node_console_sink_clear(); /* less than it was offered: finished */
            } else {
                const char c = (char)chunk[i++];
                if (c == '\r') {
                    //
                } else if (c == '\n') {
                    iotdata_node_con_line[iotdata_node_con_len] = '\0';
                    iotdata_node_con_len = 0; /* cleared BEFORE the dispatch, which may install a sink */
                    iotdata_node_console_dispatch(now_ms, iotdata_node_con_line);
                } else if (iotdata_node_con_len < sizeof(iotdata_node_con_line) - 1)
                    iotdata_node_con_line[iotdata_node_con_len++] = c;
                else
                    iotdata_node_con_len = 0; /* overlong line -> drop it */
            }
        if (n < sizeof(chunk))
            break; /* a short read is everything there was */
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_H */
