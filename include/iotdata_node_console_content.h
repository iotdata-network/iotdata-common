#ifndef IOTDATA_NODE_CONSOLE_CONTENT_H
#define IOTDATA_NODE_CONSOLE_CONTENT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_console_content.h - the store's console words: `content insert` and `content
// download`, and the framing both use.
//
// THE WORDS HERE HAVE NO WIRE EQUIVALENT, which is the basis for the split from
// iotdata_node_content_control.h -- list, delete, inspect and validate are CONTROL vocabulary and
// reach their handler by the same route an MQTT request does. These two are a byte STREAM, and the
// wire's answer to a byte stream is the CONTENT TLVs, not a command with a very large argument.
// They are a cable's words, and a cable is what they will stay.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE FRAMING, AND WHY IT HAS NO ACKS
//
//     -> content insert 0537a1 197455 0x5b5eb8c2e54aa1c4
//     <- content: insert 0537a1 ready, 197455 bytes, lines of up to 192, window 1344
//     -> d 0 SU9UQQ...
//     -> d 192 ...
//        ... up to `window` bytes ahead of the last ack, then wait for one
//     <- content: insert 0537a1 ack 1344
//        ...
//     <- content: insert 0537a1 ack 197455
//     <- content: insert 0537a1 arrived, 197455 bytes          (from the tick, after the checksum)
//
// ONE OFFSET PER LINE IS THE ERROR PROTOCOL, and the whole of it. The receiver refuses any line
// that is not exactly where it expects, so a dropped or reordered line is caught at once rather
// than becoming a file of the right length and the wrong content, and a flipped bit inside a line
// is caught by the CRC-64 at the end. No line carries a checksum of its own and nothing is
// retransmitted.
//
// THE ACK IS NOT PART OF THAT. It is a CREDIT, and it is here because the transport turned out to
// drop rather than block: see THE RECEIVE RING in iotdata_node_console.h. A host that writes a
// 200 KB file as fast as the USB will take it overruns a device that is writing each line to flash
// as it arrives, and the bytes that do not fit the ring are simply gone -- measured, not supposed:
// ten lines written back to back landed as four, with logging off and a ring four times the size of
// one line. No amount of error DETECTION fixes that, because there is no error to detect at the far
// end; the host has to be told when to continue.
//
// So the device says how far ahead the host may run, and says so again every `window` bytes. The
// window is several lines wide, so the cost is one round trip per kilobyte and a half rather than
// per line, and the transfer ends up limited by how fast the device can write flash -- which is
// where a transfer onto flash ought to be limited.
//
// THE ACK COUNTS WHAT WAS OFFERED, NOT WHAT WAS ACCEPTED, which matters once something has gone
// wrong: a refused insert still has to be fed to the declared end, and the credit has to keep
// flowing or the host would stall against a window that never opens.
//
// AND THERE IS NO RESYNC, deliberately. Once a line is refused the insert is doomed, and the
// receiver's refusal STICKS: it keeps consuming to the declared end and discards. That is not
// politeness, it is the only safe option -- the alternative is reading the remaining 190 KB of
// base64 as console commands. The host retries the whole file, which costs seconds on a cable.
//
// `end` ABANDONS IT, which is how a host that has lost its nerve gets the console back without
// waiting out the insert timeout. Nothing else terminates a transfer early: a line that is not
// data and not `end` is counted and ignored, because treating a corrupted line as a terminator
// would hand a flipped bit the power to end a transfer.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// A DOWNLOAD IS A JOB AND AN INSERT IS NOT
//
// The insert is driven by its own arrival: bytes come in and the sink writes them, as fast as the
// credit window lets the host send. Nothing has to be scheduled, and the brake is the window.
//
// The download's brake is the other direction's: a write into the console's transmit buffer blocks
// once that is full, so a host that stops reading stops the transfer. That is enough to make it
// safe but not enough to make it polite, so it is ticked -- a bounded number of lines per pass of
// the main loop, with the radio serviced between them. The bound is small on purpose: a loop pass
// is cheap and frequent, so a small slice per pass still saturates the cable while leaving the node
// a relay rather than a file server having a go at being a relay.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHY THE SINK IS HANDED now_ms, WHEN NOTHING ELSE IN A CONSOLE IS
//
// A verb does not need a clock: it is a line, it is answered, and it is over. A sink is not --
// it is a transfer that has a timeout, and the timeout is the module's, measured in the same
// now_ms the rest of the pass is working to. So now_ms comes down the console's own tick, through
// the sink signature, to here. It costs a parameter and it is the whole of the mechanism.
//
// IT MUST BE THE PASS'S CLOCK AND NOT A FRESH READING, which is why this is threaded rather than
// simply read where it is wanted. A main loop reads the clock once at the top of a pass and hands
// that value to everything in it. The console's poll runs BEFORE the content tick, so a sink that
// read the hardware itself would stamp an insert a millisecond or two AHEAD of the now_ms the tick
// is about to be given. The module's timeout is an unsigned `now_ms - last_ms`, and two
// milliseconds of the wrong sign is 4.29 billion the right one: the insert times out on the first
// tick after it starts.
//
// That is not hypothetical -- it is what happened the first time this ran on hardware, and the
// symptom was a `ready` line and a `timed out` line with the same timestamp on them. Threading the
// value makes the mistake unavailable rather than merely documented: there is no clock here to
// read.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <inttypes.h>
#include <stdlib.h>

#include "iotdata_node_console.h"
#include "iotdata_node_content.h"

/* Six §4 chunks. 192 bytes is exactly 256 base64 characters, so a line needs no padding and moves
   200 KB in about a thousand lines.
 *
 * THE CEILING IS THE CONSOLE'S RECEIVE RING AND NOT THE WIRE. The host writes a whole line in one
 * call, and nothing on the way in asks it to wait -- see THE RECEIVE RING in
 * iotdata_node_console.h for what happens when a line does not fit. The assertion below is what
 * makes that a build failure rather than a line that comes back a few bytes short. */
#ifndef IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD
#define IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD 192u
#endif

/* Lines per pass of the main loop while a download runs. One is not a typo: the loop spins, and a
   line that the USB peripheral is not ready for blocks until it is -- so the limit that matters is
   the cable's, and this only has to stop a single pass from monopolising the node. */
#ifndef IOTDATA_NODE_CONSOLE_CONTENT_TICK_LINES
#define IOTDATA_NODE_CONSOLE_CONTENT_TICK_LINES 1u
#endif

#define IOTDATA_NODE_CONSOLE_CONTENT_B64  (((IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD + 2u) / 3u) * 4u)
#define IOTDATA_NODE_CONSOLE_CONTENT_LINE (IOTDATA_NODE_CONSOLE_CONTENT_B64 + 32u) /* + "d <offset> " and a NUL */

/* HALF the ring, not all of it: the device may already have unread bytes queued when a line
   arrives, and a transport that drops its overflow gives no second chance to find out. */
_Static_assert(2u * IOTDATA_NODE_CONSOLE_CONTENT_LINE <= (unsigned)IOTDATA_NODE_CONSOLE_RX_BYTES, "a data line must fit in half the console's receive ring -- raise IOTDATA_NODE_CONSOLE_RX_BYTES or lower the payload");

/* HOW FAR AHEAD THE HOST MAY RUN, derived from the ring rather than chosen: whole lines, as many as
   fit, less one so that the line being assembled when the window closes has somewhere to land.
   Deriving it is the point -- a window and a ring that were tuned independently would eventually
   be tuned apart, and the failure that follows is lost bytes rather than a build error. */
#define IOTDATA_NODE_CONSOLE_CONTENT_WINDOW_LINES ((unsigned)IOTDATA_NODE_CONSOLE_RX_BYTES / IOTDATA_NODE_CONSOLE_CONTENT_LINE - 1u)
#define IOTDATA_NODE_CONSOLE_CONTENT_WINDOW       (IOTDATA_NODE_CONSOLE_CONTENT_WINDOW_LINES * IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD)
_Static_assert(IOTDATA_NODE_CONSOLE_CONTENT_WINDOW_LINES >= 1u, "the window must be at least one line wide, or nothing can be sent at all");

// -----------------------------------------------------------------------------------------------------------------------------------------
// BASE64
//
// Its own, in this file, because this is the only thing in the tree that frames anything this way:
// the wire carries binary TLVs and has no use for it. The library has a pair of these too, static
// inside iotdata.c behind feature macros and in the bare iotdata_* namespace -- not reachable from
// here, and not worth opening up for one caller.
//
// DECODE IS STRICT. A character outside the alphabet, a length that is not a multiple of four, a
// pad in the middle: all refused rather than skipped. A decoder that quietly ignores what it does
// not understand turns a corrupted line into a short one, and a short line is exactly the thing
// the offset check was put there to catch.
// -----------------------------------------------------------------------------------------------------------------------------------------

static const char _iotdata_node_console_content_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline int _iotdata_node_console_content_b64_val(const char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static inline size_t _iotdata_node_console_content_encode(const uint8_t *const in, const size_t len, char *const out) {
    size_t i = 0, j = 0;
    while (i < len) {
        const uint32_t a = in[i++];
        const bool have_b = i < len;
        const uint32_t b = have_b ? in[i++] : 0u;
        const bool have_c = i < len;
        const uint32_t c = have_c ? in[i++] : 0u;
        const uint32_t x = (a << 16) | (b << 8) | c;
        out[j++] = _iotdata_node_console_content_b64[(x >> 18) & 0x3Fu];
        out[j++] = _iotdata_node_console_content_b64[(x >> 12) & 0x3Fu];
        out[j++] = have_b ? _iotdata_node_console_content_b64[(x >> 6) & 0x3Fu] : '=';
        out[j++] = have_c ? _iotdata_node_console_content_b64[x & 0x3Fu] : '=';
    }
    out[j] = '\0';
    return j;
}

/* Negative on anything malformed, so a caller cannot mistake a refusal for an empty line. */
static inline int _iotdata_node_console_content_decode(const char *const in, uint8_t *const out, const size_t max) {
    const size_t len = strlen(in);
    if (len == 0u || (len % 4u) != 0u)
        return -1;
    size_t op = 0;
    for (size_t i = 0; i < len; i += 4u) {
        const bool pad_c = in[i + 2u] == '=', pad_d = in[i + 3u] == '=';
        if (pad_c && !pad_d)
            return -1; /* "xx=y" is not a thing */
        if ((pad_c || pad_d) && i + 4u != len)
            return -1; /* padding anywhere but the end */
        const int a = _iotdata_node_console_content_b64_val(in[i]), b = _iotdata_node_console_content_b64_val(in[i + 1u]);
        const int c = pad_c ? 0 : _iotdata_node_console_content_b64_val(in[i + 2u]), d = pad_d ? 0 : _iotdata_node_console_content_b64_val(in[i + 3u]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return -1;
        const size_t want = pad_c ? 1u : (pad_d ? 2u : 3u);
        if (op + want > max)
            return -1;
        out[op++] = (uint8_t)((a << 2) | (b >> 4));
        if (want > 1u)
            out[op++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
        if (want > 2u)
            out[op++] = (uint8_t)(((c & 0x03) << 6) | d);
    }
    return (int)op;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    iotdata_node_content_t *store;

    /* the upload, as the SINK sees it -- which is not as the module sees it */
    bool up;
    uint32_t up_id, up_size, up_seen; /* up_seen is a high-water of what was OFFERED, accepted or not */
    uint32_t up_acked;                /* ... and how much of that the host has been told about */
    uint32_t up_lines, up_bad;
    char line[IOTDATA_NODE_CONSOLE_CONTENT_LINE];
    size_t line_len;
    bool line_over;

    /* the download, as a job */
    bool down;
    uint32_t down_id, down_off, down_size;
    uint64_t down_crc;
} iotdata_node_console_content_t;

static iotdata_node_console_content_t _iotdata_node_console_content;

static inline void iotdata_node_console_content_init(iotdata_node_content_t *const store) {
    _iotdata_node_console_content = (iotdata_node_console_content_t){ 0 };
    _iotdata_node_console_content.store = store;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE SINK
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Done taking bytes. The INSERT is left exactly as it is: if every line arrived the module's tick
   will check the sum and rename, and if one did not the module's tick will destroy the scratch. The
   sink's job was to carry bytes, and it does not get a vote on the outcome. */
static inline void _iotdata_node_console_content_up_end(iotdata_node_console_content_t *const s) {
    s->up = false;
    s->line_len = 0u;
    s->line_over = false;
}

static inline void _iotdata_node_console_content_up_line(iotdata_node_console_content_t *const s, const uint32_t now_ms, const char *const line) {
    if (s->line_over) {
        s->up_bad++; /* longer than we said we would take: whatever it is, it is not our line */
        return;
    }
    if (strcmp(line, "end") == 0) {
        iotdata_node_content_insert_abort(s->store, IOTDATA_NODE_CONTENT_ERR_STATE);
        iotdata_node_console_reply("content: insert %06" PRIx32 " abandoned by the sender at %" PRIu32 " of %" PRIu32 "\n", s->up_id, s->up_seen, s->up_size);
        _iotdata_node_console_content_up_end(s);
        return;
    }
    if (line[0] != 'd' || line[1] != ' ') {
        s->up_bad++;
        return;
    }
    char *end = NULL;
    const unsigned long off = strtoul(line + 2, &end, 10);
    if (end == line + 2 || end == NULL || *end != ' ') {
        s->up_bad++;
        return;
    }
    uint8_t raw[IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD];
    const int n = _iotdata_node_console_content_decode(end + 1, raw, sizeof(raw));
    if (n < 0) {
        s->up_bad++;
        /* The module must be told, because a line nobody could decode is a hole: left alone, the
           next line would arrive at the right offset for a file with a gap in it. */
        iotdata_node_content_insert_abort(s->store, IOTDATA_NODE_CONTENT_ERR_CHECKSUM);
        return;
    }
    s->up_lines++;
    /* The high-water counts what was OFFERED, not what was taken, so a refused insert still knows
       when the sender has finished sending and the console can come back. */
    if ((uint32_t)off + (uint32_t)n > s->up_seen)
        s->up_seen = (uint32_t)off + (uint32_t)n;
    (void)iotdata_node_content_insert_write(s->store, (uint32_t)off, raw, (size_t)n, now_ms);
    /* The credit, once a window's worth has gone by and again at the end -- the end unconditionally,
       because a transfer whose last window is a short one would otherwise never be acknowledged and
       a host waiting on credit would wait for the insert timeout instead. */
    if (s->up_seen >= s->up_size || s->up_seen - s->up_acked >= (uint32_t)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW) {
        s->up_acked = s->up_seen;
        iotdata_node_console_reply("content: insert %06" PRIx32 " ack %" PRIu32 "\n", s->up_id, s->up_acked);
    }
    if (s->up_seen >= s->up_size)
        _iotdata_node_console_content_up_end(s);
}

static inline size_t _iotdata_node_console_content_sink(void *const ctx, const uint32_t now_ms, const uint8_t *const buf, const size_t len) {
    iotdata_node_console_content_t *const s = (iotdata_node_console_content_t *)ctx;
    if (!s->up)
        return 0u; /* finished on the last call, exactly at a chunk boundary: take none, stand down */
    size_t i = 0;
    while (i < len && s->up) {
        const char c = (char)buf[i++];
        if (c == '\r')
            continue;
        if (c == '\n') {
            s->line[s->line_len] = '\0';
            const size_t was = s->line_len;
            s->line_len = 0u;
            if (was > 0u || s->line_over)
                _iotdata_node_console_content_up_line(s, now_ms, s->line);
            s->line_over = false;
            continue;
        }
        if (s->line_len < sizeof(s->line) - 1u)
            s->line[s->line_len++] = c;
        else
            s->line_over = true;
    }
    return i;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE WORDS
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_console_content_insert(const uint32_t now_ms, const int argc, char **const argv) {
    iotdata_node_console_content_t *const s = &_iotdata_node_console_content;
    if (argc < 5) {
        iotdata_node_console_reply("content insert <id> <bytes> <crc64>\n");
        return;
    }
    if (s->up || s->down || iotdata_node_console_sink_active()) {
        iotdata_node_console_reply("content: insert refused, the console is already busy\n");
        return;
    }
    const uint32_t id = (uint32_t)(strtoul(argv[2], NULL, 16) & 0xFFFFFFuL);
    const uint32_t size = (uint32_t)strtoul(argv[3], NULL, 0);
    const uint64_t crc = (uint64_t)strtoull(argv[4], NULL, 16);
    const iotdata_node_content_status_t st = iotdata_node_content_insert_begin(s->store, id, size, crc, now_ms);
    if (st != IOTDATA_NODE_CONTENT_OK) {
        iotdata_node_console_reply("content: insert %06" PRIx32 " refused, %s\n", id, iotdata_node_content_status_name(st));
        return;
    }
    s->up = true;
    s->up_id = id;
    s->up_size = size;
    s->up_seen = s->up_acked = s->up_lines = s->up_bad = 0u;
    s->line_len = 0u;
    s->line_over = false;
    /* The device says what it will accept rather than the host assuming: the payload size is a
       build option here, and a host that guessed it would break the day it changed. */
    iotdata_node_console_reply("content: insert %06" PRIx32 " ready, %" PRIu32 " bytes, lines of up to %u, window %u (`d <offset> <base64>`, `end` to abandon)\n", id, size, (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD,
                               (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW);
    iotdata_node_console_sink_set(_iotdata_node_console_content_sink, s);
}

static inline void _iotdata_node_console_content_download(__attribute__((unused)) const uint32_t now_ms, const int argc, char **const argv) {
    iotdata_node_console_content_t *const s = &_iotdata_node_console_content;
    if (argc < 3) {
        iotdata_node_console_reply("content download <id>\n");
        return;
    }
    if (s->up || s->down || iotdata_node_console_sink_active()) {
        iotdata_node_console_reply("content: download refused, the console is already busy\n");
        return;
    }
    const uint32_t id = (uint32_t)(strtoul(argv[2], NULL, 16) & 0xFFFFFFuL);
    uint32_t total = 0u, held = 0u;
    if (!iotdata_node_content_stat(s->store, id, &total, &held)) {
        iotdata_node_console_reply("content: download %06" PRIx32 " refused, %s\n", id, iotdata_node_content_status_name(IOTDATA_NODE_CONTENT_ERR_ABSENT));
        return;
    }
    s->down = true;
    s->down_id = id;
    s->down_off = 0u;
    s->down_size = held;
    s->down_crc = IOTDATA_NODE_CRC64_INIT;
    /* The length first so the far end can size its buffer; the sum LAST, because it is computed
       from what was actually read rather than from a second pass over the file. */
    iotdata_node_console_reply("content: download %06" PRIx32 " begin, %" PRIu32 " bytes%s\n", id, held, (held == total) ? "" : " (INCOMPLETE)");
}

static inline void _iotdata_node_console_content_status(__attribute__((unused)) const uint32_t now_ms) {
    const iotdata_node_console_content_t *const s = &_iotdata_node_console_content;
    if (s->store == NULL) {
        iotdata_node_console_reply("content: no store\n");
        return;
    }
    uint32_t id = 0u, have = 0u, want = 0u;
    if (iotdata_node_content_insert_active(s->store)) {
        iotdata_node_content_insert_progress(s->store, &id, &have, &want);
        iotdata_node_console_reply("content: insert %06" PRIx32 " in flight, %" PRIu32 " of %" PRIu32 " bytes, %" PRIu32 " lines, %" PRIu32 " refused\n", id, have, want, s->up_lines, s->up_bad);
    } else if (s->down) {
        iotdata_node_console_reply("content: download %06" PRIx32 " running, %" PRIu32 " of %" PRIu32 " bytes\n", s->down_id, s->down_off, s->down_size);
    } else {
        iotdata_node_console_reply("content: %" PRIu32 " files, %" PRIu32 " bytes free, idle\n", iotdata_node_content_count(s->store), iotdata_node_content_space(s->store));
    }
}

/* Only the words that have no wire equivalent. Everything else is answered false and falls through
   to the CONTROL vocabulary, which is the one place `content list` and friends are resolved. */
static inline bool iotdata_node_console_content(const uint32_t now_ms, const int argc, char **const argv) {
    if (_iotdata_node_console_content.store == NULL)
        return false;
    if (argc < 2) {
        _iotdata_node_console_content_status(now_ms);
        return true;
    }
    if (strcmp(argv[1], "insert") == 0) {
        _iotdata_node_console_content_insert(now_ms, argc, argv);
        return true;
    }
    if (strcmp(argv[1], "download") == 0) {
        _iotdata_node_console_content_download(now_ms, argc, argv);
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE TICK
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_console_content_tick_down(iotdata_node_console_content_t *const s) {
    uint8_t raw[IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD];
    char b64[IOTDATA_NODE_CONSOLE_CONTENT_B64 + 1];
    for (unsigned n = 0; n < (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_TICK_LINES && s->down; n++) {
        if (s->down_off >= s->down_size) {
            iotdata_node_console_reply("content: download %06" PRIx32 " done, %" PRIu32 " bytes, crc64=0x%016" PRIX64 "\n", s->down_id, s->down_size, s->down_crc);
            s->down = false;
            return;
        }
        const uint32_t want = ((s->down_size - s->down_off) < (uint32_t)sizeof(raw)) ? (s->down_size - s->down_off) : (uint32_t)sizeof(raw);
        if (iotdata_node_content_read(s->store, s->down_id, s->down_off, raw, want) != (int)want) {
            iotdata_node_console_reply("content: download %06" PRIx32 " FAILED at %" PRIu32 ", %s\n", s->down_id, s->down_off, iotdata_node_content_status_name(IOTDATA_NODE_CONTENT_ERR_STORE));
            s->down = false;
            return;
        }
        (void)_iotdata_node_console_content_encode(raw, want, b64);
        iotdata_node_console_reply("d %" PRIu32 " %s\n", s->down_off, b64);
        s->down_crc = iotdata_node_crc64(raw, want, s->down_crc);
        s->down_off += want;
    }
}

/* Called every pass of the main loop, beside iotdata_node_content_tick(). Two jobs: give the
   download its slice, and make sure an abandoned upload cannot hold the console shut -- which it
   would, because the console itself has no notion of a transfer and cannot time one out. The
   module's insert timeout is the authority, and this just follows it.
 *
 * IT TAKES now_ms AND DOES NOT READ IT, which is deliberate twice over. Every deadline here is
 * the module's, asked for rather than kept, so there is nothing to compare a clock against; and a
 * tick in this tree takes the pass's clock whether it needs it this week or not, because the
 * alternative is a signature that changes the first time one of them grows a deadline of its own
 * -- and a caller that then has no clock to hand it. */
static inline void iotdata_node_console_content_tick(__attribute__((unused)) const uint32_t now_ms) {
    iotdata_node_console_content_t *const s = &_iotdata_node_console_content;
    if (s->store == NULL)
        return;
    if (s->up && !iotdata_node_content_insert_active(s->store)) {
        iotdata_node_console_reply("content: insert %06" PRIx32 " gave up at %" PRIu32 " of %" PRIu32 ", %s\n", s->up_id, s->up_seen, s->up_size, iotdata_node_content_status_name(iotdata_node_content_insert_why(s->store)));
        _iotdata_node_console_content_up_end(s);
    }
    if (!s->up && iotdata_node_console_sink_active())
        iotdata_node_console_sink_clear(); /* the sink outlived its transfer: the console comes back */
    if (s->down)
        _iotdata_node_console_content_tick_down(s);
}

/* What the store's tick said, in words, for an app that would otherwise write this out itself. */
static inline void iotdata_node_console_content_event(const iotdata_node_content_event_t ev) {
    const iotdata_node_console_content_t *const s = &_iotdata_node_console_content;
    switch (ev) {
    case IOTDATA_NODE_CONTENT_EVENT_ARRIVED:
        iotdata_node_console_reply("content: insert %06" PRIx32 " arrived, %" PRIu32 " bytes, %" PRIu32 " lines, %" PRIu32 " refused\n", s->up_id, s->up_size, s->up_lines, s->up_bad);
        break;
    case IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED:
        iotdata_node_console_reply("content: insert %06" PRIx32 " failed, %s (%" PRIu32 " lines, %" PRIu32 " refused)\n", s->up_id, iotdata_node_content_status_name(iotdata_node_content_insert_why(s->store)), s->up_lines, s->up_bad);
        break;
    case IOTDATA_NODE_CONTENT_EVENT_WORK_DONE:
    case IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED:
    case IOTDATA_NODE_CONTENT_EVENT_NONE:
    default:
        break;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_CONTENT_H */
