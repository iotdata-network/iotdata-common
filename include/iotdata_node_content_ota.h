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
//   0x01 APP            str   the application name, matched against IOTDATA_NODE_VERSION_APP
//   0x02 SEMVER         str   advisory, for humans and logs -- NOT the ordering key
//   0x03 STAMP          str   yyyymmddhhmm, 12 chars -- THE ordering key
//   0x04 FORM           u8    IOTDATA_NODE_OTA_FORM_*
//   0x05 CODEC          u8    IOTDATA_NODE_OTA_CODEC_*
//   0x06 FLAGS          u8    IOTDATA_NODE_OTA_FLAG_*
//   0x07 BOOTLOADER_MIN u32   vs esp_bootloader_desc_t.version -- a FLOOR
//   0x08 OUTPUT_SIZE    u32   the size of the resulting app image
//   0x09 TARGET_HASH    16B   what the result must hash to
//   0x0A BASE_HASH      16B   DELTA ONLY: the one image this patch applies to -- EXACT
//   0x0B PAYLOAD_SIZE   u32   the bytes AFTER END -- what the blob should weigh on arrival
//   0x0C PAYLOAD_HASH   16B   of those same bytes, before any decoding
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
// WHY THE PAYLOAD DESCRIBES ITSELF, when the transfer already had integrity. The content layer
// checks a CRC-64 as the bytes arrive and then FORGETS it -- nothing persists it -- so after a
// restart a stored blob is just bytes. Three things then have no check at all: a blob that sat in
// flash for weeks and rotted, a blob a filesystem mangled during unrelated housekeeping, and a blob
// baked into a factory image that never crossed a link in the first place.
//
// Without these two keys the only remaining check is TARGET_HASH, which is verified AFTER the
// assembly -- and assembly begins with esp_ota_begin(), which ERASES THE INACTIVE SLOT. That slot
// holds the image a rollback would return to. So the real argument is not extra assurance: it is
// that a blob which was never going to apply must be refused BEFORE it costs us the thing that
// makes a bad update survivable.
//
// SIZE IS O(1) AND IS THE ONE THAT WAS ACTUALLY MISSING. A truncated or appended-to blob was
// previously undetectable until inflate ran dry; now it costs one comparison, and nothing needs to
// be read. The hash is O(n) with no decode, which is still far cheaper than the decode it guards.
//
// WHY NOT A THIRD ALGORITHM. CRC-64/XZ is the CONTENT layer's (it covers a transfer); sha256-16 is
// this header's (it covers identity). Reusing sha256 here means the device needs no second
// primitive beyond the one TARGET_HASH already forces on it, and nobody has to remember which of
// three checks covers which bytes.
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
// WHAT A CLIENT MUST CHECK, cheapest and most fatal first -- iotdata_node_ota_check() is this list
//
//   1. the magic                      not ours at all
//   2. APP vs IOTDATA_NODE_VERSION_APP     a relay image on a sensor bricks it just as thoroughly
//   3. PAYLOAD_SIZE vs the file       truncated or added to: nothing below it means anything
//   4. FORM / CODEC supported         refuse what cannot be decoded before fetching it
//   5. BOOTLOADER_MIN vs ours         needs a cable visit, not a download
//   6. OUTPUT_SIZE vs the free slot   do not spend airtime on something that cannot land
//   7. BASE_HASH vs the running app   DELTA ONLY, and before a single byte is written
//   8. PAYLOAD_HASH vs the bytes      O(n), no decode -- the last check before the slot is ERASED
//   9. TARGET_HASH vs the result      after assembly, before the boot slot is flipped
//
// Steps 1-7 are answerable from the header and the file's length alone, which is the reason the
// header leads the blob: everything that can refuse an update cheaply does so before the expensive
// part begins. Step 3 sits where it does because the header itself survived the parse -- a
// truncation can only have taken payload -- so the fields above it are trustworthy and none of the
// ones below it are worth evaluating on a blob that is not all there.
//
// STEP 8 IS NOT IN iotdata_node_ota_check(), because it needs the bytes and this header reads none.
// The caller hashes (its platform already has sha256 for step 9) and asks
// iotdata_node_ota_payload_hash_matches().
// -----------------------------------------------------------------------------------------------------------------------------------------

/* kvr framing comes from the library (iotdata_fields.h, via iotdata.h); include that first. */

#define IOTDATA_NODE_OTA_MAGIC              "IOTA"
#define IOTDATA_NODE_OTA_MAGIC_LEN          4
#define IOTDATA_NODE_OTA_HASH_LEN           16 /* truncated sha256 -- see WHY 16-BYTE HASHES above */
#define IOTDATA_NODE_OTA_APP_MAX            24 /* as IOTDATA_NODE_VERSION_FIRMWARE_MAX */
#define IOTDATA_NODE_OTA_SEMVER_MAX         24
#define IOTDATA_NODE_OTA_STAMP_LEN          12 /* as IOTDATA_NODE_VERSION_STAMP_LEN */

#define IOTDATA_NODE_OTA_KEY_END            0x00 /* length 0; payload follows */
#define IOTDATA_NODE_OTA_KEY_APP            0x01
#define IOTDATA_NODE_OTA_KEY_SEMVER         0x02
#define IOTDATA_NODE_OTA_KEY_STAMP          0x03
#define IOTDATA_NODE_OTA_KEY_FORM           0x04
#define IOTDATA_NODE_OTA_KEY_CODEC          0x05
#define IOTDATA_NODE_OTA_KEY_FLAGS          0x06
#define IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN 0x07
#define IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE    0x08
#define IOTDATA_NODE_OTA_KEY_TARGET_HASH    0x09
#define IOTDATA_NODE_OTA_KEY_BASE_HASH      0x0A
#define IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE   0x0B
#define IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH   0x0C

#define IOTDATA_NODE_OTA_FORM_FULL          0x00
#define IOTDATA_NODE_OTA_FORM_DELTA         0x01

#define IOTDATA_NODE_OTA_CODEC_NONE         0x00
#define IOTDATA_NODE_OTA_CODEC_DEFLATE      0x01
#define IOTDATA_NODE_OTA_CODEC_HEATSHRINK   0x02 /* RESERVED, not implemented either side */

#define IOTDATA_NODE_OTA_FLAG_DOWNGRADE     0x01u /* the stamp going backwards is intended, not an error */

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    char app[IOTDATA_NODE_OTA_APP_MAX + 1];
    char semver[IOTDATA_NODE_OTA_SEMVER_MAX + 1];
    char stamp[IOTDATA_NODE_OTA_STAMP_LEN + 1];
    uint8_t form, codec, flags;
    uint32_t bootloader_min;
    uint32_t output_size;
    uint8_t target_hash[IOTDATA_NODE_OTA_HASH_LEN];
    uint8_t base_hash[IOTDATA_NODE_OTA_HASH_LEN];
    uint8_t payload_hash[IOTDATA_NODE_OTA_HASH_LEN];
    bool has_target_hash, has_base_hash, has_payload_hash, has_payload_size;
    uint32_t payload_size; /* what the payload SHOULD weigh; 0/absent means nothing is claimed */
    size_t payload_offset; /* where the payload starts -- the byte after END */
} iotdata_node_ota_header_t;

typedef enum {
    IOTDATA_NODE_OTA_OK = 0,
    IOTDATA_NODE_OTA_ERR_MAGIC,        /* not an iotdata OTA blob                     */
    IOTDATA_NODE_OTA_ERR_TRUNCATED,    /* ran off the end before END                  */
    IOTDATA_NODE_OTA_ERR_NO_END,       /* kvr walked out without the END sentinel     */
    IOTDATA_NODE_OTA_ERR_APP,          /* a different application entirely            */
    IOTDATA_NODE_OTA_ERR_FORM,         /* form we cannot decode                       */
    IOTDATA_NODE_OTA_ERR_CODEC,        /* codec we cannot decode                      */
    IOTDATA_NODE_OTA_ERR_BOOTLOADER,   /* needs a newer bootloader -- cable visit     */
    IOTDATA_NODE_OTA_ERR_SIZE,         /* will not fit the slot it must land in       */
    IOTDATA_NODE_OTA_ERR_BASE,         /* delta against an image we are not running   */
    IOTDATA_NODE_OTA_ERR_NO_BASE,      /* delta with no BASE_HASH at all              */
    IOTDATA_NODE_OTA_ERR_OLDER,        /* older stamp without FLAG_DOWNGRADE          */
    IOTDATA_NODE_OTA_ERR_PAYLOAD_SIZE, /* the blob is not the length it says it is     */
    IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH, /* it is the right length and the wrong bytes   */
} iotdata_node_ota_status_t;

static inline const char *iotdata_node_ota_status_name(const iotdata_node_ota_status_t s) {
    switch (s) {
    case IOTDATA_NODE_OTA_OK:
        return "ok";
    case IOTDATA_NODE_OTA_ERR_MAGIC:
        return "not an ota blob";
    case IOTDATA_NODE_OTA_ERR_TRUNCATED:
        return "truncated";
    case IOTDATA_NODE_OTA_ERR_NO_END:
        return "no end marker";
    case IOTDATA_NODE_OTA_ERR_APP:
        return "different application";
    case IOTDATA_NODE_OTA_ERR_FORM:
        return "unsupported form";
    case IOTDATA_NODE_OTA_ERR_CODEC:
        return "unsupported codec";
    case IOTDATA_NODE_OTA_ERR_BOOTLOADER:
        return "needs a newer bootloader";
    case IOTDATA_NODE_OTA_ERR_SIZE:
        return "will not fit the slot";
    case IOTDATA_NODE_OTA_ERR_BASE:
        return "delta base is not running";
    case IOTDATA_NODE_OTA_ERR_NO_BASE:
        return "delta base is non-existant";
    case IOTDATA_NODE_OTA_ERR_OLDER:
        return "older (downgrade not flagged)";
    case IOTDATA_NODE_OTA_ERR_PAYLOAD_SIZE:
        return "not equal to the header length";
    case IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH:
        return "not equal to the header hash";
    default:
        break; /* -Wswitch-default on the device: a value from a newer bundler is not a bug here */
    }
    return "unknown";
}

static inline const char *iotdata_node_ota_form_name(const uint8_t f) {
    return (f == IOTDATA_NODE_OTA_FORM_FULL) ? "full" : (f == IOTDATA_NODE_OTA_FORM_DELTA) ? "delta" : "?";
}
static inline const char *iotdata_node_ota_codec_name(const uint8_t c) {
    return (c == IOTDATA_NODE_OTA_CODEC_NONE) ? "none" : (c == IOTDATA_NODE_OTA_CODEC_DEFLATE) ? "deflate" : (c == IOTDATA_NODE_OTA_CODEC_HEATSHRINK) ? "heatshrink" : "?";
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_ota_status_t iotdata_node_ota_parse(const uint8_t *const buf, const size_t len, iotdata_node_ota_header_t *const out) {
    if (buf == NULL || out == NULL || len < IOTDATA_NODE_OTA_MAGIC_LEN + 2u)
        return IOTDATA_NODE_OTA_ERR_TRUNCATED;
    if (memcmp(buf, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN) != 0)
        return IOTDATA_NODE_OTA_ERR_MAGIC;

    *out = (iotdata_node_ota_header_t){ 0 };
    size_t cursor = IOTDATA_NODE_OTA_MAGIC_LEN;
    uint8_t key = 0, vlen = 0;
    const uint8_t *val = NULL;
    bool ended = false;

    while (!ended && iotdata_kvr_next(buf, len, &cursor, &key, &val, &vlen)) {
        switch (key) {
        case IOTDATA_NODE_OTA_KEY_END:
            /* length MUST be zero: that is what separates the sentinel from a stray byte pair */
            if (vlen != 0u)
                return IOTDATA_NODE_OTA_ERR_NO_END;
            ended = true;
            break;
        case IOTDATA_NODE_OTA_KEY_APP:
            (void)iotdata_kvr_str(val, vlen, out->app, sizeof(out->app));
            break;
        case IOTDATA_NODE_OTA_KEY_SEMVER:
            (void)iotdata_kvr_str(val, vlen, out->semver, sizeof(out->semver));
            break;
        case IOTDATA_NODE_OTA_KEY_STAMP:
            (void)iotdata_kvr_str(val, vlen, out->stamp, sizeof(out->stamp));
            break;
        case IOTDATA_NODE_OTA_KEY_FORM:
            out->form = iotdata_kvr_u8(val, vlen, IOTDATA_NODE_OTA_FORM_FULL);
            break;
        case IOTDATA_NODE_OTA_KEY_CODEC:
            out->codec = iotdata_kvr_u8(val, vlen, IOTDATA_NODE_OTA_CODEC_NONE);
            break;
        case IOTDATA_NODE_OTA_KEY_FLAGS:
            out->flags = iotdata_kvr_u8(val, vlen, 0u);
            break;
        case IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN:
            out->bootloader_min = iotdata_kvr_u32(val, vlen, 0u);
            break;
        case IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE:
            out->output_size = iotdata_kvr_u32(val, vlen, 0u);
            break;
        case IOTDATA_NODE_OTA_KEY_TARGET_HASH:
            if (vlen == IOTDATA_NODE_OTA_HASH_LEN) {
                memcpy(out->target_hash, val, IOTDATA_NODE_OTA_HASH_LEN);
                out->has_target_hash = true;
            }
            break;
        case IOTDATA_NODE_OTA_KEY_BASE_HASH:
            if (vlen == IOTDATA_NODE_OTA_HASH_LEN) {
                memcpy(out->base_hash, val, IOTDATA_NODE_OTA_HASH_LEN);
                out->has_base_hash = true;
            }
            break;
        case IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE:
            /* has_* rather than a zero test: a zero-length payload is malformed, not absent, and the
               two want different answers -- absent is an older bundler, zero is a broken one. */
            out->payload_size = iotdata_kvr_u32(val, vlen, 0u);
            out->has_payload_size = true;
            break;
        case IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH:
            if (vlen == IOTDATA_NODE_OTA_HASH_LEN) {
                memcpy(out->payload_hash, val, IOTDATA_NODE_OTA_HASH_LEN);
                out->has_payload_hash = true;
            }
            break;
        default:
            break; /* UNKNOWN KEYS ARE SKIPPED -- the whole reason this is kvr */
        }
    }
    if (!ended)
        return IOTDATA_NODE_OTA_ERR_NO_END;
    out->payload_offset = cursor;
    return IOTDATA_NODE_OTA_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Everything the device knows about itself that bears on whether it can take a blob. Passed in
   rather than read here, so this header stays free of esp_* and can be unit-tested on a host. */
typedef struct {
    const char *app;          /* IOTDATA_NODE_VERSION_APP                                  */
    const char *stamp;        /* our running build's stamp, or NULL to skip the check  */
    uint32_t bootloader;      /* esp_bootloader_desc_t.version                         */
    uint32_t slot_size;       /* the partition the result must land in                 */
    const uint8_t *base_hash; /* our running app's hash, or NULL if unknown            */
    bool codec_deflate;       /* what this build can actually decode                   */
    bool codec_heatshrink;
} iotdata_node_ota_device_t;

/* The checklist from the header comment, in that order: the cheapest and most fatal refusals come
   first so nothing expensive starts on a blob that was never going to be taken.

   `blob_len` is the WHOLE file, header included, and 0 means "I cannot say" -- the same convention
   d->stamp == NULL and d->slot_size == 0 already use, so a caller that only has a header in hand
   loses nothing. A blob with no PAYLOAD_SIZE key is likewise not refused: unknown and absent keys
   are what makes this format extensible, and an older bundler's output stays acceptable. The cost
   of both skips is named rather than hidden -- neither blob gets the length check, and truncation
   then surfaces where it always did, at the far end of the decode. */
static inline iotdata_node_ota_status_t iotdata_node_ota_check(const iotdata_node_ota_header_t *const h, const iotdata_node_ota_device_t *const d, const size_t blob_len) {
    if (h == NULL || d == NULL)
        return IOTDATA_NODE_OTA_ERR_TRUNCATED;
    if (d->app != NULL && h->app[0] != '\0' && strcmp(h->app, d->app) != 0)
        return IOTDATA_NODE_OTA_ERR_APP;
    /* Before anything below it: the header survived the parse, so a truncation can only have taken
       payload, and no field down there is worth weighing on a blob that is not all there. */
    if (h->has_payload_size && blob_len != 0u && (uint64_t)h->payload_offset + (uint64_t)h->payload_size != (uint64_t)blob_len)
        return IOTDATA_NODE_OTA_ERR_PAYLOAD_SIZE;
    if (h->form != IOTDATA_NODE_OTA_FORM_FULL && h->form != IOTDATA_NODE_OTA_FORM_DELTA)
        return IOTDATA_NODE_OTA_ERR_FORM;
    if ((h->codec == IOTDATA_NODE_OTA_CODEC_DEFLATE && !d->codec_deflate) || (h->codec == IOTDATA_NODE_OTA_CODEC_HEATSHRINK && !d->codec_heatshrink) || (h->codec > IOTDATA_NODE_OTA_CODEC_HEATSHRINK))
        return IOTDATA_NODE_OTA_ERR_CODEC;
    if (h->bootloader_min > d->bootloader)
        return IOTDATA_NODE_OTA_ERR_BOOTLOADER;
    if (h->output_size == 0u || (d->slot_size != 0u && h->output_size > d->slot_size))
        return IOTDATA_NODE_OTA_ERR_SIZE;
    if (h->form == IOTDATA_NODE_OTA_FORM_DELTA) {
        if (!h->has_base_hash)
            return IOTDATA_NODE_OTA_ERR_NO_BASE;
        if (d->base_hash != NULL && memcmp(h->base_hash, d->base_hash, IOTDATA_NODE_OTA_HASH_LEN) != 0)
            return IOTDATA_NODE_OTA_ERR_BASE;
    }
    /* Newness last, because it is the only refusal here that is a POLICY rather than an
       impossibility -- and the only one a flag is allowed to override. */
    if (d->stamp != NULL && h->stamp[0] != '\0' && strcmp(h->stamp, d->stamp) <= 0 && (h->flags & IOTDATA_NODE_OTA_FLAG_DOWNGRADE) == 0u)
        return IOTDATA_NODE_OTA_ERR_OLDER;
    return IOTDATA_NODE_OTA_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Step 8, which iotdata_node_ota_check() cannot do: it needs the payload and this header reads
   nothing. THE CALLER HASHES -- its platform already has sha256 for TARGET_HASH -- and asks here.
   Truncated sha256 of the payload exactly as stored, before any decoding.

   ABSENT IS A PASS, deliberately. A blob from a bundler older than the key is not refused; it simply
   does not get this check, which is the same bargain every other optional key here strikes. A caller
   that wants to insist can test has_payload_hash itself. */
static inline iotdata_node_ota_status_t iotdata_node_ota_payload_hash_check(const iotdata_node_ota_header_t *const h, const uint8_t *const hash, const size_t hash_len) {
    if (h == NULL)
        return IOTDATA_NODE_OTA_ERR_TRUNCATED;
    if (!h->has_payload_hash)
        return IOTDATA_NODE_OTA_OK;
    if (hash == NULL || hash_len < IOTDATA_NODE_OTA_HASH_LEN)
        return IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH;
    return (memcmp(h->payload_hash, hash, IOTDATA_NODE_OTA_HASH_LEN) == 0) ? IOTDATA_NODE_OTA_OK : IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_OTA_H */
