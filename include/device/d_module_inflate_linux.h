#ifndef D_MODULE_INFLATE_LINUX_H
#define D_MODULE_INFLATE_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_inflate_linux.h - RAW deflate, through zlib. The host and gateway half of ota_codec_t.
//
// RAW, with no zlib or gzip wrapper, which is what inflateInit2()'s NEGATIVE window bits select.
// ota-bundle writes the payload with deflateRawSync(level 9) for the reason stated there: a wrapper
// is bytes on a link measured in bytes per second, carrying a checksum the bundle already has. Hand
// zlib the default +15 instead of -15 and every stream fails on the first byte, so the sign here is
// load-bearing rather than decoration.
//
// WHY A HOST BACKEND AT ALL, when the node that applies updates is the esp32. Two reasons, and the
// second is the one that matters. A linux gateway will want to update itself eventually. But today
// this is what lets the host suite inflate a REAL deflate stream -- one produced by the same
// zlib.deflateRawSync the bundler uses -- instead of a hand-rolled stand-in that proves only that
// the seam is wired up. The interesting bugs in a streaming inflater are about partial input and
// output buffers that fill mid-symbol, and a stand-in has none of those.
//
// Link with -lz.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    ota_codec_t _ota_codec;
    z_stream z;
    bool open;
    uint64_t in_total, out_total;
} inflate_linux_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _inflate_linux_begin(void *const ctx) {
    inflate_linux_t *const s = (inflate_linux_t *)ctx;
    if (s == NULL)
        return false;
    if (s->open)
        (void)inflateEnd(&s->z);
    s->z = (z_stream){ 0 };
    s->open = false;
    s->in_total = s->out_total = 0u;
    /* -MAX_WBITS: raw deflate. See the note above -- the sign is the whole contract. */
    if (inflateInit2(&s->z, -MAX_WBITS) != Z_OK)
        return false;
    s->open = true;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _inflate_linux_step(void *const ctx, const void *const in, const size_t inlen, size_t *const consumed, void *const out, const size_t outlen) {
    inflate_linux_t *const s = (inflate_linux_t *)ctx;
    if (consumed != NULL)
        *consumed = 0u;
    if (s == NULL || !s->open || out == NULL || outlen == 0u)
        return -1;
    s->z.next_in = (Bytef *)(uintptr_t)in;
    s->z.avail_in = (uInt)inlen;
    s->z.next_out = (Bytef *)out;
    s->z.avail_out = (uInt)outlen;
    const int rc = inflate(&s->z, Z_NO_FLUSH);
    /* Z_BUF_ERROR is not an error here: it is zlib saying it could do nothing with what it was
       given, which for a caller that feeds in bounded steps is an ordinary "come back with more". */
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
        return -1;
    const size_t used = inlen - (size_t)s->z.avail_in;
    const size_t made = outlen - (size_t)s->z.avail_out;
    if (consumed != NULL)
        *consumed = used;
    s->in_total += used;
    s->out_total += made;
    return (int)made;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The stream ENDED, rather than merely stopped. A truncated deflate stream consumes every byte it
   is given and asks for more, so this is the only thing that can tell the difference. */
static inline bool _inflate_linux_end(void *const ctx) {
    inflate_linux_t *const s = (inflate_linux_t *)ctx;
    if (s == NULL || !s->open)
        return false;
    /* one more call with no input: a stream whose last symbol was complete reports STREAM_END here */
    uint8_t scratch[1];
    s->z.next_in = NULL;
    s->z.avail_in = 0u;
    s->z.next_out = scratch;
    s->z.avail_out = (uInt)sizeof(scratch);
    const int rc = inflate(&s->z, Z_FINISH);
    const bool done = (rc == Z_STREAM_END);
    (void)inflateEnd(&s->z);
    s->open = false;
    return done;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _inflate_linux_abort(void *const ctx) {
    inflate_linux_t *const s = (inflate_linux_t *)ctx;
    if (s == NULL || !s->open)
        return;
    (void)inflateEnd(&s->z);
    s->open = false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const ota_codec_t *inflate_linux_init(inflate_linux_t *const s) {
    if (s == NULL)
        return NULL;
    *s = (inflate_linux_t){ 0 };
    s->_ota_codec = (const ota_codec_t){
        .ctx = s,
        .begin = _inflate_linux_begin,
        .step = _inflate_linux_step,
        .end = _inflate_linux_end,
        .abort = _inflate_linux_abort,
    };
    return &s->_ota_codec;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef inflate_linux_t ota_codec_impl_t;
#define ota_codec_impl_init inflate_linux_init

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_INFLATE_LINUX_H */
