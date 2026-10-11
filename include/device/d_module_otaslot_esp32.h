#ifndef D_MODULE_OTASLOT_ESP32_H
#define D_MODULE_OTASLOT_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_otaslot_esp32.h - the other app partition, and sha256. The device half of the two seams
// iotdata_node_content_ota_apply.h defines, and the ONLY code in the update path that cannot run on
// a host.
//
// It is deliberately this small. Everything that decides anything -- which bundles to take, what to
// do when a write fails, how many attempts a bundle gets before it is poison -- is in the consumer
// and is driven by a host suite with scripted versions of both of these. What is left here is four
// esp_ota_* calls and three mbedtls ones, and the reason to keep it down to that is that this is
// the file a mistake in cannot be caught before a flash.
//
// ------------------------------------------------------------------------------------------------------------------------
// end() AND activate() ARE TWO CALLS BECAUSE THEY ARE TWO DECISIONS
//
// esp_ota_end() finalises the write and VERIFIES the image -- the magic byte, the segment table,
// the appended sha256 -- and changes nothing about what the node boots. esp_ota_set_boot_partition()
// is the irreversible one: after it, the next reset runs code this node has never executed.
//
// Splitting them is what lets the consumer write its attempt counter in between, and that ordering
// is the whole safety argument: an attempt that is activated before it is recorded is an attempt
// that can repeat for ever. See THE ATTEMPT COUNTER in the consumer.
//
// A HANDLE IS NOT LEFT OPEN. esp_ota_begin() allocates and erases; abandoning one leaks the
// allocation and leaves the partition half-erased, and the consumer calls abort() on every failure
// path for exactly that reason. The flag here is what makes a second abort harmless.
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#pragma GCC diagnostic pop

#include "d_module_otaslot.h"

// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_otaslot = "device-otaslot";

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    bool open;
    ota_slot_t slot;
} otaslot_esp32_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _otaslot_esp32_begin(void *const ctx, const uint32_t size) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL)
        return false;
    if (s->open)
        (void)esp_ota_abort(s->handle); /* nothing should get here twice, but a leak is worse */
    s->open = false;
    if ((s->part = esp_ota_get_next_update_partition(NULL)) == NULL) {
        ESP_LOGE(__tag_device_otaslot, "begin: no update partition -- this build has no second slot");
        return false;
    }
    if (size > s->part->size) {
        ESP_LOGE(__tag_device_otaslot, "begin: %" PRIu32 " bytes will not fit %s (%" PRIu32 ")", size, s->part->label, (uint32_t)s->part->size);
        return false;
    }
    /* The SIZE and not OTA_SIZE_UNKNOWN, which lets the driver erase exactly what is needed rather
       than the whole partition -- a second or two of flash erase this node does not have to spend. */
    const esp_err_t err = esp_ota_begin(s->part, (size_t)size, &s->handle);
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_otaslot, "begin: %s on %s: %s", "esp_ota_begin", s->part->label, esp_err_to_name(err));
        return false;
    }
    s->open = true;
    ESP_LOGI(__tag_device_otaslot, "begin: %s at 0x%06" PRIx32 ", %" PRIu32 " bytes", s->part->label, (uint32_t)s->part->address, size);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _otaslot_esp32_write(void *const ctx, const void *const buf, const size_t len) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL || !s->open || buf == NULL)
        return -1;
    const esp_err_t err = esp_ota_write(s->handle, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_otaslot, "write: %u bytes: %s", (unsigned)len, esp_err_to_name(err));
        return -1;
    }
    return (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Finalises and VERIFIES, and boots nothing. A failure here is the image being malformed -- the
   wrong magic, a bad appended hash -- which is the one refusal that says the bundle is wrong rather
   than the flash being unlucky. */
static inline bool _otaslot_esp32_end(void *const ctx) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL || !s->open)
        return false;
    const esp_err_t err = esp_ota_end(s->handle);
    s->open = false; /* esp_ota_end consumes the handle whether it succeeded or not */
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_otaslot, "end: %s%s", esp_err_to_name(err), (err == ESP_ERR_OTA_VALIDATE_FAILED) ? " -- the written image is not a valid app" : "");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* THE IRREVERSIBLE ONE. After this the next reset runs the new image, and the only things standing
   between a bad image and a dead node are the bootloader's rollback and the consumer's attempt
   counter -- both of which are in place before this is called. */
static inline bool _otaslot_esp32_activate(void *const ctx) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL || s->part == NULL)
        return false;
    const esp_err_t err = esp_ota_set_boot_partition(s->part);
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_otaslot, "activate: %s: %s", s->part->label, esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(__tag_device_otaslot, "activate: next boot is %s -- on probation until it confirms itself", s->part->label);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* WHAT ACTUALLY LANDED, read back from the flash rather than remembered from the stream.
 *
 * esp_partition_get_sha256 is exactly the right call and it is worth saying why: for an app
 * partition it reads the appended digest, RECOMPUTES sha256 over image_len - 32, and refuses with
 * ESP_ERR_IMAGE_INVALID if the two disagree (bootloader_common_get_sha256_of_partition). So the
 * number it returns is a verified digest of the raw image -- not a value taken on trust from bytes
 * that could themselves be the corruption -- and it is the same definition ota-stage records. */
static inline bool _otaslot_esp32_digest(void *const ctx, uint8_t *const out, const size_t outlen) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL || s->part == NULL || out == NULL)
        return false;
    uint8_t full[32];
    const esp_err_t err = esp_partition_get_sha256(s->part, full);
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_otaslot, "digest: %s on %s: %s", "esp_partition_get_sha256", s->part->label, esp_err_to_name(err));
        return false;
    }
    memset(out, 0, outlen);
    memcpy(out, full, (outlen < sizeof(full)) ? outlen : sizeof(full));
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _otaslot_esp32_abort(void *const ctx) {
    otaslot_esp32_t *const s = (otaslot_esp32_t *)ctx;
    if (s == NULL || !s->open)
        return;
    (void)esp_ota_abort(s->handle);
    s->open = false;
    ESP_LOGW(__tag_device_otaslot, "abort: the write was abandoned -- what boots is unchanged");
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline const ota_slot_t *otaslot_esp32_init(otaslot_esp32_t *const s) {
    if (s == NULL)
        return NULL;
    *s = (otaslot_esp32_t){ 0 };
    s->slot = (const ota_slot_t){
        .ctx = s,
        .begin = _otaslot_esp32_begin,
        .write = _otaslot_esp32_write,
        .end = _otaslot_esp32_end,
        .activate = _otaslot_esp32_activate,
        .abort = _otaslot_esp32_abort,
        .digest = _otaslot_esp32_digest,
    };
    return &s->slot;
}

/* What an update would land in, for the device descriptor's slot_size -- 0 when there is no second
   slot at all, which ota_check reads as "I cannot say" and so does not refuse on size. */
static inline uint32_t otaslot_esp32_size(void) {
    const esp_partition_t *const next = esp_ota_get_next_update_partition(NULL);
    return (next != NULL) ? (uint32_t)next->size : 0u;
}

/* WHAT WE ARE RUNNING, by the same definition a bundle's TARGET_HASH uses -- so a delta's BASE_HASH
   can be compared against it. Fills the device descriptor's base_hash, and returns false when the
   running image cannot be digested, which leaves that field NULL and means "I cannot say" rather
   than "no". A wrong answer here would accept a patch against the wrong image. */
static inline bool otaslot_esp32_running_digest(uint8_t *const out, const size_t outlen) {
    const esp_partition_t *const running = esp_ota_get_running_partition();
    if (running == NULL || out == NULL)
        return false;
    uint8_t full[32];
    if (esp_partition_get_sha256(running, full) != ESP_OK)
        return false;
    memset(out, 0, outlen);
    memcpy(out, full, (outlen < sizeof(full)) ? outlen : sizeof(full));
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------
// THE HASH, WHICH IS PSA AND NOT mbedtls_sha256_*
//
// IDF 6.1 moved mbedtls onto TF-PSA-Crypto, and `mbedtls/sha256.h` went with it -- it exists, under
// mbedtls/private/, which is the maintainers saying not to include it. The supported API is PSA's,
// which is also what esp_ota_ops.c itself now hashes with (ota_calc_partition_bin_sha). So this
// uses psa_hash_setup/update/finish, and the only cost of the move is that the operation has to be
// aborted rather than freed when it is abandoned.
// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    psa_hash_operation_t op;
    bool open;
    ota_hash_t hash;
} otahash_esp32_t;

static inline void _otahash_esp32_begin(void *const ctx) {
    otahash_esp32_t *const h = (otahash_esp32_t *)ctx;
    if (h->open)
        (void)psa_hash_abort(&h->op); /* a previous run that was never finished */
    h->op = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    h->open = (psa_hash_setup(&h->op, PSA_ALG_SHA_256) == PSA_SUCCESS);
    if (!h->open)
        ESP_LOGE(__tag_device_otaslot, "hash: psa_hash_setup failed -- a payload hash cannot be checked");
}

static inline void _otahash_esp32_update(void *const ctx, const void *const buf, const size_t len) {
    otahash_esp32_t *const h = (otahash_esp32_t *)ctx;
    if (h->open && psa_hash_update(&h->op, (const uint8_t *)buf, len) != PSA_SUCCESS) {
        (void)psa_hash_abort(&h->op);
        h->open = false;
    }
}

/* TRUNCATED TO WHAT THE HEADER CARRIES: the format keeps 16 bytes of the digest and not 32 -- see
   WHY 16-BYTE HASHES in iotdata_node_content_ota.h -- so the full digest is computed and its first
   half is what gets compared.
 *
 * A HASH THAT FAILED COMES BACK AS ZEROES, which the consumer will compare against the header and
 * refuse. That is the right way round: a digest that could not be computed must not be allowed to
 * pass as a digest that matched. */
static inline void _otahash_esp32_final(void *const ctx, uint8_t *const out, const size_t outlen) {
    otahash_esp32_t *const h = (otahash_esp32_t *)ctx;
    uint8_t full[32];
    size_t got = 0u;
    memset(out, 0, outlen);
    if (!h->open)
        return;
    const psa_status_t st = psa_hash_finish(&h->op, full, sizeof(full), &got);
    h->open = false;
    if (st != PSA_SUCCESS || got == 0u) {
        ESP_LOGE(__tag_device_otaslot, "hash: psa_hash_finish failed");
        return;
    }
    memcpy(out, full, (outlen < got) ? outlen : got);
}

static inline const ota_hash_t *otahash_esp32_init(otahash_esp32_t *const h) {
    if (h == NULL)
        return NULL;
    *h = (otahash_esp32_t){ 0 };
    h->hash = (const ota_hash_t){
        .ctx = h,
        .begin = _otahash_esp32_begin,
        .update = _otahash_esp32_update,
        .final = _otahash_esp32_final,
    };
    return &h->hash;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_OTASLOT_ESP32_H */
