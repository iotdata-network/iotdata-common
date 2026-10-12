#ifndef D_MODULE_HPATCH_H
#define D_MODULE_HPATCH_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_hpatch.h - HPatchLite behind ota_patch_t. Shared by every platform, because the engine is
// four files of portable C and the only thing that differs is where the bytes come from.
//
// THE PATCH IS UNCOMPRESSED, AND THAT IS DELIBERATE. hdiffi can compress its own output (tinyuz,
// zlib, lzma), and we tell it not to: FORM and CODEC are independent in our format, so the patch is
// the difference and the codec is what squeezes it. That is not a preference -- measured on
// relay 0.9.8 -> 0.9.9, hdiffi uncompressed through our deflate is 6616 bytes on the wire against
// 7003 for the hdiffz patches that used to be in the release tree, and the deflate comes free from
// the C3's ROM. A patch that arrived compressed is refused here rather than decoded, because the
// decompressor it would need is the one the codec seam already is.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hpatch_lite.h"

// ------------------------------------------------------------------------------------------------------------------------

/* The engine's scratch. "one decompress memory size + input cache size(>=3Byte)" upstream, and with
   no decompressor that is the input cache alone -- so this is purely how often read_patch is called,
   not a correctness floor. 1KB matches the chunk the rest of the apply path moves. */
#ifndef HPATCH_CACHE_BYTES
#define HPATCH_CACHE_BYTES 1024u
#endif

typedef struct {
    ota_patch_t _ota_patch;
    ota_patch_io_t *io;
    uint32_t want;
    uint32_t written;
    bool failed;
    uint8_t cache[HPATCH_CACHE_BYTES];
} hpatch_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    hpatchi_listener_t base; /* FIRST: the engine is handed &base and we cast back */
    hpatch_t *self;
} _patch_listener_t;

static hpi_BOOL _patch_read_diff(hpi_TInputStreamHandle h, hpi_byte *out, hpi_size_t *len) {
    hpatch_t *const s = (hpatch_t *)h;
    size_t n = (size_t)*len;
    if (s == NULL || s->io == NULL || s->io->read_patch == NULL || !s->io->read_patch(s->io, out, &n)) {
        if (s != NULL)
            s->failed = true;
        return hpi_FALSE;
    }
    *len = (hpi_size_t)n; /* short is END OF STREAM here, never "not yet" -- see the seam's note */
    return hpi_TRUE;
}

static hpi_BOOL _patch_read_old(hpatchi_listener_t *l, hpi_pos_t at, hpi_byte *out, hpi_size_t len) {
    hpatch_t *const s = ((_patch_listener_t *)l)->self;
    if (s->io->read_base == NULL || !s->io->read_base(s->io, (uint32_t)at, out, (size_t)len)) {
        s->failed = true;
        return hpi_FALSE;
    }
    return hpi_TRUE;
}

static hpi_BOOL _patch_write_new(hpatchi_listener_t *l, const hpi_byte *data, hpi_size_t len) {
    hpatch_t *const s = ((_patch_listener_t *)l)->self;
    if ((uint64_t)s->written + (uint64_t)len > (uint64_t)s->want || !s->io->write_new(s->io, data, (size_t)len)) {
        s->failed = true;
        return hpi_FALSE;
    }
    s->written += (uint32_t)len;
    return hpi_TRUE;
}

// ------------------------------------------------------------------------------------------------------------------------

static bool _patch_apply(void *const ctx, ota_patch_io_t *const io, const uint32_t new_size) {
    hpatch_t *const s = (hpatch_t *)ctx;
    if (s == NULL || io == NULL || io->read_patch == NULL || io->read_base == NULL || io->write_new == NULL)
        return false;
    s->io = io;
    s->want = new_size;
    s->written = 0u;
    s->failed = false;

    hpi_compressType ctype = hpi_compressType_no;
    hpi_pos_t newSize = 0u, uncompressSize = 0u;
    if (!hpatch_lite_open((hpi_TInputStreamHandle)s, _patch_read_diff, &ctype, &newSize, &uncompressSize))
        return false;                 /* not an hpatchi patch, or it ended inside its own header */
    if (ctype != hpi_compressType_no) /* A COMPRESSED PATCH IS REFUSED, NOT DECODED. See the note at the top */
        return false;
    if ((uint32_t)newSize != new_size) /* AND THE TWO SIZES MUST AGREE. */
        return false;

    _patch_listener_t l = { .base = { .diff_data = (hpi_TInputStreamHandle)s, .read_diff = _patch_read_diff, .read_old = _patch_read_old, .write_new = _patch_write_new }, .self = s };

    if (!hpatch_lite_patch(&l.base, newSize, s->cache, (hpi_size_t)sizeof(s->cache)))
        return false;
    return !s->failed && s->written == new_size;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const ota_patch_t *ota_patch_init(hpatch_t *const s) {
    if (s == NULL)
        return NULL;
    *s = (hpatch_t){ 0 };
    s->_ota_patch = (const ota_patch_t){ .ctx = s, .apply = _patch_apply };
    return &s->_ota_patch;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_HPATCH_H */
