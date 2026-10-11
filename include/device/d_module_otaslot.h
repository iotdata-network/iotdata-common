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
} ota_slot_t;

typedef struct {
    void *ctx;
    bool (*save)(void *ctx);
} ota_record_t;

typedef struct {
    void *ctx;
    void (*begin)(void *ctx);
    void (*update)(void *ctx, const void *buf, size_t len);
    void (*final)(void *ctx, uint8_t *out, size_t outlen);
} ota_hash_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_OTASLOT_H */
