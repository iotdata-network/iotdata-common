#ifndef D_MODULE_INFLATE_ESP32_H
#define D_MODULE_INFLATE_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_inflate_esp32.h - RAW deflate, through the tinfl that is already in ROM. ota_codec_t's
// esp32 half.
//
// IT COSTS NO FLASH. tinfl_decompress lives in the esp32c3's mask ROM (0x400000f4 in
// esp32c3.rom.ld), so the inflater itself is free and all this header adds is the bookkeeping around
// it. That is why CODEC_DEFLATE is the bundler's default: the saving on a link measured in
// kilobytes per second is real and the cost on the device is RAM, not image size.
//
// RAW, which is the absence of TINFL_FLAG_PARSE_ZLIB_HEADER. ota-bundle writes the payload with
// deflateRawSync -- no wrapper, because a wrapper is bytes on the wire for a checksum the bundle
// already carries. Set that flag and every bundle fails on its first byte.
//
// WHY THERE IS A 32KB DICTIONARY IN HERE, and why it is malloc'd rather than static. tinfl resolves
// a match by reaching backwards into what it has already produced, so unless the whole output is in
// memory at once -- which for a 384KB image it is not -- the output buffer must BE the sliding
// window: TINFL_LZ_DICT_SIZE bytes, written round-robin, with TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
// left clear. The caller's buffer cannot serve, because the caller's buffer is whatever size the
// apply loop chose and has no history in it. So the window lives here, is copied out of as it fills,
// and exists only between begin() and end()/abort() -- about 43KB with the decompressor's tables,
// which is a lot to hold permanently on a node that updates itself twice a year.
//
// esp32 component requirements: esp_rom (for miniz.h and the ROM symbols).
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_inflate = "device-inflate";

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    ota_codec_t _ota_codec;
    tinfl_decompressor *d;
    uint8_t *dict;   /* TINFL_LZ_DICT_SIZE, the sliding window AND tinfl's output buffer */
    size_t dict_ofs; /* where the next byte lands in it */
    size_t pending;  /* produced into the window but not yet handed to the caller */
    size_t pending_at;
    bool open, done;
    uint64_t in_total, out_total;
} inflate_esp32_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline void _inflate_esp32_release(inflate_esp32_t *const s) {
    if (s->d) {
        free(s->d);
        s->d = NULL;
    }
    if (s->dict) {
        free(s->dict);
        s->dict = NULL;
    }
    s->open = false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _inflate_esp32_begin(void *const ctx) {
    inflate_esp32_t *const s = (inflate_esp32_t *)ctx;
    if (s == NULL)
        return false;
    _inflate_esp32_release(s);
    if ((s->d = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor))) == NULL) {
        ESP_LOGE(__tag_device_inflate, "begin: %u bytes refused", (unsigned)sizeof(tinfl_decompressor));
        return false;
    }
    if ((s->dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE)) == NULL) {
        ESP_LOGE(__tag_device_inflate, "begin: %u bytes refused", (unsigned)TINFL_LZ_DICT_SIZE);
        free(s->d);
        s->d = NULL;
        return false;
    }
    tinfl_init(s->d);
    s->dict_ofs = s->pending = s->pending_at = 0u;
    s->in_total = s->out_total = 0u;
    s->open = true;
    s->done = false;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* ONE STEP. Hand back whatever is already sitting in the window first, then ask tinfl for more --
   which keeps the contract simple: the caller never has to know that the window and its buffer are
   different sizes, and a call that produces output without consuming input is ordinary rather than
   a sign of trouble. */
static inline int _inflate_esp32_step(void *const ctx, const void *const in, const size_t inlen, size_t *const consumed, void *const out, const size_t outlen) {
    inflate_esp32_t *const s = (inflate_esp32_t *)ctx;
    if (consumed != NULL)
        *consumed = 0u;
    if (s == NULL || !s->open || out == NULL || outlen == 0u)
        return -1;

    /* the backlog, if the last call made more than the caller could take */
    if (s->pending > 0u) {
        const size_t give = (s->pending < outlen) ? s->pending : outlen;
        memcpy(out, &s->dict[s->pending_at], give);
        s->pending_at += give;
        s->pending -= give;
        return (int)give;
    }
    if (s->done)
        return 0; /* nothing left, and nothing consumed: the caller's end() decides if that is right */

    size_t in_bytes = inlen;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - s->dict_ofs; /* to the end of the window, never past it */
    const mz_uint32 flags = (inlen > 0u) ? (mz_uint32)TINFL_FLAG_HAS_MORE_INPUT : 0u;
    const tinfl_status st = tinfl_decompress(s->d, (const mz_uint8 *)in, &in_bytes, s->dict, &s->dict[s->dict_ofs], &out_bytes, flags);
    if (st < TINFL_STATUS_DONE) {
        ESP_LOGE(__tag_device_inflate, "step: tinfl says %d after %llu in / %llu out", (int)st, (unsigned long long)s->in_total, (unsigned long long)s->out_total);
        return -1;
    }
    if (consumed != NULL)
        *consumed = in_bytes;
    s->in_total += in_bytes;
    s->out_total += out_bytes;
    if (st == TINFL_STATUS_DONE)
        s->done = true;

    s->pending_at = s->dict_ofs;
    s->pending = out_bytes;
    s->dict_ofs = (s->dict_ofs + out_bytes) & (size_t)(TINFL_LZ_DICT_SIZE - 1u);

    const size_t give = (s->pending < outlen) ? s->pending : outlen;
    if (give > 0u) {
        memcpy(out, &s->dict[s->pending_at], give);
        s->pending_at += give;
        s->pending -= give;
    }
    return (int)give;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The stream ENDED, rather than ran out. tinfl says DONE only when it has read a final block, so a
   truncated payload is caught here and not as a hash mismatch three steps later. */
static inline bool _inflate_esp32_end(void *const ctx) {
    inflate_esp32_t *const s = (inflate_esp32_t *)ctx;
    if (s == NULL || !s->open)
        return false;
    const bool ok = s->done && s->pending == 0u;
    if (!ok)
        ESP_LOGW(__tag_device_inflate, "end: stream %s after %llu in / %llu out", s->done ? "has output left" : "did not finish", (unsigned long long)s->in_total, (unsigned long long)s->out_total);
    _inflate_esp32_release(s);
    return ok;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _inflate_esp32_abort(void *const ctx) {
    inflate_esp32_t *const s = (inflate_esp32_t *)ctx;
    if (s != NULL)
        _inflate_esp32_release(s);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const ota_codec_t *inflate_esp32_init(inflate_esp32_t *const s) {
    if (s == NULL)
        return NULL;
    *s = (inflate_esp32_t){ 0 };
    s->_ota_codec = (const ota_codec_t){
        .ctx = s,
        .begin = _inflate_esp32_begin,
        .step = _inflate_esp32_step,
        .end = _inflate_esp32_end,
        .abort = _inflate_esp32_abort,
    };
    return &s->_ota_codec;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* the neutral name; see the note in d_module_otaslot_esp32.h */
typedef inflate_esp32_t ota_codec_impl_t;
#define ota_codec_impl_init inflate_esp32_init

#endif /* D_MODULE_INFLATE_ESP32_H */
