#ifndef IOTDATA_NODE_CONTENT_OTA_H
#define IOTDATA_NODE_CONTENT_OTA_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_content_ota.h -- the OTA blob format: a kvr header, then the payload.
//
// NOT WIRED INTO ANYTHING YET. This is the format definition and its decoder, written first so the
// off-box bundler (iotdata-common/tools/ota-bundle) and the eventual on-device client cannot drift
// apart. Nothing includes this header yet, and that is deliberate.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// SCOPE: THE APP PARTITION, AND NOTHING ELSE
//
// An OTA replaces the application and never the bootloader, the partition table or otadata. That is
// a safety property rather than a restriction:
//
//   ota_0 / ota_1   two slots and a rollback -- a failed write is recoverable
//   partition table single copy, read by the bootloader at boot -- a failed write is a BRICK
//   bootloader      single copy, loaded by ROM -- a failed write is a BRICK
//   otadata         it IS the OTA state; shipping one would override which slot is active
//
// So when any of those three must change, the answer is a person with a cable, not a cleverer
// payload. What the format carries instead is a COMPATIBILITY FLOOR (see BOOTLOADER_MIN) so a blob
// that needs a newer bootloader is refused rather than half-applied.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// FRAMING: MAGIC, THEN KVR, THEN PAYLOAD
//
//   [MAGIC 4][K][L][V]...[K][L][V][END 0x00][0x00][payload ...]
//
// The metadata is ordinary kvr (iotdata_fields.h): [key u8][len u8][value]. THERE IS NO VERSION
// FIELD and that is the point of using kvr -- a reader skips keys it does not know, so a key can be
// added without breaking an older client, and iotdata_kvr_* returns the caller's default when a
// value's width is not what the key expects, so a field can even change width without breaking one.
// If the FRAMING itself ever has to change incompatibly, the MAGIC changes; that is what it is for.
//
// END is key 0x00 with length 0. Keys are numbered per type throughout iotdata, so 0x00 is ours to
// spend here, and requiring the length to be zero keeps it distinguishable from stray bytes. The
// payload starts at the byte after it -- the header is as long as it needs to be and no longer.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE FIELDS, in the order they are emitted (and so the order they read out of a hex dump)
//
//   0x01 APP            str   the application name, matched against IOTDATA_VERSION_APP
//   0x02 SEMVER         str   advisory, for humans and logs -- NOT the ordering key
//   0x03 STAMP          str   yyyymmddhhmm, 12 chars -- THE ordering key
//   0x04 FORM           u8    IOTDATA_OTA_FORM_*
//   0x05 CODEC          u8    IOTDATA_OTA_CODEC_*
//   0x06 FLAGS          u8    IOTDATA_OTA_FLAG_*
//   0x07 BOOTLOADER_MIN u32   vs esp_bootloader_desc_t.version -- a FLOOR
//   0x08 OUTPUT_SIZE    u32   the size of the resulting app image
//   0x09 TARGET_HASH    16B   what the result must hash to
//   0x0A BASE_HASH      16B   DELTA ONLY: the one image this patch applies to -- EXACT
//
// WHY THE STAMP AND NOT THE SEMVER. iotdata_node_version.h already rules on this: the semver is
// "normative and hand-set at release points, NOT the ordering key", while the stamp is "the ONLY
// thing here that anything should compare". It stays a 12-char string rather than becoming a number
// because fixed-width yyyymmddhhmm compares chronologically under strcmp, needs no conversion, and
// reads as a date in a dump.
//
// WHY BASE_HASH IS EXACT AND BOOTLOADER_MIN IS A FLOOR. A delta is DERIVED FROM exactly one image,
// so there is precisely one valid source and a range would be meaningless -- applied to anything
// else it produces a plausible-looking corrupt image. A bootloader is DEPENDED ON, and one app
// build is usually good with several, so a floor is the honest expression. An exact hash there would
// force a lockstep the fleet does not have.
//
// WHY 16-BYTE HASHES. These are identity and compatibility checks, not authenticity ones -- the
// deployer's blob signature already carries authenticity. Truncated sha256 at 128 bits is far past
// any accidental collision, and halves what the hashes cost on a link measured in bytes per second.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// FORM AND CODEC ARE INDEPENDENT
//
// FORM says what the payload IS once decoded (a whole image, or a patch against BASE_HASH); CODEC
// says how it was squeezed for the wire. Keeping them orthogonal means a self-compressing patch
// format is expressible as FORM_DELTA + CODEC_NONE without the format needing to know, which is the
// escape hatch if on-device memory ever forces one.
//
// CODEC_DEFLATE is the working default: miniz is already in ESP-IDF and a 32KB inflate window is
// affordable on a C3. CODEC_HEATSHRINK is RESERVED, NOT IMPLEMENTED -- it wants ~1-2KB of state
// instead of 32KB, which will matter one day, but an encoder written with no decoder to test it
// against is worse than no encoder. XZ IS DELIBERATELY ABSENT: liblzma's decoder allocates the
// dictionary size recorded in the stream, which at -9 is 64MiB and simply cannot run here. xz stays
// where it belongs, on the sftp bundles a Linux box unpacks.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT A CLIENT MUST CHECK, cheapest and most fatal first -- iotdata_ota_check() is this list
//
//   1. the magic                      not ours at all
//   2. APP vs IOTDATA_VERSION_APP     a relay image on a sensor bricks it just as thoroughly
//   3. FORM / CODEC supported         refuse what cannot be decoded before fetching it
//   4. BOOTLOADER_MIN vs ours         needs a cable visit, not a download
//   5. OUTPUT_SIZE vs the free slot   do not spend airtime on something that cannot land
//   6. BASE_HASH vs the running app   DELTA ONLY, and before a single byte is written
//   7. TARGET_HASH vs the result      after assembly, before the boot slot is flipped
//
// Steps 1-6 are answerable from the header alone, which is the reason the header leads the blob:
// everything that can refuse an update does so before the expensive part begins.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* kvr framing comes from the library (iotdata_fields.h, via iotdata.h); include that first. */

#define IOTDATA_OTA_MAGIC      "IOTA"
#define IOTDATA_OTA_MAGIC_LEN  4
#define IOTDATA_OTA_HASH_LEN   16 /* truncated sha256 -- see WHY 16-BYTE HASHES above */
#define IOTDATA_OTA_APP_MAX    24 /* as IOTDATA_VERSION_FIRMWARE_MAX */
#define IOTDATA_OTA_SEMVER_MAX 24
#define IOTDATA_OTA_STAMP_LEN  12 /* as IOTDATA_VERSION_STAMP_LEN */

#define IOTDATA_OTA_KEY_END            0x00 /* length 0; payload follows */
#define IOTDATA_OTA_KEY_APP            0x01
#define IOTDATA_OTA_KEY_SEMVER         0x02
#define IOTDATA_OTA_KEY_STAMP          0x03
#define IOTDATA_OTA_KEY_FORM           0x04
#define IOTDATA_OTA_KEY_CODEC          0x05
#define IOTDATA_OTA_KEY_FLAGS          0x06
#define IOTDATA_OTA_KEY_BOOTLOADER_MIN 0x07
#define IOTDATA_OTA_KEY_OUTPUT_SIZE    0x08
#define IOTDATA_OTA_KEY_TARGET_HASH    0x09
#define IOTDATA_OTA_KEY_BASE_HASH      0x0A

#define IOTDATA_OTA_FORM_FULL  0x00
#define IOTDATA_OTA_FORM_DELTA 0x01

#define IOTDATA_OTA_CODEC_NONE       0x00
#define IOTDATA_OTA_CODEC_DEFLATE    0x01
#define IOTDATA_OTA_CODEC_HEATSHRINK 0x02 /* RESERVED, not implemented either side */

#define IOTDATA_OTA_FLAG_DOWNGRADE 0x01u /* the stamp going backwards is intended, not an error */

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    char app[IOTDATA_OTA_APP_MAX + 1];
    char semver[IOTDATA_OTA_SEMVER_MAX + 1];
    char stamp[IOTDATA_OTA_STAMP_LEN + 1];
    uint8_t form, codec, flags;
    uint32_t bootloader_min;
    uint32_t output_size;
    uint8_t target_hash[IOTDATA_OTA_HASH_LEN];
    uint8_t base_hash[IOTDATA_OTA_HASH_LEN];
    bool has_target_hash, has_base_hash;
    size_t payload_offset; /* where the payload starts -- the byte after END */
} iotdata_ota_header_t;

typedef enum {
    IOTDATA_OTA_OK = 0,
    IOTDATA_OTA_ERR_MAGIC,      /* not an iotdata OTA blob                     */
    IOTDATA_OTA_ERR_TRUNCATED,  /* ran off the end before END                  */
    IOTDATA_OTA_ERR_NO_END,     /* kvr walked out without the END sentinel     */
    IOTDATA_OTA_ERR_APP,        /* a different application entirely            */
    IOTDATA_OTA_ERR_FORM,       /* form we cannot decode                       */
    IOTDATA_OTA_ERR_CODEC,      /* codec we cannot decode                      */
    IOTDATA_OTA_ERR_BOOTLOADER, /* needs a newer bootloader -- cable visit     */
    IOTDATA_OTA_ERR_SIZE,       /* will not fit the slot it must land in       */
    IOTDATA_OTA_ERR_BASE,       /* delta against an image we are not running   */
    IOTDATA_OTA_ERR_NO_BASE,    /* delta with no BASE_HASH at all              */
    IOTDATA_OTA_ERR_OLDER,      /* older stamp without FLAG_DOWNGRADE          */
} iotdata_ota_status_t;

static inline const char *iotdata_ota_status_name(const iotdata_ota_status_t s) {
    switch (s) {
    case IOTDATA_OTA_OK: return "ok";
    case IOTDATA_OTA_ERR_MAGIC: return "not an ota blob";
    case IOTDATA_OTA_ERR_TRUNCATED: return "truncated";
    case IOTDATA_OTA_ERR_NO_END: return "no end marker";
    case IOTDATA_OTA_ERR_APP: return "different application";
    case IOTDATA_OTA_ERR_FORM: return "unsupported form";
    case IOTDATA_OTA_ERR_CODEC: return "unsupported codec";
    case IOTDATA_OTA_ERR_BOOTLOADER: return "needs a newer bootloader";
    case IOTDATA_OTA_ERR_SIZE: return "will not fit the slot";
    case IOTDATA_OTA_ERR_BASE: return "delta base is not what we are running";
    case IOTDATA_OTA_ERR_NO_BASE: return "delta without a base";
    case IOTDATA_OTA_ERR_OLDER: return "older, and downgrade not flagged";
    }
    return "unknown";
}

static inline const char *iotdata_ota_form_name(const uint8_t f) { return (f == IOTDATA_OTA_FORM_FULL) ? "full" : (f == IOTDATA_OTA_FORM_DELTA) ? "delta" : "?"; }
static inline const char *iotdata_ota_codec_name(const uint8_t c) {
    return (c == IOTDATA_OTA_CODEC_NONE) ? "none" : (c == IOTDATA_OTA_CODEC_DEFLATE) ? "deflate" : (c == IOTDATA_OTA_CODEC_HEATSHRINK) ? "heatshrink" : "?";
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Parse only -- says what the blob CLAIMS, never whether we should take it. Unknown keys are
   skipped, which is what makes the format extensible; absent keys leave their field at zero, so a
   caller tests has_* rather than comparing against a sentinel that could be real data. */
static inline iotdata_ota_status_t iotdata_ota_parse(const uint8_t *const buf, const size_t len, iotdata_ota_header_t *const out) {
    if (buf == NULL || out == NULL || len < IOTDATA_OTA_MAGIC_LEN + 2u)
        return IOTDATA_OTA_ERR_TRUNCATED;
    if (memcmp(buf, IOTDATA_OTA_MAGIC, IOTDATA_OTA_MAGIC_LEN) != 0)
        return IOTDATA_OTA_ERR_MAGIC;

    *out = (iotdata_ota_header_t){ 0 };
    size_t cursor = IOTDATA_OTA_MAGIC_LEN;
    uint8_t key = 0, vlen = 0;
    const uint8_t *val = NULL;
    bool ended = false;

    while (!ended && iotdata_kvr_next(buf, len, &cursor, &key, &val, &vlen)) {
        switch (key) {
        case IOTDATA_OTA_KEY_END:
            /* length MUST be zero: that is what separates the sentinel from a stray byte pair */
            if (vlen != 0u)
                return IOTDATA_OTA_ERR_NO_END;
            ended = true;
            break;
        case IOTDATA_OTA_KEY_APP: (void)iotdata_kvr_str(val, vlen, out->app, sizeof(out->app)); break;
        case IOTDATA_OTA_KEY_SEMVER: (void)iotdata_kvr_str(val, vlen, out->semver, sizeof(out->semver)); break;
        case IOTDATA_OTA_KEY_STAMP: (void)iotdata_kvr_str(val, vlen, out->stamp, sizeof(out->stamp)); break;
        case IOTDATA_OTA_KEY_FORM: out->form = iotdata_kvr_u8(val, vlen, IOTDATA_OTA_FORM_FULL); break;
        case IOTDATA_OTA_KEY_CODEC: out->codec = iotdata_kvr_u8(val, vlen, IOTDATA_OTA_CODEC_NONE); break;
        case IOTDATA_OTA_KEY_FLAGS: out->flags = iotdata_kvr_u8(val, vlen, 0u); break;
        case IOTDATA_OTA_KEY_BOOTLOADER_MIN: out->bootloader_min = iotdata_kvr_u32(val, vlen, 0u); break;
        case IOTDATA_OTA_KEY_OUTPUT_SIZE: out->output_size = iotdata_kvr_u32(val, vlen, 0u); break;
        case IOTDATA_OTA_KEY_TARGET_HASH:
            if (vlen == IOTDATA_OTA_HASH_LEN) {
                memcpy(out->target_hash, val, IOTDATA_OTA_HASH_LEN);
                out->has_target_hash = true;
            }
            break;
        case IOTDATA_OTA_KEY_BASE_HASH:
            if (vlen == IOTDATA_OTA_HASH_LEN) {
                memcpy(out->base_hash, val, IOTDATA_OTA_HASH_LEN);
                out->has_base_hash = true;
            }
            break;
        default: break; /* UNKNOWN KEYS ARE SKIPPED -- the whole reason this is kvr */
        }
    }
    if (!ended)
        return IOTDATA_OTA_ERR_NO_END;
    out->payload_offset = cursor;
    return IOTDATA_OTA_OK;
}

/* Everything the device knows about itself that bears on whether it can take a blob. Passed in
   rather than read here, so this header stays free of esp_* and can be unit-tested on a host. */
typedef struct {
    const char *app;           /* IOTDATA_VERSION_APP                                  */
    const char *stamp;         /* our running build's stamp, or NULL to skip the check  */
    uint32_t bootloader;       /* esp_bootloader_desc_t.version                         */
    uint32_t slot_size;        /* the partition the result must land in                 */
    const uint8_t *base_hash;  /* our running app's hash, or NULL if unknown            */
    bool codec_deflate;        /* what this build can actually decode                   */
    bool codec_heatshrink;
} iotdata_ota_device_t;

/* The checklist from the header comment, in that order: the cheapest and most fatal refusals come
   first so nothing expensive starts on a blob that was never going to be taken. */
static inline iotdata_ota_status_t iotdata_ota_check(const iotdata_ota_header_t *const h, const iotdata_ota_device_t *const d) {
    if (h == NULL || d == NULL)
        return IOTDATA_OTA_ERR_TRUNCATED;
    if (d->app != NULL && h->app[0] != '\0' && strcmp(h->app, d->app) != 0)
        return IOTDATA_OTA_ERR_APP;
    if (h->form != IOTDATA_OTA_FORM_FULL && h->form != IOTDATA_OTA_FORM_DELTA)
        return IOTDATA_OTA_ERR_FORM;
    if ((h->codec == IOTDATA_OTA_CODEC_DEFLATE && !d->codec_deflate) || (h->codec == IOTDATA_OTA_CODEC_HEATSHRINK && !d->codec_heatshrink) ||
        (h->codec > IOTDATA_OTA_CODEC_HEATSHRINK))
        return IOTDATA_OTA_ERR_CODEC;
    if (h->bootloader_min > d->bootloader)
        return IOTDATA_OTA_ERR_BOOTLOADER;
    if (h->output_size == 0u || (d->slot_size != 0u && h->output_size > d->slot_size))
        return IOTDATA_OTA_ERR_SIZE;
    if (h->form == IOTDATA_OTA_FORM_DELTA) {
        if (!h->has_base_hash)
            return IOTDATA_OTA_ERR_NO_BASE;
        if (d->base_hash != NULL && memcmp(h->base_hash, d->base_hash, IOTDATA_OTA_HASH_LEN) != 0)
            return IOTDATA_OTA_ERR_BASE;
    }
    /* Newness last, because it is the only refusal here that is a POLICY rather than an
       impossibility -- and the only one a flag is allowed to override. */
    if (d->stamp != NULL && h->stamp[0] != '\0' && strcmp(h->stamp, d->stamp) <= 0 && (h->flags & IOTDATA_OTA_FLAG_DOWNGRADE) == 0u)
        return IOTDATA_OTA_ERR_OLDER;
    return IOTDATA_OTA_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_OTA_H */
