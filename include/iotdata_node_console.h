
#ifndef IOTDATA_NODE_CONSOLE_H
#define IOTDATA_NODE_CONSOLE_H

/*
 * iotdata_node_console.h — a small, generic console line: the verbs, and none of the wiring.
 *
 * Non-blocking and verb-based: a set of built-in consoles (help, vers, stat, logl, boot) plus any
 * consoles the app plugs in on top. Designed to be driven from a cooperative main loop — call
 * iotdata_node_console_tick() each pass; it drains whatever the console has and dispatches a full line.
 * Output is plain printf, over whatever the application already logs on, so records and status come
 * straight back the way they were asked for.
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
 *
 * THE LINE BELONGS TO THE PLATFORM, and so does each verb's platform-shaped half: see
 * device/d_console_<platform>.h, which the application includes beside its other d_ modules, and
 * which lists the ESP-IDF components a device build needs. The five verbs are the same everywhere --
 * they were ESP-only until the wiring moved out from under them, which made a gateway answer
 * `unknown command 'vers'` about a question it could answer.
 */

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* Command output is emitted as a pseudo log line "<TAG> (<ms>) <message>" — the same shape as an
 * ESP_LOG "<LEVEL> (<ms>) tag: msg" line — so a host monitor can grep/strip it by the leading level
 * letter (default 'C'), consistent with the I/W/E/D log lines it's interleaved with.
 */
#ifndef IOTDATA_NODE_CONSOLE_TAG
#define IOTDATA_NODE_CONSOLE_TAG "C"
#endif

#ifndef IOTDATA_NODE_CONSOLE_PREFIX
#define IOTDATA_NODE_CONSOLE_PREFIX() printf(IOTDATA_NODE_CONSOLE_TAG " (%u) ", (unsigned)console_stamp_ms()) /* the platform's own log clock */
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
    IOTDATA_NODE_CONSOLE_PREFIX();
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

static const iotdata_node_console_t *iotdata_node_con_app = NULL;
static size_t iotdata_node_con_app_n = 0;
static char iotdata_node_con_line[IOTDATA_NODE_CONSOLE_LINE_MAX];
static size_t iotdata_node_con_len = 0;
static iotdata_node_console_sink_fn iotdata_node_con_sink = NULL;
static void *iotdata_node_con_sink_ctx = NULL;

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
    char vbuf[IOTDATA_NODE_VERSION_STR_MAX + 1], ibuf[IOTDATA_NODE_FIRMWARE_STR_MAX + 1], pbuf[80];
    emit("version: %s\n", iotdata_node_version_str(vbuf, sizeof(vbuf), NULL));
    emit("firmware: %s\n", iotdata_node_firmware_str(ibuf, sizeof(ibuf)));
    emit("platform: %s\n", console_platform_str(pbuf, sizeof(pbuf)));
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_stat(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    /* The reason is the PROTOCOL's name for it, the same one STATUS reports over the air, rather
       than a second vocabulary for the same fact at the console. */
    emit("uptime=%us reset=%s\n", (unsigned)(hw_time_ms() / 1000), iotdata_node_tlv_status_reason_str(iotdata_node_reason_reset()));
    uint32_t heap_free = 0, heap_min = 0, heap_largest = 0;
    if (console_heap(&heap_free, &heap_min, &heap_largest))
        emit("heap: free=%u min=%u largest=%u\n", (unsigned)heap_free, (unsigned)heap_min, (unsigned)heap_largest);
    else
        emit("heap: not reported on this platform\n");
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_logl(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, int argc, char **argv) {
    if (argc < 2) {
        emit("log level = %s\n", console_log_level_name(platform_log_level_get()));
        return;
    }
    platform_log_level_t lvl;
    if (!console_log_level_parse(argv[1], &lvl)) {
        emit("logl: unknown level '%s' (none|error|warn|info|debug|verbose | 0..5)\n", argv[1]);
        return;
    }
    /* A CEILING, NOT A FLOOR: a build that compiled a level out stays quiet however high this goes
       (CONFIG_LOG_MAXIMUM_LEVEL on a device), so the answer is read back rather than assumed. */
    platform_log_level_set(lvl);
    emit("log level = %s\n", console_log_level_name(platform_log_level_get()));
}

// ------------------------------------------------------------------------------------------------------------------------

static void iotdata_node_con_cmd_boot(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, __attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
    emit("rebooting\n");
    console_restart(); /* never returns: a reset on a device, an exit on a host */
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static const iotdata_node_console_t iotdata_node_con_builtin[] = {
    { "vers", iotdata_node_con_cmd_vers, "device firmware / platform / build version" },
    { "stat", iotdata_node_con_cmd_stat, "device uptime, reset reason, heap" },
    { "logl", iotdata_node_con_cmd_logl, "device loglevel — 'logl' shows, 'logl <lvl>' sets" },
    { "boot", iotdata_node_con_cmd_boot, "device restart" },
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
    (void)console_line_begin(IOTDATA_NODE_CONSOLE_RX_BYTES, IOTDATA_NODE_CONSOLE_TX_BYTES);
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
    return console_line_read(buf, max);
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
