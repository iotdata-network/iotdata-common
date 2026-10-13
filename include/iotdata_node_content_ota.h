#ifndef IOTDATA_NODE_CONTENT_OTA_H
#define IOTDATA_NODE_CONTENT_OTA_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_content_ota.h -- the OTA blob format: a kvr header, then the payload.
//
// NOT WIRED INTO ANYTHING YET. This is the format definition and its decoder, written first so the
// off-box bundler (iotdata-platform/tools/ota-bundle) and the eventual on-device client cannot drift
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

#include <inttypes.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------------------------------------------------

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
    const char *app;          /* IOTDATA_NODE_VERSION_APP                                  */
    const char *stamp;        /* our running build's stamp, or NULL to skip the check  */
    uint32_t bootloader;      /* esp_bootloader_desc_t.version                         */
    uint32_t slot_size;       /* the partition the result must land in                 */
    const uint8_t *base_hash; /* our running app's hash, or NULL if unknown            */
    bool codec_deflate;       /* what this build can actually decode                   */
    bool codec_heatshrink;
} iotdata_node_ota_device_t;

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
    IOTDATA_NODE_OTA_ERR_TARGET_HASH,  /* it arrived intact and did not land intact    */
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
    case IOTDATA_NODE_OTA_ERR_TARGET_HASH:
        return "not what landed in the slot";
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
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT THE SECOND HALF NEEDS, AND THE FIRST HALF DOES NOT
//
// The format half above parses a header and judges it, and needs nothing but the library's kvr
// reader -- which is why a suite can include it on its own, and why anything at all can inspect
// what ota-bundle produced. The consumer half below reads a file out of the content store, so it
// needs that, and this file now needs it too.
//
// IT DOES NOT NEED THE STATE STORE, and that is deliberate rather than lucky. The attempt counter
// has to be written down somewhere that survives a reboot, and the obvious way to say so was to
// take an iotdata_node_state_t -- which drags in iotdata_node_state.h, which needs a datastore_t,
// which is a DEVICE module an application picks (NVS here, a directory there). A shared header
// cannot make that choice, and a suite testing a header parser has no business having one. So
// writing the record down is a seam like the other two: see THE ATTEMPT COUNTER.
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// PART TWO: THE CONSUMER -- the one thing that takes a bundle out of the content store and makes
// it the firmware this node runs next.
//
// It is a content CONSUMER and nothing more exotic: it registers with iotdata_node_content.h, is
// offered every file the store holds, claims the ones that are firmware for this build, and is
// ticked while it has writing to do. Everything about who owns what, what survives a reboot and
// what gets reaped is the store's, already built and already tested.
//
// IT IS IN THIS FILE AND NOT ONE OF ITS OWN. The half above knows what the bytes MEAN -- it parses
// a header and judges it against a device -- and this half knows what to DO about the verdict. They
// were briefly two files and that was the wrong cut: nothing else will ever parse one of these
// headers, nothing else will ever apply one, and a reader of either half wants the other in front
// of them. iotdata_node_firmware.h stays separate because it is a genuinely different subject --
// the slot we are RUNNING FROM and whether the bootloader trusts it, which is true with or without
// any of this. All three meet at one moment, the instant a node is told to boot something it has
// never run, and that moment is below.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHY THE SLOT AND THE HASH ARE BOTH SEAMS
//
// Writing an OTA partition is four esp_ota_* calls, and sha256 is mbedtls. Neither exists on a
// host, and if this module called either one directly then the part of it that actually decides
// things -- which bundles to take, what to do when one fails, how many times to try before giving
// up on it -- would only be reachable with a cable and a flash. That is the part worth testing, and
// it is the part where a mistake bricks a remote site.
//
// So both are vtables, exactly as the content store's backend is: d_module_otaslot_esp32.h fills
// the slot with the real thing, and the host harness fills both with scripted ones that can be told
// to fail at any point. The dozen lines that talk to the hardware are the only lines that cannot be
// tested on a host, and they are in a file of their own.
//
// THE TEST'S HASH IS NOT A REAL SHA256, AND THAT IS BETTER. What this module owes its caller is
// that it compares the payload against the header and refuses a mismatch; proving that needs a
// digest the test can CONTROL, not a real one it would have to find a collision for.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ATTEMPT COUNTER, WHICH IS THE WHOLE SAFETY ARGUMENT
//
// Rollback (iotdata_node_firmware.h) catches an image that boots and then dies. It does not catch
// the loop: the bundle is still in the store, so the next time this consumer is offered it, it
// applies it again, and the node spends the rest of its life rebooting between two images. Rollback
// makes each individual failure survivable and makes the overall failure permanent.
//
// So the attempt is WRITTEN DOWN BEFORE IT IS TAKEN -- the count and the stamp being attempted go
// into the state store and are flushed, synchronously, and only then is the slot activated. A node
// that comes back up can therefore always tell which of three situations it is in:
//
//   nothing recorded                  no update was in flight. Normal.
//   recorded, and the stamp is MINE   the update booted. Not yet proven -- the record stays until
//                                     the bootloader's probation is confirmed, because an image
//                                     that rolls back after we cleared it would be applied again.
//   recorded, and the stamp is NOT    it did not stick: either it never booted, or it booted and
//                                     rolled back. Worth another go, up to the limit, and after
//                                     that the bundle is refused and said to be refused.
//
// THE STAMP IS THE IDENTITY, not a hash, because it is the one thing both sides certainly have: the
// bundle carries it in its header and the running build has it compiled in, both from the same
// twelve digits of `date -u`. A hash would be better and is not available -- see WHAT CANNOT BE
// VERIFIED YET below.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT TARGET_HASH IS TAKEN OVER, AND WHY IT IS CHECKED TWICE
//
// TWO HASHES, TWO DIFFERENT QUESTIONS, and the update path asks both:
//
//   PAYLOAD_HASH  over the payload exactly as stored -- "are the bytes I was given the bytes the
//                 bundler made". Computed as the payload streams past, before anything decodes it.
//   TARGET_HASH   over the image as it now sits in the slot -- "are the bytes I wrote the bytes I
//                 was given". Read back from the flash after the write.
//
// Between the two sits the flash write, which is the one step neither of the others covers, so a
// full bundle really does want both.
//
// AND IT IS THE RAW IMAGE, NOT THE .bin FILE. esptool appends 32 bytes of sha256 to an app image
// and flags it in the image header, so sha256(file) is a hash OF a hash OF the image -- a number
// nothing but esptool's own output can reproduce, and in particular not a device: what a device can
// compute for an image in flash is the digest WITHOUT those 32 bytes, because that is what they
// are. Hashing the bare image instead gives a definition both ends produce independently, and the
// device's is VERIFIED rather than trusted -- esp_partition_get_sha256 recomputes it and refuses
// the partition if the appended bytes disagree.
//
// The appended bytes stay in the payload regardless. hash_appended is set in the image header, so
// the bootloader expects them; it is the HASHING that ignores them, not the transfer.
//
// DELTA IS STILL REFUSED, by the form check, and now for a smaller reason: BASE_HASH is answerable
// with the same call, so what is missing is the patcher rather than the identity.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Bytes moved from the store into the slot per work() call. A whole image in one go would hold the
   main loop for as long as the flash took, and this node has a radio to service: 1 KB is a
   millisecond or two, and ~200 passes for a 200 KB image, which on a loop that runs constantly is
   a fraction of a second of wall clock and never more than a blink of it at a time.
 *
 * AND THE BUFFERS LIVE IN THE CONSUMER, NOT IN THE FUNCTIONS, which is what the size is really
   constrained by. A node task here has a 1 KB stack-usage ceiling in the build (-Wstack-usage), so a
   kilobyte of locals in one function is already over it -- the bytes have to persist somewhere else.
 *
 * THAT SOMEWHERE IS iotdata_node_ota_apply_t AND NOT A FUNCTION STATIC. They were function statics,
   which cost the same RAM and read as though they cost nothing: a `static uint8_t buf[1024]` three
   call levels down is invisible in the struct that is supposed to describe what a consumer owns, and
   it quietly makes the consumer a singleton -- two of them would scribble over each other with no
   warning at all. In the struct, the ownership is written down, the size shows up where somebody is
   counting RAM, and a second consumer is merely a second allocation.
 *
 * TWO OF THEM AND NOT THREE: `wire` holds what came off the store, `plain` what the codec made of
   it. A delta needs only `wire`, because its output goes to the patcher's own buffer. */
#ifndef IOTDATA_NODE_OTA_APPLY_CHUNK
#define IOTDATA_NODE_OTA_APPLY_CHUNK 1024u
#endif

/* How many times one bundle may be attempted before it is treated as poison. Three: one for a
   transient (a bad flash write, a brownout mid-boot), and two more to be sure, after which the
   evidence is that this bundle does not run on this node. */
#ifndef IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX
#define IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX 3u
#endif

#define IOTDATA_NODE_OTA_APPLY_STATE_TAG 0x4F544131UL /* "OTA1" */

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Persisted across the reboot it is written for. Deliberately tiny: a state block costs its bytes
   in every save of every block, and what has to survive is an id, an identity and a count. */
typedef struct {
    uint32_t id; /* the bundle being attempted; 0 = nothing in flight */
    char stamp[IOTDATA_NODE_OTA_STAMP_LEN + 1];
    uint8_t attempts;
} iotdata_node_ota_apply_state_t;

typedef enum {
    IOTDATA_NODE_OTA_APPLY_IDLE = 0,
    IOTDATA_NODE_OTA_APPLY_WRITING, /* streaming the payload into the slot */
    IOTDATA_NODE_OTA_APPLY_FAILED,  /* this bundle will not be tried again this boot */
} iotdata_node_ota_apply_phase_t;

typedef struct {
    iotdata_node_content_consumer_t consumer;
    const iotdata_node_ota_device_t *device;
    const ota_slot_t *slot;
    const ota_hash_t *hash;
    const ota_codec_t *codec; /* NULL = this build can only take CODEC_NONE */
    const ota_patch_t *patch; /* NULL = this build can only take FORM_FULL   */
    iotdata_node_content_t *store;
    const ota_record_t *record;

    iotdata_node_ota_apply_state_t persist;
    const char *stamp; /* OUR build's, told once at begin: see THE ATTEMPT COUNTER */
    bool enabled;

    iotdata_node_ota_apply_phase_t phase;
    uint32_t id;      /* the bundle being written */
    uint32_t off;     /* payload bytes CONSUMED so far -- of the wire payload, compressed or not */
    uint32_t out;     /* and image bytes produced from them, which differ once a codec is in the way */
    uint32_t refused; /* a bundle this boot will not touch again; 0 = none */
    iotdata_node_ota_header_t head;
    iotdata_node_ota_status_t why; /* the last verdict, for describe() */

    uint32_t stat_claimed, stat_written, stat_applied, stat_rejected;

    /* see AND THE BUFFERS LIVE IN THE CONSUMER: payload bytes as they arrive, and what the codec
       turned them into. Last in the struct because they dwarf everything above them. */
    uint8_t wire[IOTDATA_NODE_OTA_APPLY_CHUNK];
    uint8_t plain[IOTDATA_NODE_OTA_APPLY_CHUNK];
} iotdata_node_ota_apply_t;

static iotdata_node_ota_apply_t *_iotdata_node_ota_apply = NULL;

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Reads the header out of the store. Cheap: one read of the first block, which is all a header is. */
static inline iotdata_node_ota_status_t _iotdata_node_ota_apply_head(iotdata_node_ota_apply_t *const a, const uint32_t id, iotdata_node_ota_header_t *const out, uint32_t *const total) {
    uint8_t peek[IOTDATA_NODE_CONTENT_BLOCK_BYTES];
    uint32_t held = 0u;
    if (!iotdata_node_content_stat(a->store, id, total, &held) || held == 0u)
        return IOTDATA_NODE_OTA_ERR_TRUNCATED;
    const uint32_t want = (held < (uint32_t)sizeof(peek)) ? held : (uint32_t)sizeof(peek);
    if (iotdata_node_content_read(a->store, id, 0u, peek, want) != (int)want)
        return IOTDATA_NODE_OTA_ERR_TRUNCATED;
    return iotdata_node_ota_parse(peek, (size_t)want, out);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* OFFERED A FILE. Cheap checks only -- a header and a verdict, no payload and no flash.
 *
 * NOT MINE and MINE-BUT-NO is the distinction that matters here. A file that is not an OTA bundle,
 * or is one for another application, is not this consumer's business at all and is left entirely
 * alone: something else may own it, and if nothing does the store says so. A bundle that IS for
 * this build and is refused anyway -- older, too big, needs a newer bootloader -- is claimed and
 * then not worked on, so that `content inspect` has somebody to ask and the reason is on record.
 * It is deliberately NOT deleted: a refusal can stop being one. A newer bootloader arrives, or the
 * same bundle is re-cut with the downgrade flag set, and a bundle thrown away for being early is a
 * bundle that has to be sent twice. */
static inline iotdata_node_content_result_t _iotdata_node_ota_apply_claim(void *const ctx, const uint32_t id, __attribute__((unused)) const uint32_t size) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)ctx;
    if (a == NULL || a->store == NULL)
        return IOTDATA_NODE_CONTENT_R_NONE;

    iotdata_node_ota_header_t h;
    uint32_t total = 0u;
    const iotdata_node_ota_status_t pst = _iotdata_node_ota_apply_head(a, id, &h, &total);
    if (pst == IOTDATA_NODE_OTA_ERR_MAGIC)
        return IOTDATA_NODE_CONTENT_R_NONE; /* not firmware: most content is not */
    if (pst != IOTDATA_NODE_OTA_OK) {
        a->why = pst;
        return IOTDATA_NODE_CONTENT_R_MINE; /* a broken bundle is still ours to describe */
    }
    if (a->device != NULL && a->device->app != NULL && h.app[0] != '\0' && strcmp(h.app, a->device->app) != 0)
        return IOTDATA_NODE_CONTENT_R_NONE; /* another application's: not ours to hold an opinion on */

    const iotdata_node_ota_status_t cst = iotdata_node_ota_check(&h, a->device, (size_t)total);
    a->why = cst;
    if (cst != IOTDATA_NODE_OTA_OK)
        return IOTDATA_NODE_CONTENT_R_MINE;
    /* AND ONLY FULL, which iotdata_node_ota_check does NOT decide for us: it judges a delta on its
       own terms and passes one whose base matches what we are running. That is correct of IT and
       would be fatal here -- without a patcher this consumer would stream the PATCH into the slot
       as though it were an image, and a slot full of patch is an image that may well still boot.
       So the form is refused explicitly, by the half that knows what it can actually do. */
    if (h.form != IOTDATA_NODE_OTA_FORM_FULL && (h.form != IOTDATA_NODE_OTA_FORM_DELTA || a->patch == NULL)) {
        a->why = IOTDATA_NODE_OTA_ERR_FORM;
        return IOTDATA_NODE_CONTENT_R_MINE;
    }
    /* A DELTA ALSO NEEDS SOMETHING TO PATCH, and the slot is the only thing that can hand us the
       image we are running. A build with a patcher and no way to read its own base would stream a
       difference against nothing. */
    if (h.form == IOTDATA_NODE_OTA_FORM_DELTA && (a->slot->read_base == NULL)) {
        a->why = IOTDATA_NODE_OTA_ERR_FORM;
        return IOTDATA_NODE_CONTENT_R_MINE;
    }
    /* AND ONLY A CODEC WE WERE ACTUALLY GIVEN A DECODER FOR, which iotdata_node_ota_check also does
       not decide for us: it asks the DEVICE what codecs it can decode and believes the answer. That
       is right of it -- the capability is the application's to declare -- but declaring a capability
       is not the same as handing this consumer something to decode with. Those are two different
       facts, and only one of them is visible from here: the codec seam, or NULL.
     *
     * Getting this wrong was worse than the delta case. A patch at least has to be asked for, while
       deflate is what ota-bundle emits BY DEFAULT -- so the ordinary command produced the one bundle
       that could not be applied safely, and a build advertising codec_deflate wrote its COMPRESSED
       payload into the other partition as though it were an image. */
    if (h.codec != IOTDATA_NODE_OTA_CODEC_NONE && a->codec == NULL) {
        a->why = IOTDATA_NODE_OTA_ERR_CODEC;
        return IOTDATA_NODE_CONTENT_R_MINE;
    }
    /* Ours, and takeable -- but not this boot if we have already been told not to. */
    if (!a->enabled || id == a->refused || a->phase == IOTDATA_NODE_OTA_APPLY_FAILED)
        return IOTDATA_NODE_CONTENT_R_MINE;
    if (a->persist.id == id && a->persist.attempts >= (uint8_t)IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX)
        return IOTDATA_NODE_CONTENT_R_MINE; /* tried enough: see THE ATTEMPT COUNTER */
    if (a->phase != IOTDATA_NODE_OTA_APPLY_IDLE && a->id != id)
        return IOTDATA_NODE_CONTENT_R_MINE; /* one at a time */

    a->head = h;
    a->id = id;
    a->off = 0u;
    a->phase = IOTDATA_NODE_OTA_APPLY_IDLE; /* work() opens the slot: claim() touches no flash */
    a->stat_claimed++;
    return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_WORK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_ota_apply_give_up(iotdata_node_ota_apply_t *const a, const iotdata_node_ota_status_t why) {
    if (a->phase == IOTDATA_NODE_OTA_APPLY_WRITING && a->slot != NULL && a->slot->abort != NULL)
        a->slot->abort(a->slot->ctx);
    /* and the decompressor, which on a device is holding a 32KB window plus its tables */
    if (a->phase == IOTDATA_NODE_OTA_APPLY_WRITING && a->head.codec != IOTDATA_NODE_OTA_CODEC_NONE && a->codec != NULL && a->codec->abort != NULL)
        a->codec->abort(a->codec->ctx);
    a->phase = IOTDATA_NODE_OTA_APPLY_FAILED;
    a->refused = a->id;
    a->why = why;
    a->off = 0u;
    a->stat_rejected++;
}

/* ------------------------------------------------------------------------------------------------
   THE DELTA'S THREE CALLBACKS. The patcher drives, so these are what it pulls through -- see the
   seam's own note on why it cannot be stepped like the codec.
   ---------------------------------------------------------------------------------------------- */

/* THE PATCH, WHICH IS THE PAYLOAD, AND MUST BE HANDED OVER IN FULL. A short read tells the engine
   the patch has ended, so this loops until the buffer is full or the wire payload is genuinely
   exhausted -- the codec may hand back less than was asked on any one step, and stopping there
   would report the end of a patch that had barely started. That mistake, made once in a test
   harness, produced an apply that wrote nothing and blamed the patch.
 *
 * The hash sees the WIRE bytes, compressed as they arrived, exactly once each -- the same rule the
 * full path follows, and the reason `used` and not `want` is what advances a->off. */
static inline bool _iotdata_node_ota_read_patch(ota_patch_io_t *const io, void *const out, size_t *const len) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)io->ctx;
    uint8_t *const dst = (uint8_t *)out;
    size_t done = 0u;
    /* NOT `while (... && a->off < payload_size)`, WHICH CONFLATES TWO DIFFERENT ENDINGS. Running out
       of PAYLOAD is not running out of PATCH: a codec may be holding a great deal of output that no
       more input is needed to produce. Stopping at the last wire byte returns a short read, a short
       read means end-of-stream to the patcher, and the apply fails having written most of an image.
     *
     * THE TWO CODECS DIFFER INSIDE THEIR CONTRACT, which is what made this invisible on a host. zlib
       consumes lazily -- never more input than the output it was asked for -- so `a->off` crawls and
       the loop keeps going by accident. The C3's ROM tinfl consumes EAGERLY and buffers into its
       32KB window, so 529 bytes of payload were swallowed whole with 388KB still inside the codec.
       Both are correct; only one of them forgave the bug. Measured on a relay: the delta failed with
       `unsupported form` after writing nothing, and the same bundle applied perfectly on a host.
     *
     * So the loop ends when the CODEC says it has nothing left -- no output and no input taken --
       and the drain below asks it with an empty input to find out. */
    while (done < *len) {
        const uint32_t remain = (a->head.payload_size > a->off) ? (a->head.payload_size - a->off) : 0u;
        const uint32_t want = (remain < (uint32_t)sizeof(a->wire)) ? remain : (uint32_t)sizeof(a->wire);
        if (want > 0u && iotdata_node_content_read(a->store, a->id, (uint32_t)a->head.payload_offset + a->off, a->wire, want) != (int)want)
            return false;
        size_t used = 0u, made = 0u;
        if (a->head.codec == IOTDATA_NODE_OTA_CODEC_NONE) {
            if (want == 0u)
                break; /* uncompressed: no payload left IS no patch left, there is nothing to drain */
            const size_t room = *len - done;
            made = used = (want < room) ? (size_t)want : room;
            memcpy(&dst[done], a->wire, made);
        } else {
            /* want may be 0 here, and that is the DRAIN: an empty input asks the codec whether it
               still has output buffered. 0 produced and 0 taken is the only honest end. */
            const int got = a->codec->step(a->codec->ctx, a->wire, (size_t)want, &used, &dst[done], *len - done);
            if (got < 0 || used > (size_t)want)
                return false;
            if (got == 0 && used == 0u)
                break; /* nothing left to give: the patch ends here, and *len says so below */
            made = (size_t)got;
        }
        if (a->hash != NULL && used > 0u)
            a->hash->update(a->hash->ctx, a->wire, used);
        a->off += (uint32_t)used;
        a->stat_written += (uint32_t)used;
        done += made;
    }
    *len = done; /* short ONLY because the payload ran out, which IS the end of the patch */
    return true;
}

/* THE BASE, WHICH IS THE IMAGE WE ARE RUNNING, at whatever offset the patch asks for. */
static inline bool _iotdata_node_ota_read_base(ota_patch_io_t *const io, const uint32_t at, void *const out, const size_t len) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)io->ctx;
    return a->slot->read_base != NULL && a->slot->read_base(a->slot->ctx, at, out, len);
}

/* AND THE OUTPUT, STRAIGHT INTO THE SLOT, counted so the finish can check it against OUTPUT_SIZE. */
static inline bool _iotdata_node_ota_write_new(ota_patch_io_t *const io, const void *const buf, const size_t len) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)io->ctx;
    if (a->slot->write(a->slot->ctx, buf, len) != (int)len)
        return false;
    a->out += (uint32_t)len;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* TICKED WHILE THERE IS WRITING TO DO, one bounded chunk per call. The loop this runs in has a
   radio to service, so the shape of this function is "move CHUNK bytes and return" and the only
   thing that happens at the end of the payload is the handover below. */
static inline iotdata_node_content_result_t _iotdata_node_ota_apply_work(void *const ctx, const uint32_t id) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)ctx;
    if (a == NULL || a->store == NULL || a->slot == NULL || id != a->id)
        return IOTDATA_NODE_CONTENT_R_MINE;

    if (a->phase == IOTDATA_NODE_OTA_APPLY_FAILED)
        return IOTDATA_NODE_CONTENT_R_MINE;

    if (a->phase == IOTDATA_NODE_OTA_APPLY_IDLE) {
        if (!a->slot->begin(a->slot->ctx, a->head.output_size)) {
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
            return IOTDATA_NODE_CONTENT_R_MINE;
        }
        if (a->hash != NULL)
            a->hash->begin(a->hash->ctx);
        if (a->head.codec != IOTDATA_NODE_OTA_CODEC_NONE && (a->codec == NULL || a->codec->begin == NULL || !a->codec->begin(a->codec->ctx))) {
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_CODEC);
            return IOTDATA_NODE_CONTENT_R_MINE;
        }
        a->phase = IOTDATA_NODE_OTA_APPLY_WRITING;
        a->off = a->out = 0u;
    }

    /* THE DELTA TICK IS A LONG ONE, and that is a real cost rather than an oversight.
     *
     * Everything else in this function moves one bounded chunk and returns, so the pass gets back to
     * the radio. A patcher cannot be driven that way -- it is a program of copies and inserts, it
     * drives, and its read contract has no way to say "nothing yet" (see ota_patch_t). So this one
     * call writes the whole image before it returns: a few seconds, during which the node relays
     * nothing. It is spending them on something it is about to reboot out of anyway, and the
     * alternative -- a second task, a queue and the patcher blocking on it -- buys a few seconds of
     * mesh traffic for a lot of machinery that can go wrong at exactly the wrong moment.
     *
     * WHAT IT MUST NOT DO is let the task watchdog fire, so the slot's write is where a device
     * implementation feeds it; writing 384KB through esp_ota_write is well inside a 10s timeout, but
     * the margin belongs to whoever knows the flash, not to this header. */
    if (a->head.form == IOTDATA_NODE_OTA_FORM_DELTA) {
        ota_patch_io_t io = {
            .ctx = a,
            .read_patch = _iotdata_node_ota_read_patch,
            .read_base = _iotdata_node_ota_read_base,
            .write_new = _iotdata_node_ota_write_new,
        };
        if (a->patch == NULL || a->patch->apply == NULL || !a->patch->apply(a->patch->ctx, &io, a->head.output_size)) {
            /* The patch did not fit the base, or stopped early, or asked for more than the header
               promised. None of those get better by trying again with the same two files. */
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_FORM);
            return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
        }
        /* and fall through to the finish, which checks the hashes and the length as it always does */
    }

    const uint32_t remain = (a->head.form == IOTDATA_NODE_OTA_FORM_DELTA) ? 0u : ((a->head.payload_size > a->off) ? (a->head.payload_size - a->off) : 0u);
    if (remain > 0u) {
        const uint32_t want = (remain < (uint32_t)sizeof(a->wire)) ? remain : (uint32_t)sizeof(a->wire);
        if (iotdata_node_content_read(a->store, id, (uint32_t)a->head.payload_offset + a->off, a->wire, want) != (int)want) {
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_TRUNCATED);
            return IOTDATA_NODE_CONTENT_R_MINE;
        }
        uint32_t took = want;
        if (a->head.codec == IOTDATA_NODE_OTA_CODEC_NONE) {
            if (a->slot->write(a->slot->ctx, a->wire, (size_t)want) != (int)want) {
                _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
                return IOTDATA_NODE_CONTENT_R_MINE;
            }
            a->out += want;
        } else {
            /* ONE STEP, NOT A LOOP, which is the whole reason the seam reports what it consumed. An
               inflater's output is unbounded in terms of its input -- a kilobyte of compressed zeros
               is a megabyte of image -- so a loop here would hold the pass for as long as the
               bundle felt like, starve the radio and eventually trip the task watchdog. Instead the
               output buffer bounds the tick, step() takes only as much input as its output will fit,
               and the remainder is re-read from the store next time round. */
            size_t used = 0u;
            const int made = a->codec->step(a->codec->ctx, a->wire, (size_t)want, &used, a->plain, sizeof(a->plain));
            if (made < 0 || used > (size_t)want || (made == 0 && used == 0u)) {
                /* no progress is a corrupt stream, not something to come back to */
                _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_CODEC);
                return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
            }
            if (made > 0 && a->slot->write(a->slot->ctx, a->plain, (size_t)made) != made) {
                _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
                return IOTDATA_NODE_CONTENT_R_MINE;
            }
            /* AND NOT MORE THAN THE HEADER PROMISED. output_size is what the slot was opened for,
               so a stream that keeps producing past it is one the slot would refuse anyway -- but it
               is caught here, by name, rather than as a write failure that reads like a bad flash. */
            a->out += (uint32_t)made;
            if (a->out > a->head.output_size) {
                _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
                return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
            }
            took = (uint32_t)used;
        }
        /* THE HASH COVERS THE WIRE PAYLOAD, compressed as it arrived -- that is what the bundler
           hashed -- so it sees exactly the bytes step() consumed, once each, and never the ones it
           has yet to look at. */
        if (a->hash != NULL && took > 0u)
            a->hash->update(a->hash->ctx, a->wire, (size_t)took);
        a->off += took;
        a->stat_written += took;
        return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_WORK; /* come back for the next one */
    }

    /* THE PAYLOAD IS ALL THERE. Everything from here to the reboot happens in one pass, because
       there is nothing left that can be usefully interrupted and every step is cheap. */

    /* THE STREAM ENDED WHERE IT SHOULD HAVE, which the byte count alone does not say: a truncated
       deflate stream can consume every payload byte and still be waiting for more, and an inflater
       is the only thing that knows. Asked before the hashes, because "the compressed data stops
       mid-symbol" is a more useful answer than "the digest did not match". */
    if (a->head.codec != IOTDATA_NODE_OTA_CODEC_NONE && a->codec != NULL && a->codec->end != NULL && !a->codec->end(a->codec->ctx)) {
        _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_CODEC);
        return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
    }
    /* AND PRODUCED EXACTLY THE IMAGE THE HEADER DESCRIBED. For CODEC_NONE this restates what
       iotdata_node_ota_check already compared; for a codec it is the first time anyone can know. */
    if (a->out != a->head.output_size) {
        _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
        return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
    }

    if (a->hash != NULL) {
        uint8_t digest[IOTDATA_NODE_OTA_HASH_LEN];
        a->hash->final(a->hash->ctx, digest, sizeof(digest));
        if (iotdata_node_ota_payload_hash_check(&a->head, digest, sizeof(digest)) != IOTDATA_NODE_OTA_OK) {
            /* The right number of the wrong bytes. The store already verified what ARRIVED against
               the sender's checksum, so this is not a transfer fault -- the file genuinely is not
               what its header says it is, and keeping it would mean trying it again for ever. */
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH);
            return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE;
        }
    }
    if (!a->slot->end(a->slot->ctx)) {
        _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_PAYLOAD_HASH);
        return IOTDATA_NODE_CONTENT_R_MINE;
    }

    /* AND WHAT ACTUALLY LANDED ON FLASH, which is a different question from what streamed past.
       PAYLOAD_HASH above says the bytes we were given were the bytes the bundler made;
       TARGET_HASH here says the bytes now in the slot are those bytes. Between the two sits the
       flash write, which is the one step neither of the others covers.
     *
       The digest is over the RAW IMAGE -- see WHAT TARGET_HASH IS TAKEN OVER. A slot that cannot
       answer is not an error: the check is skipped, exactly as an absent key would be. */
    if (a->slot->digest != NULL && a->head.has_target_hash) {
        uint8_t landed[IOTDATA_NODE_OTA_HASH_LEN];
        if (!a->slot->digest(a->slot->ctx, landed, sizeof(landed)) || memcmp(a->head.target_hash, landed, sizeof(landed)) != 0) {
            _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_TARGET_HASH);
            return IOTDATA_NODE_CONTENT_R_MINE;
        }
    }

    /* WRITTEN DOWN BEFORE IT IS TAKEN. If the flush does not land we do not activate: an attempt
       nobody recorded is an attempt nobody can count, and the loop this counter exists to stop
       would be back. Refusing to apply is the safe side of that -- the node carries on running
       what it is running. */
    /* PER BUNDLE, which means comparing the id BEFORE overwriting it. The count answers "how many
       times have we tried THIS image", and carrying it across a different bundle made the next one
       inherit the last one's failures: a node that had burnt three attempts on a bad image reported
       the one that replaced it as "attempt 4 -- giving up", on the boot where it had just run. A new
       id starts at one. */
    a->persist.attempts = (a->persist.id == a->id && a->persist.attempts < 0xFFu) ? (uint8_t)(a->persist.attempts + 1u) : ((a->persist.id == a->id) ? 0xFFu : 1u);
    a->persist.id = a->id;
    (void)snprintf(a->persist.stamp, sizeof(a->persist.stamp), "%s", a->head.stamp);
    if (a->record != NULL && a->record->save != NULL && !a->record->save(a->record->ctx)) {
        _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_TRUNCATED);
        return IOTDATA_NODE_CONTENT_R_MINE;
    }
    if (!a->slot->activate(a->slot->ctx)) {
        _iotdata_node_ota_apply_give_up(a, IOTDATA_NODE_OTA_ERR_SIZE);
        return IOTDATA_NODE_CONTENT_R_MINE;
    }
    a->phase = IOTDATA_NODE_OTA_APPLY_IDLE;
    a->off = 0u;
    a->stat_applied++;
    a->why = IOTDATA_NODE_OTA_OK;
    /* MINE and REBOOT, and NOT DELETE. The bundle stays: if the new image never comes up there is
       nothing to retry from and nothing to look at, and the attempt counter is what stops that
       retry being infinite. */
    return IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_REBOOT;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* One line for `content inspect`, which is the only place an operator finds out WHY a bundle that
   is plainly for this node is sitting there not being applied. */
static inline int _iotdata_node_ota_apply_describe(void *const ctx, const uint32_t id, char *const out, const size_t size) {
    iotdata_node_ota_apply_t *const a = (iotdata_node_ota_apply_t *)ctx;
    if (a == NULL || out == NULL || size == 0u)
        return 0;
    iotdata_node_ota_header_t h;
    uint32_t total = 0u;
    if (_iotdata_node_ota_apply_head(a, id, &h, &total) != IOTDATA_NODE_OTA_OK)
        return 0;
    const char *what = "held";
    if (a->id == id && a->phase == IOTDATA_NODE_OTA_APPLY_WRITING)
        what = "applying";
    else if (a->id == id && a->phase == IOTDATA_NODE_OTA_APPLY_FAILED)
        what = "given up on";
    else if (!a->enabled)
        what = "not applied: updates are switched off";
    else if (a->persist.id == id && a->persist.attempts >= (uint8_t)IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX)
        what = "not applied: tried and did not run";
    else if (a->why != IOTDATA_NODE_OTA_OK)
        what = iotdata_node_ota_status_name(a->why);
    return snprintf(out, size, "firmware %s %s (%s), %s", h.app, h.semver, h.stamp, what);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Registers with the store. Call BEFORE iotdata_node_content_begin(), which is what offers the
   files.
 *
 * THE DURABLE RECORD SURVIVES THIS CALL, and that is not a convenience. Everything else here
 * belongs to one boot of one consumer and is rightly wiped; the record belongs to the DEVICE, and
 * describes an attempt made by the image that ran before this one. An application's state layer
 * decides when its blocks are registered and when they are restored -- on a node the restore is one
 * shot, early, and refuses a block offered after it -- so a begin() that zeroed the record would
 * force the store's mount to happen before the state load, or silently throw away the only evidence
 * that the last boot tried an update and failed. Both have been the bug: the relay mounts its store
 * after the radio, on purpose, and the counter it could not register is the ONLY thing standing
 * between a bad image and a node that re-applies it for ever. So the record is preserved across
 * begin, the order no longer matters, and apply_record() may be registered and loaded whenever the
 * application's own state layer requires.
 *
 * `my_stamp` is this build's own, and is told ONCE here rather than passed to every call that needs
 * it: it cannot change while the process runs, and a caller asked for it three times could give
 * three different answers. The three-way verdict in THE ATTEMPT COUNTER turns on comparing it with
 * what was recorded, so a build that cannot say when it was made cannot take part -- pass NULL and
 * the counter degrades to counting attempts without ever clearing one, which fails safe by refusing
 * the bundle once the limit is reached. It must outlive the consumer, which a literal from the
 * build system does. */
static inline bool iotdata_node_content_ota_apply_begin(iotdata_node_ota_apply_t *const a, iotdata_node_content_t *const store, const ota_record_t *const record, const iotdata_node_ota_device_t *const device, const ota_slot_t *const slot,
                                                        const ota_hash_t *const hash, const ota_codec_t *const codec, const ota_patch_t *const patch, const char *const my_stamp, const bool enabled) {
    if (a == NULL || store == NULL || slot == NULL)
        return false;
    const iotdata_node_ota_apply_state_t persist = a->persist; /* the device's, not this boot's -- see above */
    *a = (iotdata_node_ota_apply_t){ 0 };
    a->persist = persist;
    a->store = store;
    a->record = record;
    a->device = device;
    a->slot = slot;
    a->hash = hash;
    a->codec = codec; /* NULL is a legitimate answer: a build that only ever takes CODEC_NONE */
    a->patch = patch; /* and so is this: a node that takes whole images and no deltas */
    a->enabled = enabled;
    a->stamp = my_stamp;
    a->phase = IOTDATA_NODE_OTA_APPLY_IDLE;
    a->consumer = (const iotdata_node_content_consumer_t){
        .name = "firmware",
        .ctx = a,
        .claim = _iotdata_node_ota_apply_claim,
        .work = _iotdata_node_ota_apply_work,
        .describe = _iotdata_node_ota_apply_describe,
    };
    _iotdata_node_ota_apply = a;

    return iotdata_node_content_register(store, &a->consumer);
}

/* WHAT THE LAST BOOT LEFT BEHIND, in words, for the banner -- and the verdict that goes with it.
   Returns true when an attempt was in flight, so a caller knows whether the line is worth printing.
 *
 * Reading this does not change anything. The record is cleared by _confirmed() and only there,
 * because an image that cleared it and then rolled back would be applied all over again. */
static inline bool iotdata_node_content_ota_apply_pending(const iotdata_node_ota_apply_t *const a, char *const out, const size_t size) {
    if (a == NULL || a->persist.attempts == 0u || out == NULL || size == 0u)
        return false;
    const bool mine = (a->stamp != NULL && strcmp(a->persist.stamp, a->stamp) == 0);
    if (mine)
        (void)snprintf(out, size, "firmware: %06" PRIx32 " attempt %u BOOTED (%s) -- awaiting confirmation", a->persist.id, (unsigned)a->persist.attempts, a->persist.stamp);
    else if (a->persist.attempts >= (uint8_t)IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX)
        (void)snprintf(out, size, "firmware: %06" PRIx32 " attempt %u did NOT run (%s) -- giving up on it", a->persist.id, (unsigned)a->persist.attempts, a->persist.stamp);
    else
        (void)snprintf(out, size, "firmware: %06" PRIx32 " attempt %u did NOT run (%s) -- will try again", a->persist.id, (unsigned)a->persist.attempts, a->persist.stamp);
    return true;
}

/* WHERE THE CONSUMER HAS GOT TO, in lines an operator can read. Everything here was previously
   visible only in the boot log: the attempt record was reported once, at startup, and the phase not
   at all -- so watching an update meant watching a console at the right moment, and asking a node
   that had been up for a week was impossible. One line of state, plus the record's own verdict when
   there is one.
 *
 * THE ANSWER IS A LINE AT A TIME, through the same emit the rest of the store uses, so `content-ota`
 * typed at a console answers the person who typed it while the identical command off the air goes to
 * the log. See WHERE ITS WORDS GO in iotdata_node_content.h. */
static inline void iotdata_node_content_ota_apply_report(const iotdata_node_ota_apply_t *const a, char *const buf, const size_t size, void (*const say)(const char *line)) {
    if (a == NULL || buf == NULL || size == 0u || say == NULL)
        return;
    const char *phase = "idle";
    if (a->phase == IOTDATA_NODE_OTA_APPLY_WRITING)
        phase = "writing";
    else if (a->phase == IOTDATA_NODE_OTA_APPLY_FAILED)
        phase = "failed";

    if (a->phase == IOTDATA_NODE_OTA_APPLY_WRITING)
        (void)snprintf(buf, size, "firmware: %s, updates %s -- %06" PRIx32 " %s %" PRIu32 " of %" PRIu32 " payload, %" PRIu32 " of %" PRIu32 " image", phase, a->enabled ? "on" : "OFF", a->id, iotdata_node_ota_codec_name(a->head.codec),
                       a->off, a->head.payload_size, a->out, a->head.output_size);
    else
        (void)snprintf(buf, size, "firmware: %s, updates %s -- %s", phase, a->enabled ? "on" : "OFF", (a->why == IOTDATA_NODE_OTA_OK) ? "nothing to say" : iotdata_node_ota_status_name(a->why));
    say(buf);

    if (a->refused != 0u) {
        (void)snprintf(buf, size, "firmware: %06" PRIx32 " will not be tried again before a restart", a->refused);
        say(buf);
    }
    if (iotdata_node_content_ota_apply_pending(a, buf, size))
        say(buf);
    (void)snprintf(buf, size, "firmware: claimed=%" PRIu32 " written=%" PRIu32 " applied=%" PRIu32 " rejected=%" PRIu32, a->stat_claimed, a->stat_written, a->stat_applied, a->stat_rejected);
    say(buf);
}

/* THE IMAGE IN FLIGHT HAS BEEN ABANDONED ON PURPOSE -- call this immediately before a deliberate
   rollback, and nowhere else.
 *
 * WHY IT HAS TO EXIST. The attempt counter learns what happened by comparing the stamp it recorded
   against the one that came back, and a rollback is indistinguishable from a crash by that test:
   both end with the old image running and the new one not. So a node rolled back by hand reported
   "attempt 1 did NOT run -- will try again", and then did -- re-applying, on the spot, the very
   image the operator had just rejected. Observed on a relay: rollback, boot, re-apply, within two
   seconds. The node and the person fought, and the node had the stamina.
 *
 * So the operator's decision is written into the record as a verdict rather than left to be
   inferred: the attempt is marked spent, and the bundle is refused from then on exactly as one that
   had used up its three goes. It stays in the store, `content-ota` says why, and
   iotdata_node_content_ota_apply_retry() is how somebody who changes their mind arms it again. */
static inline void iotdata_node_content_ota_apply_abandon(iotdata_node_ota_apply_t *const a) {
    if (a == NULL || a->persist.id == 0u)
        return; /* nothing was ever applied here: there is no bundle to hold responsible */
    a->persist.attempts = (uint8_t)IOTDATA_NODE_OTA_APPLY_ATTEMPTS_MAX;
    a->refused = a->persist.id;
    if (a->record != NULL && a->record->save != NULL)
        (void)a->record->save(a->record->ctx); /* synchronously: the caller is about to reboot */
}

/* TRY THIS ONE NOW, because an operator has decided to. Two things stand between a held bundle and
   an attempt, and this clears exactly those two: the THIS-BOOT refusal, and the attempt limit for
   that id. Both exist to stop a node looping unattended, and neither is an argument against a person
   who has looked at the node and chosen to retry.
 *
 * WHAT IT DOES NOT CLEAR IS THE ENABLE. That row is a fleet decision -- the brake you reach for when
   a bad bundle is already on a hundred nodes -- and a verb that quietly overrode it would leave the
   brake meaning nothing. A node with updates switched off says so and stays put.
 *
 * It does not apply anything itself either. It makes the bundle takeable again and lets the ordinary
   re-offer do the work, so there is still exactly one path into the slot. */
static inline bool iotdata_node_content_ota_apply_retry(iotdata_node_ota_apply_t *const a, const uint32_t id) {
    if (a == NULL || !a->enabled)
        return false;
    if (a->phase == IOTDATA_NODE_OTA_APPLY_WRITING)
        return false; /* one at a time, and one is already going */
    if (a->refused == id)
        a->refused = 0u;
    if (a->persist.id == id) {
        a->persist = (iotdata_node_ota_apply_state_t){ 0 };
        if (a->record != NULL && a->record->save != NULL)
            (void)a->record->save(a->record->ctx);
    }
    if (a->phase == IOTDATA_NODE_OTA_APPLY_FAILED)
        a->phase = IOTDATA_NODE_OTA_APPLY_IDLE;
    a->why = IOTDATA_NODE_OTA_OK;
    /* and asked for again at the next tick, not at the next cadence: a minute of nothing after an
       operator types this is indistinguishable from the command having been dropped. */
    (void)iotdata_node_content_reoffer(a->store, id);
    return true;
}

/* The bootloader's probation has been confirmed -- call this on FIRMWARE_EVENT_CONFIRMED, and only
   there. THIS is where an attempt stops being in flight, because this is the first moment the new
   image cannot be taken away again. */
static inline void iotdata_node_content_ota_apply_confirmed(iotdata_node_ota_apply_t *const a) {
    if (a == NULL || a->persist.attempts == 0u)
        return;
    if (a->stamp == NULL || strcmp(a->persist.stamp, a->stamp) != 0)
        return; /* confirmed, but not the image that was attempted -- a cable flash, say. Leave it. */
    /* THE COUNT GOES, THE IDENTITY STAYS. Zeroing the whole record was tidier and lost the one fact
       a later rollback needs: WHICH bundle produced the image now running. Without it a node rolled
       back after a successful confirmation had nothing to condemn, so the consumer saw a held bundle
       newer than what was running, applied it again, and undid the rollback -- observed on a relay,
       twice in a row. attempts == 0 is what "nothing in flight" means, and is still what silences
       the boot report; the id and stamp are a memory, not a claim. */
    a->persist.attempts = 0u;
    if (a->record != NULL && a->record->save != NULL)
        (void)a->record->save(a->record->ctx);
}

/* THE DURABLE BLOCK, for an application to hand to whatever it keeps state in -- which on a device
   is iotdata_node_state_insert(..., record, size, defaults). Handed out rather than reached into,
   so the only thing an application knows about the record is how big it is and where it lives.
 *
 * Safe to ask for before begin(): it answers from the address of a field, not from anything begin()
 * sets up, so an application whose state layer wants every block registered early can register this
 * one early and still begin the consumer later. begin() keeps what was restored into it. */
static inline void *iotdata_node_content_ota_apply_record(iotdata_node_ota_apply_t *const a, size_t *const size) {
    if (a == NULL)
        return NULL;
    if (size != NULL)
        *size = sizeof(a->persist);
    return &a->persist;
}

/* Accepting updates, or not. Separate from the store's own enable: a node may be perfectly willing
   to HOLD a bundle for a neighbour and unwilling to run one itself. */
static inline void iotdata_node_content_ota_apply_enable(iotdata_node_ota_apply_t *const a, const bool on) {
    if (a != NULL)
        a->enabled = on;
}

static inline bool iotdata_node_content_ota_apply_busy(const iotdata_node_ota_apply_t *const a) {
    return a != NULL && a->phase == IOTDATA_NODE_OTA_APPLY_WRITING;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_OTA_H */
