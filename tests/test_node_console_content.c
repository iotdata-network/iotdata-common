//
// test_node_console_content.c - the store's console and CONTROL words, on the host.
//
// The half of the content work that a device was going to be needed for, done here instead. Every
// piece of it is ordinary code: the console's line assembly and sink run on a host (the USB-JTAG
// driver is the only thing behind #if defined(ESP_PLATFORM)), the framing is text, base64 is
// arithmetic, and the store underneath is the RAM backend. What is left for a device is SPIFFS, and
// nothing else.
//
// THIS ONE DOES CAPTURE STDOUT, unlike test_node_console.c, and the difference is the point. There
// a reply was a reply and what mattered was that the right handler ran. Here the reply IS the
// protocol -- `d <offset> <base64>` is what esp32-tool parses -- so a test that trusted the
// handler to have meant well would not be testing the thing that can break.
//
// THE SINK IS FED DIRECTLY rather than through stdin. iotdata_node_console_tick() reads a real
// descriptor, and a test that pushed bytes into stdin would be testing the read rather than the
// framing; the sink is a plain function taking a buffer, so the awkward cases -- a line split
// across two reads, a transfer that ends exactly on a buffer boundary, a command arriving behind
// the last data line -- are reachable exactly and on purpose. The one case that needs the console
// itself is that a verb can install a sink and the data behind it in the same read, and that goes
// through poll's own loop.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "device/d_format.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"
#include "iotdata_node_console.h"
#include "iotdata_node_content.h"
#include "d_module_filestore_ram.h"
#include "iotdata_node_content_control.h"
#include "iotdata_node_console_content.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */
/* stdout, caught in a file and read back. fd 1 and not the FILE*, so that printf's buffering is
   not something every assertion has to reason about. */

#define CAP_MAX 65536
static char cap[CAP_MAX];
static int cap_saved = -1;
static FILE *cap_file = NULL;

static void cap_begin(void) {
    fflush(stdout);
    cap_file = tmpfile();
    cap_saved = dup(1);
    (void)dup2(fileno(cap_file), 1);
}

static const char *cap_end(void) {
    fflush(stdout);
    (void)dup2(cap_saved, 1);
    (void)close(cap_saved);
    rewind(cap_file);
    const size_t n = fread(cap, 1u, sizeof(cap) - 1u, cap_file);
    cap[n] = '\0';
    (void)fclose(cap_file);
    cap_file = NULL;
    return cap;
}

/* ------------------------------------------------------------------------------------------- */

static char said[64][IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX];
static int said_n = 0;

static void say(const char *const line) {
    if (said_n < (int)(sizeof(said) / sizeof(said[0])))
        snprintf(said[said_n++], sizeof(said[0]), "%s", line);
}

static void said_reset(void) {
    said_n = 0;
    memset(said, 0, sizeof(said));
}

static bool said_has(const char *const needle) {
    for (int i = 0; i < said_n; i++)
        if (strstr(said[i], needle) != NULL)
            return true;
    return false;
}

/* ------------------------------------------------------------------------------------------- */

static filestore_ram_t fs;
static iotdata_node_content_t store;
static uint32_t clock_ms = 1000u;

static void rig(void) {
    const iotdata_node_content_backend_t *const be = filestore_ram_init(&fs);
    iotdata_node_content_init(&store, be);
    (void)iotdata_node_content_begin(&store, clock_ms); /* the tick does nothing until it has */
    iotdata_node_console_content_init(&store);
    iotdata_node_content_control_init(&store, NULL);
    (void)iotdata_node_content_emit_set(say);
    iotdata_node_console_sink_clear();
    said_reset();
}

/* One verb, as the console would dispatch it: strtok over a mutable copy, exactly as
   iotdata_node_console_dispatch does, so argv looks the way the real one looks. */
static bool verb(const char *const cmdline) {
    char line[160];
    char *argv[IOTDATA_NODE_CONSOLE_ARGC_MAX];
    int argc = 0;
    snprintf(line, sizeof(line), "%s", cmdline);
    for (char *tok = strtok(line, " \t"); tok != NULL && argc < IOTDATA_NODE_CONSOLE_ARGC_MAX; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    return iotdata_node_console_content(clock_ms, argc, argv);
}

static size_t sink(const char *const s) {
    return _iotdata_node_console_content_sink(&_iotdata_node_console_content, clock_ms, (const uint8_t *)s, strlen(s));
}

/* A `d <offset> <base64>` line for one slice, built the way the host will build it. */
static void line_for(char *const out, const size_t size, const uint8_t *const data, const uint32_t off, const uint32_t len) {
    char b64[IOTDATA_NODE_CONSOLE_CONTENT_B64 + 1];
    (void)_iotdata_node_console_content_encode(data + off, len, b64);
    snprintf(out, size, "d %u %s\n", (unsigned)off, b64);
}

static void upload(const uint8_t *const data, const uint32_t len, const uint32_t skip_line) {
    char l[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    uint32_t n = 0;
    for (uint32_t off = 0; off < len; off += IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD, n++) {
        const uint32_t want = ((len - off) < IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD) ? (len - off) : IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD;
        if (n == skip_line)
            continue;
        line_for(l, sizeof(l), data, off, want);
        (void)sink(l);
    }
}

static iotdata_node_content_event_t tick(void) {
    const iotdata_node_content_event_t ev = iotdata_node_content_tick(&store, clock_ms);
    iotdata_node_console_content_tick(clock_ms);
    return ev;
}

static void fill(uint8_t *const buf, const uint32_t len, const uint8_t seed) {
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + (i * 31u) + (i >> 5));
}

/* ------------------------------------------------------------------------------------------- */

static void test_base64(void) {
    printf("\nbase64 round-trips, and refuses rather than guesses\n");
    uint8_t in[200], out[256];
    char b64[512];
    fill(in, sizeof(in), 7u);
    bool all = true;
    for (size_t len = 0; len <= sizeof(in); len++) {
        const size_t n = _iotdata_node_console_content_encode(in, len, b64);
        if (len == 0u) {
            all = all && (n == 0u) && (b64[0] == '\0');
            continue;
        }
        all = all && (n == ((len + 2u) / 3u) * 4u);
        const int back = _iotdata_node_console_content_decode(b64, out, sizeof(out));
        all = all && (back == (int)len) && (memcmp(in, out, len) == 0);
    }
    CHECK(all, "every length from 1 to 200 comes back exactly");

    /* The one encoding the framing depends on: 192 bytes is 256 characters with no padding, which
       is why that payload size was chosen and what a host will assume. */
    CHECK(_iotdata_node_console_content_encode(in, IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD, b64) == IOTDATA_NODE_CONSOLE_CONTENT_B64, "a full line is exactly B64 characters");
    CHECK(strchr(b64, '=') == NULL, "and needs no padding");

    CHECK(_iotdata_node_console_content_decode("", out, sizeof(out)) < 0, "an empty string is not a line");
    CHECK(_iotdata_node_console_content_decode("QUJD", out, sizeof(out)) == 3, "QUJD is ABC");
    CHECK(_iotdata_node_console_content_decode("QUJDR", out, sizeof(out)) < 0, "a length that is not a multiple of four");
    CHECK(_iotdata_node_console_content_decode("QU!D", out, sizeof(out)) < 0, "a character outside the alphabet");
    CHECK(_iotdata_node_console_content_decode("QU=D", out, sizeof(out)) < 0, "a pad with something after it in its own group");
    CHECK(_iotdata_node_console_content_decode("QUJDQUJD", out, sizeof(out)) == 6, "two groups");
    CHECK(_iotdata_node_console_content_decode("QU==QUJD", out, sizeof(out)) < 0, "padding anywhere but the last group");
    CHECK(_iotdata_node_console_content_decode("QUJD", out, 2u) < 0, "and it refuses rather than overrun what it was given");
}

/* ------------------------------------------------------------------------------------------- */

static void test_insert(void) {
    printf("\nan upload arrives, is summed, and lands at its real id\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 3u);
    const uint64_t crc = iotdata_node_crc64(data, sizeof(data), IOTDATA_NODE_CRC64_INIT);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a1 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    const bool took = verb(cmd);
    const char *out = cap_end();
    CHECK(took, "`content insert` is the console's own word");
    CHECK(strstr(out, "insert 0537a1 ready") != NULL, "and it says it is ready");
    /* The device announces the line size rather than the host assuming it: a build option that a
       host had hardcoded would break silently the day it changed. */
    char want[64];
    snprintf(want, sizeof(want), "lines of up to %u", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD);
    CHECK(strstr(out, want) != NULL, "and how long a line it will take");
    CHECK(iotdata_node_console_sink_active(), "the sink is installed");

    /* Nothing is at the real id yet: the bytes go to scratch and only a passed checksum renames. */
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537a1u, &total, &held), "nothing at the id while it is in flight");

    cap_begin();
    upload(data, sizeof(data), 0xFFFFFFFFu);
    (void)cap_end();
    CHECK(!iotdata_node_console_sink_active() || !_iotdata_node_console_content.up, "the sink stands down when the last byte has been offered");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    iotdata_node_console_content_event(ev);
    out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "the tick says it arrived");
    CHECK(strstr(out, "arrived") != NULL, "and says so");
    CHECK(iotdata_node_content_stat(&store, 0x0537a1u, &total, &held), "and it is at the real id now");
    CHECK(total == sizeof(data) && held == sizeof(data), "whole");

    uint8_t back[sizeof(data)];
    CHECK(iotdata_node_content_read(&store, 0x0537a1u, 0u, back, sizeof(back)) == (int)sizeof(back), "readable");
    CHECK(memcmp(data, back, sizeof(data)) == 0, "and byte for byte what was sent");

    /* And the store agrees with itself afterwards, which the insert's own running sum cannot say. */
    uint64_t sum = 0u;
    uint32_t len = 0u;
    CHECK(iotdata_node_content_checksum(&store, 0x0537a1u, &sum, &len) == IOTDATA_NODE_CONTENT_OK, "and sums complete");
    CHECK(sum == crc, "to the checksum that was declared before a byte was sent");
}

/* ------------------------------------------------------------------------------------------- */

static void test_dropped_line(void) {
    printf("\na dropped line is caught by its offset, and the rest is swallowed not obeyed\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 11u);
    const uint64_t crc = iotdata_node_crc64(data, sizeof(data), IOTDATA_NODE_CRC64_INIT);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a2 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    (void)verb(cmd);
    upload(data, sizeof(data), 2u); /* line 2 never sent */
    (void)cap_end();

    /* THE SINK STAYED INSTALLED to the declared end: the alternative is reading the remaining
       base64 as commands, and `d 576 SGVsbG8...` is not a command anybody wants resolved. */
    CHECK(_iotdata_node_console_content.up_lines == 5u, "every line that was sent was consumed");
    CHECK(!_iotdata_node_console_content.up, "and the sink stood down at the end, not at the error");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    iotdata_node_console_content_event(ev);
    const char *out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the insert failed");
    CHECK(iotdata_node_content_insert_why(&store) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "at the offset, before the checksum had to save it");
    CHECK(strstr(out, "not the bytes expected next") != NULL, "and says which");
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537a2u, &total, &held), "and nothing was left behind at the id");
}

static void test_corrupt_line(void) {
    printf("\na line that will not decode is a hole, and is treated as one\n");
    rig();
    uint8_t data[400];
    fill(data, sizeof(data), 5u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a3 %u 0000000000000000", (unsigned)sizeof(data));
    cap_begin();
    (void)verb(cmd);
    char l[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    line_for(l, sizeof(l), data, 0u, IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD);
    (void)sink(l);
    (void)sink("d 192 not!valid!base64!\n");
    line_for(l, sizeof(l), data, 384u, 16u);
    (void)sink(l);
    (void)cap_end();
    CHECK(_iotdata_node_console_content.up_bad == 1u, "the bad line was counted");
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "and the insert died rather than leaving a gap");
}

static void test_end_abandons(void) {
    printf("\n`end` gives the console back without waiting out the timeout\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 9u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a4 %u 0000000000000000", (unsigned)sizeof(data));
    cap_begin();
    (void)verb(cmd);
    upload(data, 384u, 0xFFFFFFFFu); /* two lines of six, then second thoughts */
    const size_t took = sink("end\nstat\n");
    const char *out = cap_end();
    CHECK(took == 4u, "the sink took `end\\n` and left `stat\\n` for the console");
    CHECK(!_iotdata_node_console_content.up, "it stood down");
    CHECK(strstr(out, "abandoned") != NULL, "and said so");
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the insert was abandoned too, not left in flight");
    CHECK(!iotdata_node_console_sink_active(), "and the console is back");
}

static void test_timeout_frees_the_console(void) {
    printf("\na host that walks away does not keep the console shut\n");
    rig();
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a5 1000 0000000000000000");
    cap_begin();
    (void)verb(cmd);
    (void)sink("d 0 QUJD\n");
    (void)cap_end();
    CHECK(iotdata_node_console_sink_active(), "the sink is installed and the host is gone");
    clock_ms += (IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S + 1u) * 1000u;
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    const char *out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the module timed it out");
    CHECK(strstr(out, "gave up") != NULL, "and the console said so");
    /* THE AUTHORITY IS THE MODULE'S TIMEOUT and the console follows it: nothing in
       iotdata_node_console.h knows what a transfer is, so nothing there could have done this. */
    CHECK(!iotdata_node_console_sink_active(), "and the sink was cleared, so a verb can be typed again");
    clock_ms = 1000u;
}

/* ------------------------------------------------------------------------------------------- */
/* The credit window. Not an error check -- the offsets do that -- but the only thing that stops a
   host outrunning a device whose transport drops what will not fit. */

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle))
        n++;
    return n;
}

static void test_window(void) {
    printf("\nthe host is given credit, and is given more of it as the bytes go by\n");
    rig();
    /* Three windows and a bit, so there is an ack per window AND a short last one. */
    const uint32_t len = 3u * IOTDATA_NODE_CONSOLE_CONTENT_WINDOW + IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD;
    uint8_t *data = (uint8_t *)malloc(len);
    CHECK(data != NULL, "a buffer");
    if (data == NULL)
        return;
    fill(data, len, 77u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537f9 %u %016llx", (unsigned)len, (unsigned long long)iotdata_node_crc64(data, len, IOTDATA_NODE_CRC64_INIT));

    cap_begin();
    (void)verb(cmd);
    const char *out = cap_end();
    char want[64];
    snprintf(want, sizeof(want), "window %u", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW);
    CHECK(strstr(out, want) != NULL, "the ready line says how far ahead the host may run");

    cap_begin();
    upload(data, len, 0xFFFFFFFFu);
    out = cap_end();
    /* One per full window, plus one for the short tail -- the tail's is unconditional, because a
       host waiting on credit for a last short window would wait out the insert timeout instead. */
    CHECK(count_of(out, " ack ") == 4, "an ack per window, and one at the end");
    char at[64];
    snprintf(at, sizeof(at), " ack %u\n", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW);
    CHECK(strstr(out, at) != NULL, "the first lands exactly on the window");
    snprintf(at, sizeof(at), " ack %u\n", (unsigned)len);
    CHECK(strstr(out, at) != NULL, "and the last on the final byte");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and the file landed");

    /* A REFUSED insert must keep acking. The credit counts what was OFFERED, so a host feeding to
       the declared end after a dropped line is still let through rather than stalling on a window
       that will never open again. */
    rig();
    cap_begin();
    (void)verb(cmd);
    (void)cap_end();
    cap_begin();
    upload(data, len, 1u); /* line 1 never sent: doomed from the second line onwards */
    out = cap_end();
    CHECK(count_of(out, " ack ") == 4, "a doomed insert is credited exactly the same");
    CHECK(!_iotdata_node_console_content.up, "and still stands down at the declared end");
    free(data);
}

/* ------------------------------------------------------------------------------------------- */

static void test_sink_boundaries(void) {
    printf("\nthe sink gives back exactly what is not its own\n");
    rig();
    uint8_t data[384];
    fill(data, sizeof(data), 21u);
    const uint64_t crc = iotdata_node_crc64(data, sizeof(data), IOTDATA_NODE_CRC64_INIT);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537b1 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    (void)verb(cmd);

    /* Both data lines and a command behind them, in ONE buffer -- which is what a USB packet looks
       like when a host writes the whole thing at once. */
    char buf[2 * (IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32) + 16];
    char a[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], b[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    line_for(a, sizeof(a), data, 0u, 192u);
    line_for(b, sizeof(b), data, 192u, 192u);
    snprintf(buf, sizeof(buf), "%s%sstat\n", a, b);
    const size_t n = sink(buf);
    (void)cap_end();
    CHECK(n == strlen(a) + strlen(b), "it took both data lines");
    CHECK(n < strlen(buf), "and less than it was offered, which is how it says it has finished");
    CHECK(strcmp(buf + n, "stat\n") == 0, "leaving the command behind it untouched");

    /* The other boundary: the transfer ends exactly where the buffer does, so there is nothing
       left to hand back and the sink has to stand down on the NEXT call instead. */
    rig();
    cap_begin();
    (void)verb(cmd);
    snprintf(buf, sizeof(buf), "%s%s", a, b);
    const size_t m = sink(buf);
    const size_t again = sink("stat\n");
    (void)cap_end();
    CHECK(m == strlen(buf), "it took the whole buffer");
    CHECK(again == 0u, "and then took none of the next one");

    /* And a line split across two reads is still one line. */
    rig();
    cap_begin();
    (void)verb(cmd);
    char half[256];
    snprintf(half, sizeof(half), "%.40s", a);
    (void)sink(half);
    (void)sink(a + 40);
    (void)cap_end();
    CHECK(_iotdata_node_console_content.up_lines == 1u, "a line split across two reads is one line");
    CHECK(_iotdata_node_console_content.up_bad == 0u, "and not a broken one");
}

/* The one case that needs poll() itself: a verb and the data behind it in the SAME read. The sink
   is installed by the dispatch, mid-buffer, and the bytes behind it must reach the sink rather than
   the verb table -- which is the whole reason poll() re-checks the sink per byte and not per read. */
static void con_content(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const int argc, char **const argv) {
    (void)iotdata_node_console_content(clock_ms, argc, argv);
}

static void test_through_poll(void) {
    printf("\na verb can install a sink and have its data follow in the same read\n");
    rig();
    uint8_t data[384];
    fill(data, sizeof(data), 33u);
    const uint64_t crc = iotdata_node_crc64(data, sizeof(data), IOTDATA_NODE_CRC64_INIT);
    static const iotdata_node_console_t cons[] = { { "content", con_content, "the store" } };
    char a[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], b[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], all[2048];
    line_for(a, sizeof(a), data, 0u, 192u);
    line_for(b, sizeof(b), data, 192u, 192u);
    snprintf(all, sizeof(all), "content insert 0537c1 %u %016llx\n%s%s", (unsigned)sizeof(data), (unsigned long long)crc, a, b);

    /* A pipe on fd 0, so this goes through the host transport exactly as it ships -- poll() asking
       first, then a chunked read -- rather than through a test-only entry point. */
    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe stands in for the cable");
    const int saved = dup(0);
    (void)dup2(fds[0], 0);
    CHECK(write(fds[1], all, strlen(all)) == (ssize_t)strlen(all), "written in one go");
    cap_begin();
    iotdata_node_console_init(cons, 1u); /* opens the host transport and clears the line buffer */
    iotdata_node_console_tick(clock_ms);
    const iotdata_node_content_event_t ev = tick();
    const char *out = cap_end();
    (void)dup2(saved, 0);
    (void)close(saved);
    (void)close(fds[0]);
    (void)close(fds[1]);

    CHECK(_iotdata_node_console_content.up_lines == 2u, "both data lines reached the sink");
    CHECK(_iotdata_node_console_content.up_bad == 0u, "and none of them reached the verb table");
    CHECK(strstr(out, "unknown command") == NULL, "which is what that would have looked like");
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and the file landed");
    uint8_t back[sizeof(data)];
    CHECK(iotdata_node_content_read(&store, 0x0537c1u, 0u, back, sizeof(back)) == (int)sizeof(back), "readable");
    CHECK(memcmp(back, data, sizeof(data)) == 0, "byte for byte");
}

/* ------------------------------------------------------------------------------------------- */

static void test_download(void) {
    printf("\na download comes back byte for byte, a slice at a time\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 17u);
    CHECK(filestore_ram_plant(&fs, 0x0537d1u, data, sizeof(data)), "planted, as a factory image would be");

    cap_begin();
    const bool took = verb("content download 0537d1");
    const char *out = cap_end();
    CHECK(took, "`content download` is the console's own word");
    CHECK(strstr(out, "download 0537d1 begin, 1000 bytes") != NULL, "and declares the length up front");
    CHECK(strstr(out, "INCOMPLETE") == NULL, "a whole file is not flagged");

    /* Ticked, not dumped: a line a pass, with the radio serviced in between. */
    cap_begin();
    int passes = 0;
    while (_iotdata_node_console_content.down && passes < 100) {
        (void)tick();
        passes++;
    }
    out = cap_end();
    CHECK(passes == 1 + (int)((sizeof(data) + IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD - 1u) / IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD), "one pass per line, plus the pass that finishes it");

    /* Reassembled from the replies, the way esp32-tool will do it. */
    uint8_t got[sizeof(data)];
    uint32_t seen = 0u;
    memset(got, 0, sizeof(got));
    for (const char *p = out; (p = strstr(p, " d ")) != NULL;) {
        p += 3;
        char *end = NULL;
        const unsigned long off = strtoul(p, &end, 10);
        CHECK(end != NULL && *end == ' ', "an offset and a space");
        char b64[IOTDATA_NODE_CONSOLE_CONTENT_B64 + 1];
        snprintf(b64, sizeof(b64), "%.*s", (int)strcspn(end + 1, "\n"), end + 1);
        uint8_t raw[IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD];
        const int n = _iotdata_node_console_content_decode(b64, raw, sizeof(raw));
        CHECK(n > 0, "a line that decodes");
        if (n > 0 && off + (unsigned long)n <= sizeof(got)) {
            memcpy(got + off, raw, (size_t)n);
            seen += (uint32_t)n;
        }
        p = end;
    }
    CHECK(seen == sizeof(data), "every byte came back");
    CHECK(memcmp(got, data, sizeof(data)) == 0, "and they are the bytes that were there");

    char want[80];
    snprintf(want, sizeof(want), "crc64=0x%016llX", (unsigned long long)iotdata_node_crc64(data, sizeof(data), IOTDATA_NODE_CRC64_INIT));
    CHECK(strstr(out, want) != NULL, "and the trailer's sum is the file's sum");
    CHECK(strstr(out, "done, 1000 bytes") != NULL, "and it says it finished");
}

static void test_download_refusals(void) {
    printf("\nand a download refuses what it cannot do\n");
    rig();
    cap_begin();
    (void)verb("content download 0537ff");
    const char *out = cap_end();
    CHECK(strstr(out, "refused") != NULL, "no such id");

    /* An incomplete file downloads what is there and says it is not all of it, rather than
       refusing: half a file is sometimes exactly what is wanted when working out why. */
    uint8_t data[400];
    fill(data, sizeof(data), 41u);
    (void)iotdata_node_content_insert_begin(&store, 0x0537e1u, sizeof(data), 0u, clock_ms);
    (void)iotdata_node_content_insert_write(&store, 0u, data, 192u, clock_ms);
    cap_begin();
    (void)verb("content download 000001"); /* the scratch the insert is writing to */
    out = cap_end();
    CHECK(strstr(out, "INCOMPLETE") != NULL, "a partial file says so");
    CHECK(strstr(out, "begin, 192 bytes") != NULL, "and offers what it has");

    /* One job at a time: the insert owns the console until it is done. */
    cap_begin();
    (void)verb("content insert 0537e2 100 0");
    out = cap_end();
    CHECK(strstr(out, "refused") != NULL, "a second insert is refused while one is in flight");
}

/* ------------------------------------------------------------------------------------------- */

static void test_control_words(void) {
    printf("\nlist, delete and validate, through the handler a radio would reach\n");
    rig();
    uint8_t a[300], b[64];
    fill(a, sizeof(a), 1u);
    fill(b, sizeof(b), 2u);
    CHECK(filestore_ram_plant(&fs, 0x0537f1u, a, sizeof(a)), "two files");
    CHECK(filestore_ram_plant(&fs, 0x0537f2u, b, sizeof(b)), "planted");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST, NULL, 0u), "list is ours");
    CHECK(said_has("0537f1"), "and lists the first");
    CHECK(said_has("0537f2"), "and the second");
    CHECK(said_has("2 files"), "and counts them last");

    /* The id is read back out of the three bytes iotdata_node_control_build writes, which is the
       only check that the two layers agree about the width of a content id. */
    said_reset();
    const uint8_t id_f1[3] = { 0x05u, 0x37u, 0xf1u };
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_VALIDATE, id_f1, 3u), "validate is ours");
    char want[80];
    snprintf(want, sizeof(want), "crc64=0x%016llX", (unsigned long long)iotdata_node_crc64(a, sizeof(a), IOTDATA_NODE_CRC64_INIT));
    CHECK(said_has(want), "and reports the store's own sum");
    CHECK(!said_has("INCOMPLETE"), "over a whole file");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, id_f1, 3u), "delete is ours");
    CHECK(said_has("deleted"), "and says so");
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537f1u, &total, &held), "and it is gone");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, id_f1, 3u), "a second delete is still ours");
    CHECK(!said_has("deleted"), "but deletes nothing");
    /* And says WHY in the words an operator needs: a well-formed id with nothing under it is not
       the same complaint as an id that could never name anything. */
    CHECK(said_has("no such content id"), "and distinguishes absent from malformed");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, NULL, 0u), "an id-less command is OURS and refused");
    CHECK(said_has("no id"), "and says which");
    CHECK(!iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, 0x7Fu, NULL, 0u), "an action we do not have is not ours");
    CHECK(!iotdata_node_content_control(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_CONTENT_LIST, NULL, 0u), "nor another subject's");
}

/* ------------------------------------------------------------------------------------------- */
/* `content inspect` over faux bundles. The header parser has its own suite; what is new here is
   that a blob in the STORE is found, read out of it, parsed and judged -- and that the length check
   gets the length from what the store is actually holding, which is the one check that cannot be
   made from a header alone. */

static const uint8_t HASH_T[IOTDATA_NODE_OTA_HASH_LEN] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };

static uint32_t faux(const uint32_t id, const char *const app, const uint32_t outsize, const uint32_t paysize, const uint32_t payload_bytes) {
    uint8_t blob[512];
    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, "202610111200");
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_FULL);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_NONE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, outsize);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_TARGET_HASH, HASH_T, IOTDATA_NODE_OTA_HASH_LEN);
    if (paysize != 0u)
        iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paysize);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    for (uint32_t i = 0; i < payload_bytes && head + i < sizeof(blob); i++)
        blob[head + i] = (uint8_t)(i * 7u);
    return filestore_ram_plant(&fs, id, blob, head + payload_bytes) ? head : 0u;
}

static void test_inspect_ota(void) {
    printf("\ninspect reads a bundle out of the store and judges it\n");
    rig();
    static const iotdata_node_ota_device_t relay = {
        .app = "relay",
        .stamp = NULL,
        .bootloader = 1u,
        .slot_size = 0x100000u,
        .base_hash = NULL,
        .codec_deflate = true,
    };
    iotdata_node_content_control_init(&store, &relay);
    const uint8_t id_ok[3] = { 0x05u, 0x38u, 0x01u }, id_app[3] = { 0x05u, 0x38u, 0x02u }, id_short[3] = { 0x05u, 0x38u, 0x03u };
    const uint8_t id_plain[3] = { 0x05u, 0x38u, 0x04u }, id_trunc[3] = { 0x05u, 0x38u, 0x05u };

    const uint32_t head = faux(0x053801u, "relay", 0x40000u, 64u, 64u);
    CHECK(head > 0u, "a bundle this build could take");
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_ok, 3u), "inspect is ours");
    CHECK(said_has("app=relay semver=1.2.3 stamp=202610111200"), "the identity");
    CHECK(said_has("form=full codec=none"), "the shape");
    CHECK(said_has("payload=64"), "and what the payload should weigh");
    CHECK(said_has("ota check: ok"), "and this build would take it");
    CHECK(said_has("nobody has claimed it"), "while no consumer owns it, which is true and worth saying");

    (void)faux(0x053802u, "gateway", 0x40000u, 64u, 64u);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_app, 3u);
    CHECK(said_has("app=gateway"), "a bundle for something else parses");
    CHECK(said_has("ota check: different application"), "and is refused for the right reason");

    /* The one that needs the store: a header claiming 64 payload bytes over a file that holds 32.
       Nothing in the header is wrong -- it is the FILE that is short, and only the store knows. */
    (void)faux(0x053803u, "relay", 0x40000u, 64u, 32u);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_short, 3u);
    CHECK(said_has("ota check: not equal to the header length"), "a short file is caught here, not at the far end of a decode");

    /* Most content is not firmware, and inspect says nothing about OTA for it rather than
       announcing that it failed to be something nobody claimed it was. */
    uint8_t plain[200];
    fill(plain, sizeof(plain), 61u);
    CHECK(filestore_ram_plant(&fs, 0x053804u, plain, sizeof(plain)), "an ordinary file");
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_plain, 3u);
    CHECK(said_has("053804 200 of 200 bytes"), "is described");
    CHECK(!said_has("ota"), "and is not accused of being a bundle");

    /* The magic and nothing else: it IS claiming to be one, so saying why it is not readable is
       the useful answer. */
    CHECK(filestore_ram_plant(&fs, 0x053805u, IOTDATA_NODE_OTA_MAGIC, (uint32_t)IOTDATA_NODE_OTA_MAGIC_LEN), "a bundle that is only its magic");
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_trunc, 3u);
    CHECK(said_has("ota header:"), "says the header is the problem");
    CHECK(!said_has("ota check:"), "and does not go on to judge what it could not read");

    /* And with nothing declared about this build, it parses and declines to judge rather than
       judging against zeroes and calling a good bundle unacceptable. */
    iotdata_node_content_control_init(&store, NULL);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_ok, 3u);
    CHECK(said_has("app=relay"), "still parses");
    CHECK(said_has("not judged"), "and says why it will not say");
}

/* ------------------------------------------------------------------------------------------- */

static void test_control_argv_agrees(void) {
    printf("\nthe vocabulary builds the id the handler reads\n");
    iotdata_node_control_args_t args;
    char *argv[] = { (char *)"content", (char *)"delete", (char *)"1a2b3c" };
    const iotdata_node_control_command_t *const c = iotdata_node_control_from_argv(3, argv, &args, NULL);
    CHECK(c != NULL && strcmp(c->name, "content-delete") == 0, "`content delete 1a2b3c` resolves");
    CHECK(args.content == 0x1a2b3cu, "and a bare id is read as HEX, which is the only way §4's ids are legible");

    uint8_t buf[32];
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    CHECK(iotdata_node_control_build(&kv, c, &args), "and packs");
    size_t cur = 0;
    uint8_t key = 0, vlen = 0;
    const uint8_t *val = NULL;
    CHECK(iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen), "one entry");
    CHECK(key == IOTDATA_NODE_CONTROL_CONTROL, "a CONTROL");
    CHECK(vlen == 5u, "subject, action and three bytes of id");
    CHECK(val[0] == IOTDATA_NODE_TLV_CONTENT && val[1] == IOTDATA_NODE_ACTION_CONTENT_DELETE, "for the store");
    uint32_t id = 0u;
    CHECK(_iotdata_node_content_control_id(val + 2, (uint8_t)(vlen - 2u), &id), "the handler reads it");
    CHECK(id == 0x1a2b3cu, "as the same id");

    /* And what is not an id is refused before it can be mistaken for one. */
    char *bad[] = { (char *)"content", (char *)"delete", (char *)"1a2b3c4" };
    CHECK(iotdata_node_control_from_argv(3, bad, &args, NULL) == NULL, "seven hex digits is not a 24-bit id");
    char *none[] = { (char *)"content", (char *)"delete" };
    CHECK(iotdata_node_control_from_argv(2, none, &args, NULL) == NULL, "and an id is not optional");
    char *zz[] = { (char *)"content", (char *)"delete", (char *)"zz" };
    CHECK(iotdata_node_control_from_argv(3, zz, &args, NULL) == NULL, "nor is a word an id");
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_console_content: the store's words\n");
    test_base64();
    test_insert();
    test_dropped_line();
    test_corrupt_line();
    test_end_abandons();
    test_timeout_frees_the_console();
    test_window();
    test_sink_boundaries();
    test_through_poll();
    test_download();
    test_download_refusals();
    test_control_words();
    test_inspect_ota();
    test_control_argv_agrees();
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
