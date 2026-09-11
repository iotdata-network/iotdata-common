#ifndef D_FORMAT_H
#define D_FORMAT_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ------------------------------------------------------------------------------------------------------------------------

/* printf into a caller's buffer and return the buffer, so a formatted value can be used as an
   argument where only an expression fits -- a "%s" in a larger printf, or a struct field. The
   caller owns the buffer and therefore its lifetime, which is why this is not a static-buffer
   convenience: two of these in one printf would otherwise fight over it. */
__attribute__((format(printf, 3, 4))) static inline const char *snprintf_inline(char *const buf, const size_t size, const char *const fmt, ...) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buf, size, fmt, args);
    va_end(args);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _secs2str_put(char **const p, const char *const end, const char *s, int n) {
    while (n-- && *p < end)
        *(*p)++ = *s++;
}

/* A duration as the largest units that fit: 501 -> "8m21s", 90061 -> "1d1h1m1s", 0 -> "0". Zero
   components are skipped rather than padded, so it reads as a span and not as a clock.
   ALWAYS returns a valid C string and never NULL, because the result goes straight into a "%s"
   where a null pointer is undefined behaviour rather than a diagnosable error -- a buffer too
   small yields a truncated string instead. 24 bytes covers any int32 second count. */
static inline char *secs2str(const long sec, char *const out, const int len) {
    static const long div[] = { 86400, 3600, 60, 1 };
    static const char unit[] = "dhms";
    long left = sec < 0 ? -sec : sec;
    char *p = out;
    bool any = false;
    if (len < 2) {
        if (len > 0)
            *out = '\0';
        return out;
    }
    const char *const end = out + len - 1; /* the last byte is reserved for the terminator */
    if (sec < 0)
        _secs2str_put(&p, end, "-", 1);
    for (int i = 0; i < 4; i++) {
        long q = left / div[i];
        left -= q * div[i];
        if (q) {
            char b[24];
            int n = 0;
            do {
                b[n++] = '0' + (char)(q % 10);
                q /= 10;
            } while (q);
            while (n)
                _secs2str_put(&p, end, &b[--n], 1);
            _secs2str_put(&p, end, &unit[i], 1);
            any = true;
        }
    }
    if (!any) {
        out[0] = '0';
        p = out + 1;
    }
    *p = '\0'; /* p never passes `end`, so this is always in bounds */
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------

#define FORMAT_HEXDUMP_COLUMNS  16
#define FORMAT_HEXDUMP_LINE_MAX 80 /* "[0000] " + 16*3 + a mid gap + "|" + 16 + "|" + NUL = 75 */

/* One line of a hex dump, rendered into `out`:
 *
 *     [0000] 35 38 00 01 2C C1 68 97  3E 00 00 00 00 00 00 00 |58..,.h.>.......|
 *
 * Returns false when `offset` is at or past the end, so the caller's loop is
 *
 *     for (size_t off = 0; format_hexdump_line(line, sizeof(line), data, len, off); off += FORMAT_HEXDUMP_COLUMNS)
 *         <log it however this module logs>
 *
 * The PREFIX and the logging are deliberately not here: a prefix is caller-supplied and of
 * unbounded length, so keeping it out is what lets the line have a fixed, checkable bound. Built
 * by placing characters rather than by chaining snprintf, because `p += snprintf(...)` advances by
 * the length that WOULD have been written -- so on truncation `p` passes the end and `size - p`
 * underflows into a huge size_t. Placement cannot do that.
 */
static inline bool format_hexdump_line(char *const out, const size_t size, const uint8_t *const data, const size_t len, const size_t offset) {
    static const char hexdigit[] = "0123456789ABCDEF";
    if (out == NULL || size < FORMAT_HEXDUMP_LINE_MAX || data == NULL || offset >= len)
        return false; /* a diagnostic that does not fit is refused, not silently cut short */
    size_t n = 0;
    out[n++] = '[';
    for (int shift = 12; shift >= 0; shift -= 4) /* 4 digits: frames are far below 64K */
        out[n++] = hexdigit[(offset >> shift) & 0xFu];
    out[n++] = ']';
    out[n++] = ' ';
    for (size_t i = 0; i < FORMAT_HEXDUMP_COLUMNS; i++) {
        if (i == FORMAT_HEXDUMP_COLUMNS / 2)
            out[n++] = ' '; /* a gap at the halfway mark, to count columns by eye */
        if (offset + i < len) {
            const uint8_t b = data[offset + i];
            out[n++] = hexdigit[b >> 4];
            out[n++] = hexdigit[b & 0x0Fu];
        } else {
            out[n++] = ' ';
            out[n++] = ' ';
        }
        out[n++] = ' ';
    }
    /* delimited, so trailing spaces in a short last line are unambiguous */
    out[n++] = '|';
    for (size_t i = 0; i < FORMAT_HEXDUMP_COLUMNS; i++)
        out[n++] = (offset + i < len) ? (isprint(data[offset + i]) ? (char)data[offset + i] : '.') : ' ';
    out[n++] = '|';
    out[n] = '\0';
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Fixed-point rendering for logs: 2135 -> "21.35". Avoids pulling in float printf. */
#define CENTI_STR_MAX 16
static inline const char *centi_str(char *const buf, const size_t size, const int32_t centi) {
    const int32_t whole = centi < 0 ? -centi : centi; /* not `abs`: that shadows the libc function */
    return snprintf_inline(buf, size, "%s%" PRId32 ".%02" PRId32, centi < 0 ? "-" : "", whole / 100, whole % 100);
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_FORMAT_H */
