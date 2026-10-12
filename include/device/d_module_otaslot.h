#ifndef D_MODULE_OTASLOT_H
#define D_MODULE_OTASLOT_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_otaslot.h -- the interface an OTA slot implements, and nothing else.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    void *ctx;
    bool (*begin)(void *ctx, uint32_t size); /* open the slot an update would go to */
    int (*write)(void *ctx, const void *buf, size_t len);
    bool (*end)(void *ctx);      /* finalise and verify the image; boots nothing */
    bool (*activate)(void *ctx); /* select it for the next boot. Irreversible. */
    void (*abort)(void *ctx);    /* give up, leaving what boots untouched */
    /* THE RAW IMAGE'S DIGEST, read back from the slot after end() -- sha256 over the image as the
       bundler sees it, which is where TARGET_HASH is checked. Optional: a slot that cannot answer
       simply does not get the check. */
    bool (*digest)(void *ctx, uint8_t *out, size_t outlen);
    /* THE IMAGE WE ARE RUNNING, at an arbitrary offset. Only a DELTA needs this, and it needs it
       randomly rather than in order: a patch is a sequence of "copy from there, insert this", and
       "there" goes backwards as often as forwards. It is the running image and not the slot's,
       because the base of a delta is what the node is executing -- and reading it while writing the
       other partition is safe, which is the whole reason there are two. Optional: a build with no
       patcher never calls it. */
    bool (*read_base)(void *ctx, uint32_t at, void *buf, size_t len);
} ota_slot_t;

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    void *ctx;
    bool (*save)(void *ctx);
} ota_record_t;

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    void *ctx;
    void (*begin)(void *ctx);
    void (*update)(void *ctx, const void *buf, size_t len);
    void (*final)(void *ctx, uint8_t *out, size_t outlen);
} ota_hash_t;

// ------------------------------------------------------------------------------------------------------------------------

/* THE DECOMPRESSOR, for a bundle whose payload is not the image. RAW DEFLATE -- no zlib or gzip
   wrapper, because the wrapper is bytes on a link measured in bytes per second for a checksum the
   bundle already carries. ota-bundle writes it with deflateRawSync(level 9).
 *
 * ONE STEP PER CALL, BOUNDED BOTH WAYS, which is what makes it usable from a cooperative loop. The
 * caller hands in up to `inlen` compressed bytes and an `out` buffer; step() consumes as much input
 * as the output it produces will fit and says how much of each. It may consume NOTHING and still
 * produce output, or consume input and produce none -- both are ordinary for an inflater -- so the
 * caller advances by *consumed and comes back, rather than assuming either.
 *
 * Returning 0 produced AND 0 consumed means no progress is possible, which the caller must treat as
 * a corrupt stream rather than looping on it. <0 is an outright error.
 *
 * WHY THE DEVICE OWNS IT. On an esp32c3 tinfl_decompress lives in ROM, so inflate costs no flash at
 * all; on a host it is zlib. Neither belongs in a shared header, and a 32KB sliding window is a
 * decision about RAM that only the application can make -- which is the same reason the slot and the
 * hash are seams. See WHY THE SLOT AND THE HASH ARE BOTH SEAMS in iotdata_node_content_ota.h. */
typedef struct {
    void *ctx;
    bool (*begin)(void *ctx);
    int (*step)(void *ctx, const void *in, size_t inlen, size_t *consumed, void *out, size_t outlen);
    bool (*end)(void *ctx); /* the stream ended where it should have */
    void (*abort)(void *ctx);
} ota_codec_t;

// ------------------------------------------------------------------------------------------------------------------------

/* THE PATCHER, for a bundle whose payload is a difference rather than an image.
 *
 * IT IS SHAPED NOTHING LIKE THE CODEC, and the difference is not arbitrary. An inflater can be told
 * "here are some bytes, give me what you can" and asked again, so ota_codec_t steps and the caller
 * keeps the loop. A patch applier cannot: it is a program -- copy from there, insert this, seek back
 * -- and it drives. HPatchLite's hpatch_lite_patch() runs start to finish and PULLS what it needs
 * through callbacks, and its read contract has no way to say "nothing yet, come back", only "that is
 * the end of the stream". detools' esp_delta_ota is the same shape in reverse and would have fitted
 * a step(), which is worth knowing if the engine is ever swapped; this is modelled on what the
 * engine we chose actually is, rather than on what would have been tidier.
 *
 * SO apply() DOES NOT RETURN UNTIL THE IMAGE IS WRITTEN, which breaks the one-bounded-chunk-per-pass
 * rule the rest of this consumer keeps. See THE DELTA TICK IS A LONG ONE in
 * iotdata_node_content_ota.h for what that costs and why it is accepted rather than engineered
 * around.
 *
 * The three callbacks are ours, not the engine's: the patch comes from the content store (through
 * the codec, if there is one), the base is the running image, and the output goes to the slot. The
 * engine is handed io and never learns where any of it lives. */
typedef struct ota_patch_io ota_patch_io_t;
struct ota_patch_io {
    void *ctx;
    /* Sequential. A SHORT READ MEANS THE END, which is the engine's contract and not a convenience:
       there is no third answer, so a reader that has merely run out for the moment must block or
       fail, never return zero and hope. */
    bool (*read_patch)(ota_patch_io_t *io, void *out, size_t *len);
    bool (*read_base)(ota_patch_io_t *io, uint32_t at, void *out, size_t len);
    bool (*write_new)(ota_patch_io_t *io, const void *buf, size_t len);
};

typedef struct {
    void *ctx;
    /* new_size is the finished image's length, which the bundle header carries as OUTPUT_SIZE. The
       engine also reads its own idea of it out of the patch, and an implementation should refuse a
       patch that disagrees rather than trusting either one alone. */
    bool (*apply)(void *ctx, ota_patch_io_t *io, uint32_t new_size);
} ota_patch_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_OTASLOT_H */
