//
// test_node_content_ota.c - the OTA blob header: encode, decode, and every refusal.
//
// Two jobs here. The first is ordinary: build a header with the kvr writers, read it back with
// iotdata_ota_parse, and walk iotdata_ota_check through each way it can say no.
//
// The second is the reason this test exists at all. The format has TWO implementations in TWO
// languages -- this header decodes it, iotdata-common/tools/ota-bundle encodes it -- and no device
// client yet to notice when they disagree. So the last test reads the bundler's own source and
// asserts its key numbers, magic, hash length and enum values match the defines here. A renumbered
// key is otherwise silent until a field device reads a blob wrong.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "iotdata_node_content_ota.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static const uint8_t HASH_T[IOTDATA_OTA_HASH_LEN] = { 0xe0, 0xd4, 0x39, 0x9c, 0xb1, 0xd8, 0x82, 0xbb, 0x95, 0x57, 0x80, 0xbf, 0xbe, 0x18, 0xc9, 0x5e };
static const uint8_t HASH_B[IOTDATA_OTA_HASH_LEN] = { 0xed, 0x96, 0x31, 0x5d, 0x9f, 0x58, 0xea, 0x50, 0x82, 0x2c, 0x70, 0xfc, 0x40, 0xf7, 0x85, 0xea };

/* Build a header the way ota-bundle does: magic, kvr pairs in order, END, then payload. */
static size_t build(uint8_t *const buf, const size_t size, const uint8_t form, const uint8_t codec, const uint8_t flags, const bool with_base, const char *const stamp, const uint32_t blmin,
                    const uint32_t outsize) {
    memcpy(buf, IOTDATA_OTA_MAGIC, IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf + IOTDATA_OTA_MAGIC_LEN, size - IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_OTA_KEY_APP, "relay");
    iotdata_kvr_add_str(&kv, IOTDATA_OTA_KEY_SEMVER, "0.9.9");
    iotdata_kvr_add_str(&kv, IOTDATA_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_OTA_KEY_FORM, form);
    iotdata_kvr_add_u8(&kv, IOTDATA_OTA_KEY_CODEC, codec);
    iotdata_kvr_add_u8(&kv, IOTDATA_OTA_KEY_FLAGS, flags);
    iotdata_kvr_add_u32(&kv, IOTDATA_OTA_KEY_BOOTLOADER_MIN, blmin);
    iotdata_kvr_add_u32(&kv, IOTDATA_OTA_KEY_OUTPUT_SIZE, outsize);
    iotdata_kvr_add(&kv, IOTDATA_OTA_KEY_TARGET_HASH, HASH_T, IOTDATA_OTA_HASH_LEN);
    if (with_base)
        iotdata_kvr_add(&kv, IOTDATA_OTA_KEY_BASE_HASH, HASH_B, IOTDATA_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_OTA_KEY_END, NULL, 0);
    return (kv.overflow) ? 0u : IOTDATA_OTA_MAGIC_LEN + kv.len;
}

static iotdata_ota_device_t a_relay_on_098(void) {
    return (iotdata_ota_device_t){ .app = "relay", .stamp = "202610070900", .bootloader = 1, .slot_size = 0x100000, .base_hash = HASH_B, .codec_deflate = true };
}

/* ------------------------------------------------------------------------------------------- */

static void test_roundtrip(void) {
    printf("\na header survives the trip, and the payload starts where it says\n");
    uint8_t buf[512];
    const size_t n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_DELTA, IOTDATA_OTA_CODEC_DEFLATE, 0u, true, "202610071014", 1u, 216688u);
    CHECK(n > 0, "built without overflow");
    iotdata_ota_header_t h;
    CHECK(iotdata_ota_parse(buf, n, &h) == IOTDATA_OTA_OK, "parses");
    CHECK(strcmp(h.app, "relay") == 0, "app");
    CHECK(strcmp(h.semver, "0.9.9") == 0, "semver");
    CHECK(strcmp(h.stamp, "202610071014") == 0, "stamp");
    CHECK(h.form == IOTDATA_OTA_FORM_DELTA, "form");
    CHECK(h.codec == IOTDATA_OTA_CODEC_DEFLATE, "codec");
    CHECK(h.bootloader_min == 1u, "bootloader_min");
    CHECK(h.output_size == 216688u, "output_size");
    CHECK(h.has_target_hash && memcmp(h.target_hash, HASH_T, IOTDATA_OTA_HASH_LEN) == 0, "target hash");
    CHECK(h.has_base_hash && memcmp(h.base_hash, HASH_B, IOTDATA_OTA_HASH_LEN) == 0, "base hash");
    /* the payload begins at the byte after END, which is what makes the header variable length */
    CHECK(h.payload_offset == n, "payload starts right after END");

    const size_t full = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 216688u);
    CHECK(iotdata_ota_parse(buf, full, &h) == IOTDATA_OTA_OK, "a full image parses");
    CHECK(!h.has_base_hash, "and carries no base hash");
    CHECK(full < n, "a full image's header is shorter than a delta's");
}

static void test_framing_refusals(void) {
    printf("\nframing that is not ours, or not finished\n");
    iotdata_ota_header_t h;
    const uint8_t nope[] = { 'N', 'O', 'P', 'E', 0, 0 };
    CHECK(iotdata_ota_parse(nope, sizeof(nope), &h) == IOTDATA_OTA_ERR_MAGIC, "wrong magic");
    CHECK(iotdata_ota_parse(NULL, 99, &h) == IOTDATA_OTA_ERR_TRUNCATED, "no buffer");
    const uint8_t stub[] = { 'I', 'O', 'T', 'A' };
    CHECK(iotdata_ota_parse(stub, sizeof(stub), &h) == IOTDATA_OTA_ERR_TRUNCATED, "magic alone");

    uint8_t buf[512];
    size_t n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 1024u);
    /* chop the END pair off: a reader must not treat running out of bytes as a finished header */
    CHECK(iotdata_ota_parse(buf, n - 2u, &h) == IOTDATA_OTA_ERR_NO_END, "no END sentinel");
    /* END must carry a zero length -- that is what separates it from a stray pair */
    buf[n - 1u] = 4u;
    CHECK(iotdata_ota_parse(buf, n, &h) == IOTDATA_OTA_ERR_NO_END, "END with a length is not an END");
}

static void test_unknown_keys_are_skipped(void) {
    printf("\nan unknown key does not break an older reader -- the whole point of kvr\n");
    uint8_t buf[512];
    memcpy(buf, IOTDATA_OTA_MAGIC, IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf + IOTDATA_OTA_MAGIC_LEN, sizeof(buf) - IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_OTA_KEY_APP, "relay");
    iotdata_kvr_add_u32(&kv, 0x7Eu, 0xdeadbeefu);   /* a system key from a newer release    */
    iotdata_kvr_add_str(&kv, 0xFEu, "proprietary"); /* and a proprietary one                */
    iotdata_kvr_add_u8(&kv, IOTDATA_OTA_KEY_FORM, IOTDATA_OTA_FORM_FULL);
    iotdata_kvr_add_u32(&kv, IOTDATA_OTA_KEY_OUTPUT_SIZE, 4096u);
    iotdata_kvr_add(&kv, IOTDATA_OTA_KEY_END, NULL, 0);
    const size_t n = IOTDATA_OTA_MAGIC_LEN + kv.len;
    iotdata_ota_header_t h;
    CHECK(iotdata_ota_parse(buf, n, &h) == IOTDATA_OTA_OK, "parses past both unknown keys");
    CHECK(strcmp(h.app, "relay") == 0, "a field before them still read");
    CHECK(h.output_size == 4096u, "a field after them still read");

    /* A value of unexpected width degrades that field rather than the blob (iotdata_kvr_* returns
       the caller's default), which is what lets a key change width without a flag day. */
    uint8_t odd[512];
    memcpy(odd, IOTDATA_OTA_MAGIC, IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_init(&kv, odd + IOTDATA_OTA_MAGIC_LEN, sizeof(odd) - IOTDATA_OTA_MAGIC_LEN);
    iotdata_kvr_add_u8(&kv, IOTDATA_OTA_KEY_OUTPUT_SIZE, 7u); /* u8 where u32 is expected */
    iotdata_kvr_add(&kv, IOTDATA_OTA_KEY_END, NULL, 0);
    CHECK(iotdata_ota_parse(odd, IOTDATA_OTA_MAGIC_LEN + kv.len, &h) == IOTDATA_OTA_OK, "a mis-width value does not fail the parse");
}

static void test_check_accepts(void) {
    printf("\nwhat a device accepts\n");
    uint8_t buf[512];
    iotdata_ota_header_t h;
    const size_t n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_DELTA, IOTDATA_OTA_CODEC_DEFLATE, 0u, true, "202610071014", 1u, 216688u);
    (void)iotdata_ota_parse(buf, n, &h);
    iotdata_ota_device_t d = a_relay_on_098();
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_OK, "the delta it was built for");
    d.base_hash = NULL; /* a device that cannot say what it runs is not blocked by that alone */
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_OK, "unknown running hash is not a refusal");
    d = a_relay_on_098();
    d.stamp = NULL;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_OK, "unknown running stamp is not a refusal");
}

static void test_check_refusals(void) {
    printf("\nand every way it says no\n");
    uint8_t buf[512];
    iotdata_ota_header_t h;
    const size_t n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_DELTA, IOTDATA_OTA_CODEC_DEFLATE, 0u, true, "202610071014", 1u, 216688u);
    (void)iotdata_ota_parse(buf, n, &h);

    iotdata_ota_device_t d = a_relay_on_098();
    d.app = "sensor";
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_APP, "a relay image offered to a sensor");

    d = a_relay_on_098();
    d.codec_deflate = false;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_CODEC, "a codec this build cannot decode");

    d = a_relay_on_098();
    d.bootloader = 0u;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_BOOTLOADER, "needs a newer bootloader");

    d = a_relay_on_098();
    d.slot_size = 1024u;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_SIZE, "will not fit the slot");

    d = a_relay_on_098();
    static const uint8_t other[IOTDATA_OTA_HASH_LEN] = { 9 };
    d.base_hash = other;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_BASE, "a delta against an image we are not running");

    /* BOOTLOADER_MIN is a FLOOR: newer than required must pass, or the fleet locks itself out. */
    d = a_relay_on_098();
    d.bootloader = 99u;
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_OK, "a newer bootloader than required is fine");

    /* a delta with no base at all is malformed, not merely incompatible */
    const size_t m = build(buf, sizeof(buf), IOTDATA_OTA_FORM_DELTA, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 216688u);
    (void)iotdata_ota_parse(buf, m, &h);
    d = a_relay_on_098();
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_NO_BASE, "a delta with no base hash");

    /* an unknown form or codec is refused rather than attempted */
    const size_t b = build(buf, sizeof(buf), 0x7Fu, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 4096u);
    (void)iotdata_ota_parse(buf, b, &h);
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_FORM, "an unknown form");
    const size_t c = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, 0x7Fu, 0u, false, "202610071014", 1u, 4096u);
    (void)iotdata_ota_parse(buf, c, &h);
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_CODEC, "an unknown codec");
}

static void test_newness(void) {
    printf("\nnewness is the only refusal a flag may override\n");
    uint8_t buf[512];
    iotdata_ota_header_t h;
    iotdata_ota_device_t d = a_relay_on_098();
    d.stamp = "202610081200"; /* we are running something NEWER than the offer */

    size_t n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 4096u);
    (void)iotdata_ota_parse(buf, n, &h);
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_OLDER, "older than ours, unflagged");

    n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, IOTDATA_OTA_CODEC_NONE, IOTDATA_OTA_FLAG_DOWNGRADE, false, "202610071014", 1u, 4096u);
    (void)iotdata_ota_parse(buf, n, &h);
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_OK, "older than ours, flagged as intended");

    /* the same stamp is not an upgrade either -- a rebuild at the same minute is not a new release */
    d.stamp = "202610071014";
    n = build(buf, sizeof(buf), IOTDATA_OTA_FORM_FULL, IOTDATA_OTA_CODEC_NONE, 0u, false, "202610071014", 1u, 4096u);
    (void)iotdata_ota_parse(buf, n, &h);
    CHECK(iotdata_ota_check(&h, &d) == IOTDATA_OTA_ERR_OLDER, "the same stamp is not newer");

    /* fixed-width yyyymmddhhmm orders chronologically under strcmp, which is why it stays a string */
    CHECK(strcmp("202609301200", "202610010900") < 0, "the stamp orders across a month boundary");
    CHECK(strcmp("202512312359", "202601010000") < 0, "and across a year boundary");
}

/* ------------------------------------------------------------------------------------------- */
/* THE CROSS-LANGUAGE CHECK. ota-bundle encodes what this header decodes, so its constants are read
   out of its source and compared. Nothing else notices if the two drift apart. */

static const char *bundler(void) {
    static char buf[80000];
    static const char *paths[] = { "../tools/ota-bundle/ota-bundle", "../../iotdata-common/tools/ota-bundle/ota-bundle" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "r");
        if (f != NULL) {
            const size_t n = fread(buf, 1, sizeof(buf) - 1u, f);
            fclose(f);
            buf[n] = '\0';
            return buf;
        }
    }
    return NULL;
}

/* Find "<table> = {" then "<name>: 0xNN" within it. */
static long js_member(const char *const src, const char *const table, const char *const name) {
    char pat[64];
    (void)snprintf(pat, sizeof(pat), "%s = {", table);
    const char *t = strstr(src, pat);
    if (t == NULL)
        return -1;
    const char *end = strchr(t, '}');
    (void)snprintf(pat, sizeof(pat), "%s:", name);
    const char *m = strstr(t, pat);
    if (m == NULL || (end != NULL && m > end))
        return -1;
    return strtol(m + strlen(pat), NULL, 0);
}

static void test_bundler_agrees(void) {
    printf("\nthe bundler encodes what this header decodes\n");
    const char *src = bundler();
    if (src == NULL) {
        printf("  SKIP: ota-bundle not found from here\n");
        return;
    }
    CHECK(strstr(src, "MAGIC = '" IOTDATA_OTA_MAGIC "'") != NULL, "magic agrees");
    CHECK(js_member(src, "const", "HASH_LEN") == IOTDATA_OTA_HASH_LEN || strstr(src, "HASH_LEN = 16") != NULL, "hash length agrees");

#define AGREE(table, name, want) CHECK(js_member(src, table, #name) == (long)(want), "KEY/enum " #name " agrees")
    AGREE("KEY", END, IOTDATA_OTA_KEY_END);
    AGREE("KEY", APP, IOTDATA_OTA_KEY_APP);
    AGREE("KEY", SEMVER, IOTDATA_OTA_KEY_SEMVER);
    AGREE("KEY", STAMP, IOTDATA_OTA_KEY_STAMP);
    AGREE("KEY", FORM, IOTDATA_OTA_KEY_FORM);
    AGREE("KEY", CODEC, IOTDATA_OTA_KEY_CODEC);
    AGREE("KEY", FLAGS, IOTDATA_OTA_KEY_FLAGS);
    AGREE("KEY", BOOTLOADER_MIN, IOTDATA_OTA_KEY_BOOTLOADER_MIN);
    AGREE("KEY", OUTPUT_SIZE, IOTDATA_OTA_KEY_OUTPUT_SIZE);
    AGREE("KEY", TARGET_HASH, IOTDATA_OTA_KEY_TARGET_HASH);
    AGREE("KEY", BASE_HASH, IOTDATA_OTA_KEY_BASE_HASH);
    AGREE("FORM", full, IOTDATA_OTA_FORM_FULL);
    AGREE("FORM", delta, IOTDATA_OTA_FORM_DELTA);
    AGREE("CODEC", none, IOTDATA_OTA_CODEC_NONE);
    AGREE("CODEC", deflate, IOTDATA_OTA_CODEC_DEFLATE);
    AGREE("CODEC", heatshrink, IOTDATA_OTA_CODEC_HEATSHRINK);
#undef AGREE
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_content_ota\n");
    test_roundtrip();
    test_framing_refusals();
    test_unknown_keys_are_skipped();
    test_check_accepts();
    test_check_refusals();
    test_newness();
    test_bundler_agrees();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
