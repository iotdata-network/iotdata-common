// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_version.c - host tests for iotdata_node_version.h.
//
// The grammars here are PARSED by anything reading a fleet, so what matters is not that a string
// is produced but that it cannot be produced malformed: a separator smuggled in by a detected
// value, an identity truncated, a capability entry that is not a whole number of 16-bit words.
// Each of those was a real defect found while wiring the gateway.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "iotdata_node_version.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

static void test_caps_entries(void) {
    printf("capabilities: one 16-bit entry per category, OR-merged\n");
    iotdata_version_caps_t c;
    memset(&c, 0, sizeof(c));

    CHECK(iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_RADIO, IOTDATA_VERSION_RADIO_E22_DIP), "added");
    CHECK(c.count == 1, "one entry");
    CHECK(iotdata_version_caps_get(&c, IOTDATA_VERSION_CAP_RADIO) == IOTDATA_VERSION_RADIO_E22_DIP, "reads back");

    /* two radios is a real configuration, not an error -- which is why the value is a mask */
    CHECK(iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_RADIO, IOTDATA_VERSION_RADIO_SX1302), "second radio");
    CHECK(c.count == 1, "same category, still ONE entry: OR-merged rather than duplicated");
    CHECK(iotdata_version_caps_get(&c, IOTDATA_VERSION_CAP_RADIO) == (IOTDATA_VERSION_RADIO_E22_DIP | IOTDATA_VERSION_RADIO_SX1302), "both bits set");

    /* a zero mask is not "category present, empty" -- there is nothing to say, so say nothing */
    CHECK(!iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_DISPLAY, 0), "zero mask refused");
    CHECK(c.count == 1, "and nothing was added");
    /* twelve bits is the field; anything above it would be silently eaten by the key nibble */
    CHECK(!iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_DISPLAY, 0x1000), "a mask past 12 bits is refused");
    CHECK(!iotdata_version_caps_add(&c, 0x10, 0x001), "a key past 4 bits is refused");

    CHECK(strcmp(iotdata_version_cap_name(IOTDATA_VERSION_CAP_PROPRIETARY), "proprietary") == 0, "the high bit means proprietary, as in every other key space");
    CHECK(strcmp(iotdata_version_cap_name(0x7), "reserved") == 0, "the one unassigned public key says so");
}

static void test_caps_wire(void) {
    printf("capabilities: the wire layout, big-endian, whole words only\n");
    iotdata_version_caps_t c;
    memset(&c, 0, sizeof(c));
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_DISPLAY, 0xA2B);

    uint8_t buf[IOTDATA_VERSION_CAP_COUNT * 2];
    const int n = iotdata_version_caps_pack(&c, buf, sizeof(buf));
    CHECK(n == 2, "one entry is two bytes");
    /* [key:4][mask:12] big-endian, so this reads out of a hex dump as key 1, mask A2B */
    CHECK(buf[0] == 0x1A && buf[1] == 0x2B, "1A 2B: nibble-aligned and readable");

    iotdata_version_caps_t r;
    CHECK(iotdata_version_caps_parse(buf, (size_t)n, &r), "parsed");
    CHECK(r.count == 1 && iotdata_version_caps_get(&r, IOTDATA_VERSION_CAP_DISPLAY) == 0xA2B, "round trip");

    /* an odd length is a malformed value, not a short read to tolerate */
    const uint8_t odd[3] = { 0x1A, 0x2B, 0x30 };
    CHECK(!iotdata_version_caps_parse(odd, sizeof(odd), &r), "an odd length is rejected");
    CHECK(r.count == 0, "and yields nothing rather than a partial answer");
    CHECK(iotdata_version_caps_parse(buf, 0, &r) && r.count == 0, "empty is valid and empty");

    /* a repeated key on the wire OR-merges, which is the escape hatch if a category ever outgrows
       twelve bits -- the format does not have to change for it */
    const uint8_t twice[4] = { 0x10, 0x01, 0x10, 0x02 };
    CHECK(iotdata_version_caps_parse(twice, sizeof(twice), &r), "parsed");
    CHECK(r.count == 1 && iotdata_version_caps_get(&r, IOTDATA_VERSION_CAP_DISPLAY) == 0x003, "a repeated key merged");

    /* a full table must fit the pack buffer the header sizes for it */
    memset(&c, 0, sizeof(c));
    for (uint8_t k = 0; k < IOTDATA_VERSION_CAP_COUNT; k++)
        CHECK(iotdata_version_caps_add(&c, k, 0xFFF), "filled a category");
    CHECK(c.count == IOTDATA_VERSION_CAP_COUNT, "all sixteen");
    CHECK(iotdata_version_caps_pack(&c, buf, sizeof(buf)) == IOTDATA_VERSION_CAP_COUNT * 2, "and packs whole");
    CHECK(iotdata_version_caps_pack(&c, buf, 4) == -1, "a short buffer is refused, not half-filled");
}

static void test_caps_render(void) {
    printf("capabilities: rendering\n");
    iotdata_version_caps_t c;
    char s[IOTDATA_VERSION_CAPS_STR_MAX + 1];
    memset(&c, 0, sizeof(c));
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_FEATURES, IOTDATA_VERSION_FEATURE_MESH | IOTDATA_VERSION_FEATURE_OTA);
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_RADIO, IOTDATA_VERSION_RADIO_E22_USB);
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_DISPLAY, IOTDATA_VERSION_DISPLAY_ILI9488);
    /* FEATURES renders bare -- "mesh" reads better than "features=mesh", and it is the only
       category where the category name adds nothing */
    CHECK(strcmp(iotdata_version_caps_str(&c, s, sizeof(s)), "mesh,ota,radio/e22-usb,display/ili9488") == 0, "named bits, features bare, '/' for category/member");

    /* an unnamed bit says WHICH bit rather than guessing: a proprietary category's meaning is not
       ours to know, and neither is a bit added by a newer build */
    memset(&c, 0, sizeof(c));
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_PROPRIETARY, 0x004);
    CHECK(strcmp(iotdata_version_caps_str(&c, s, sizeof(s)), "proprietary/0x004") == 0, "unnamed renders as its bit");

    /* a buffer that runs out leaves a valid string, not a half-written entry */
    memset(&c, 0, sizeof(c));
    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_RADIO, 0xFFF);
    char tiny[12];
    memset(tiny, 0x7F, sizeof(tiny));
    (void)iotdata_version_caps_str(&c, tiny, sizeof(tiny));
    CHECK(memchr(tiny, '\0', sizeof(tiny)) != NULL, "terminated inside the buffer");
}

static void test_grammars(void) {
    printf("grammars: what a reader is entitled to assume\n");
    char sw[IOTDATA_VERSION_SOFTWARE_MAX + 1], hw[IOTDATA_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_VERSION_FIRMWARE_MAX + 1], sn[IOTDATA_VERSION_SERIAL_MAX + 1];

    /* a stamp is twelve digits or it is not a stamp -- see _iotdata_version_stamp_valid */
    CHECK(_iotdata_version_stamp_valid("202609121706"), "twelve digits is a stamp");
    CHECK(!_iotdata_version_stamp_valid(""), "empty is NOT -- a failed `date` in a Makefile passes this");
    CHECK(!_iotdata_version_stamp_valid("20260912170"), "eleven digits is not");
    CHECK(!_iotdata_version_stamp_valid("2026091217061"), "thirteen is not");
    CHECK(!_iotdata_version_stamp_valid("2026-09-12ab"), "and neither is twelve of the wrong characters");
    CHECK(!_iotdata_version_stamp_valid(NULL), "nor NULL");
    {   /* whatever the build passed, what comes out is a stamp or the explicit placeholder */
        char st[IOTDATA_VERSION_STAMP_LEN + 1];
        (void)_iotdata_version_stamp(st, sizeof(st));
        CHECK(_iotdata_version_stamp_valid(st), "the stamp actually emitted is always well-formed");
        CHECK(iotdata_version_stamp_is_real() == (strcmp(st, IOTDATA_VERSION_STAMP_NONE) != 0), "and is_real agrees with it");
    }

    /* app/semver/stamp -- exactly two separators, so a reader can split without counting */
    (void)iotdata_version_software(sw, sizeof(sw));
    int slashes = 0;
    for (const char *p = sw; *p != '\0'; p++)
        if (*p == '/')
            slashes++;
    CHECK(slashes == 2, "software has exactly two '/'");
    CHECK(strncmp(sw, IOTDATA_VERSION_APP "/", strlen(IOTDATA_VERSION_APP) + 1) == 0, "and starts with the app name");

    /* board/arch, at most one separator; the arch alone is a valid answer when there is no board */
    (void)iotdata_version_hardware(hw, sizeof(hw));
    slashes = 0;
    for (const char *p = hw; *p != '\0'; p++)
        if (*p == '/')
            slashes++;
    CHECK(slashes <= 1, "hardware has at most one '/'");
    CHECK(hw[0] != '\0', "and is never empty");

    /* THE BUG THIS CATCHES: `uname -r` is "6.12.96+deb13-amd64" on Debian, and '+' is this
       grammar's separator for the [+low] field -- carrying it through made the kernel parse as a
       bootloader that does not exist, and truncated at 24 chars as well. */
    (void)iotdata_version_firmware(fw, sizeof(fw));
    CHECK(fw[0] != '\0', "firmware is never empty");
    CHECK(strlen(fw) <= IOTDATA_VERSION_FIRMWARE_MAX, "and fits its field");
#if defined(__linux__)
    CHECK(strchr(fw, '+') == NULL, "no stray '+': packaging suffixes are not a bootloader");
    CHECK(strncmp(fw, "linux/", 6) == 0, "stack/version, as board/arch and app/semver/stamp are");
    for (const char *p = fw + 6; *p != '\0'; p++)
        CHECK((*p >= '0' && *p <= '9') || *p == '.', "and the version is digits and dots only");
#endif

    /* an identity must never be truncated: a cut machine-id is a different, possibly colliding id */
    (void)iotdata_version_serial(sn, sizeof(sn));
    CHECK(sn[0] != '\0', "serial is never empty");
    CHECK(strlen(sn) <= IOTDATA_VERSION_SERIAL_MAX, "and fits whole -- 32 hex of machine-id included");
    CHECK(strchr(sn, '/') == NULL && strchr(sn, '+') == NULL, "and carries no separator");
}

/* One sanitiser for foreign values, one writer for our own separators. The division is what makes
   it safe to build a grammar out of the very characters a value must not contain. */
static void test_append(void) {
    printf("sanitising: a detected value cannot smuggle in a separator\n");
    char out[16];
    size_t n = 0;
    out[0] = '\0';
    _iotdata_version_append(out, sizeof(out), &n, "a/b+c,d e");
    CHECK(strcmp(out, "a-b-c-d-e") == 0, "separators and spaces become '-'");

    n = 0;
    out[0] = '\0';
    _iotdata_version_append(out, sizeof(out), &n, "10MUS0P800");
    CHECK(strcmp(out, "10mus0p800") == 0, "and everything is lower cased: a DMI model is shouted");

    /* the separator writer puts in verbatim what append would have replaced */
    n = 0;
    out[0] = '\0';
    _iotdata_version_append(out, sizeof(out), &n, "linux");
    _iotdata_version_sep(out, sizeof(out), &n, '/');
    _iotdata_version_append(out, sizeof(out), &n, "6.12");
    CHECK(strcmp(out, "linux/6.12") == 0, "and a deliberate separator survives");

    char two[3];
    n = 0;
    two[0] = '\0';
    _iotdata_version_append(two, sizeof(two), &n, "abcdef");
    CHECK(strcmp(two, "ab") == 0, "truncates to the buffer");
    _iotdata_version_sep(two, sizeof(two), &n, '/');
    CHECK(strcmp(two, "ab") == 0, "and a separator with no room is dropped, not written past the end");

    n = 0;
    out[0] = '\0';
    _iotdata_version_append(out, sizeof(out), &n, NULL);
    CHECK(strcmp(out, "") == 0, "NULL is the empty string, not a crash");
}

static void test_pack(void) {
    printf("the TLV: four keys always, capabilities when there are any\n");
    uint8_t buf[256];
    iotdata_kvr_t kv;
    iotdata_version_caps_t c;
    memset(&c, 0, sizeof(c));

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    CHECK(iotdata_version_pack(&kv, NULL) > 0, "packed without capabilities");
    /* walk the kvr: key, vlen, value... */
    int keys = 0;
    bool seen[5] = { false, false, false, false, false };
    for (size_t i = 0; i + 1 < kv.len;) {
        const uint8_t key = buf[i], vlen = buf[i + 1];
        if (key < 5)
            seen[key] = true;
        keys++;
        i += 2u + vlen;
    }
    CHECK(keys == 4, "four keys");
    CHECK(seen[IOTDATA_NODE_VERSION_HARDWARE] && seen[IOTDATA_NODE_VERSION_FIRMWARE] && seen[IOTDATA_NODE_VERSION_SOFTWARE] && seen[IOTDATA_NODE_VERSION_SERIAL], "hardware, firmware, software, serial");
    CHECK(!seen[IOTDATA_NODE_VERSION_CAPABILITIES], "and no capabilities key when there is nothing to declare");

    (void)iotdata_version_caps_add(&c, IOTDATA_VERSION_CAP_RADIO, IOTDATA_VERSION_RADIO_E22_DIP);
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    CHECK(iotdata_version_pack(&kv, &c) > 0, "packed with capabilities");
    bool caps_present = false;
    uint8_t caps_len = 0;
    for (size_t i = 0; i + 1 < kv.len;) {
        if (buf[i] == IOTDATA_NODE_VERSION_CAPABILITIES) {
            caps_present = true;
            caps_len = buf[i + 1];
        }
        i += 2u + buf[i + 1];
    }
    CHECK(caps_present && caps_len == 2, "capabilities present, one entry");

    /* the whole report has to fit a frame with room to spare, or a node cannot answer in one */
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    memset(&c, 0, sizeof(c));
    for (uint8_t k = 0; k < 6; k++)
        (void)iotdata_version_caps_add(&c, k, 0xFFF);
    (void)iotdata_version_pack(&kv, &c);
    printf("  a full report is %u bytes of kvr\n", (unsigned)kv.len);
    CHECK(kv.len < 200, "and fits one 240-byte frame with the header and TLV overhead");

    /* a buffer too small must fail rather than emit a half-built report */
    uint8_t small[8];
    iotdata_kvr_init(&kv, small, sizeof(small));
    CHECK(iotdata_version_pack(&kv, NULL) == -1, "a short buffer is an error");
}

/* The splitters exist so that no reader reimplements the grammars -- which is how they drift. */
static void test_splitters(void) {
    printf("grammar splitters: taking the values apart again\n");
    char a[32], b[32], c[32];

    CHECK(iotdata_version_software_split("relay/1.0.0/202609121743", a, sizeof(a), b, sizeof(b), c, sizeof(c)), "software split");
    CHECK(strcmp(a, "relay") == 0 && strcmp(b, "1.0.0") == 0 && strcmp(c, "202609121743") == 0, "app, semver, stamp");
    /* all three are required: an app name with no stamp says nothing about WHICH build it is */
    CHECK(!iotdata_version_software_split("relay/1.0.0", a, sizeof(a), b, sizeof(b), c, sizeof(c)), "a missing stamp is malformed, not partial");

    CHECK(iotdata_version_hardware_split("esp32c3/riscv32", a, sizeof(a), b, sizeof(b)), "hardware split");
    CHECK(strcmp(a, "esp32c3") == 0 && strcmp(b, "riscv32") == 0, "board, arch");
    /* one part IS the arch: a host reporting only its architecture is still telling the truth */
    CHECK(iotdata_version_hardware_split("x86_64", a, sizeof(a), b, sizeof(b)), "arch alone is valid");
    CHECK(a[0] == '\0' && strcmp(b, "x86_64") == 0, "and lands in arch, not board");

    CHECK(iotdata_version_firmware_split("linux/6.12.96", a, sizeof(a), b, sizeof(b), c, sizeof(c)), "firmware split");
    CHECK(strcmp(a, "linux") == 0 && strcmp(b, "6.12.96") == 0 && c[0] == '\0', "stack, version, no low half");
    CHECK(iotdata_version_firmware_split("idf/6.1+bl1", a, sizeof(a), b, sizeof(b), c, sizeof(c)), "with a bootloader");
    CHECK(strcmp(a, "idf") == 0 && strcmp(b, "6.1") == 0 && strcmp(c, "bl1") == 0, "and the low half is separated");

    /* what the detectors actually produce must survive its own splitter -- the round trip is the
       only thing that proves the grammar and the parser agree */
    char hw[IOTDATA_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_VERSION_FIRMWARE_MAX + 1], sw[IOTDATA_VERSION_SOFTWARE_MAX + 1];
    CHECK(iotdata_version_software_split(iotdata_version_software(sw, sizeof(sw)), a, sizeof(a), b, sizeof(b), c, sizeof(c)), "this build's software splits");
    CHECK(strcmp(a, IOTDATA_VERSION_APP) == 0, "back to the app name it was given");
    CHECK(iotdata_version_hardware_split(iotdata_version_hardware(hw, sizeof(hw)), a, sizeof(a), b, sizeof(b)), "this build's hardware splits");
    CHECK(iotdata_version_firmware_split(iotdata_version_firmware(fw, sizeof(fw)), a, sizeof(a), b, sizeof(b), c, sizeof(c)), "this build's firmware splits");

    /* a truncating buffer gives a short answer, not a refusal: a reader after the first field
       should not be defeated by a long third one */
    char tiny[4];
    CHECK(iotdata_version_part("abcdefgh/x", '/', 0, tiny, sizeof(tiny)) == 3, "truncated to the buffer");
    CHECK(strcmp(tiny, "abc") == 0, "and terminated");
    CHECK(iotdata_version_part("a/b", '/', 5, tiny, sizeof(tiny)) == -1, "no such part");
    CHECK(tiny[0] == '\0', "and the buffer is cleared, not left stale");
    CHECK(iotdata_version_part(NULL, '/', 0, tiny, sizeof(tiny)) == -1, "NULL is not a crash");
}

#if !defined(IOTDATA_NO_JSON)
static void test_json(void) {
    printf("json: the capability registry travels with the report\n");
    iotdata_version_caps_t caps, back;
    memset(&caps, 0, sizeof(caps));
    (void)iotdata_version_caps_add(&caps, IOTDATA_VERSION_CAP_RADIO, IOTDATA_VERSION_RADIO_E22_DIP | IOTDATA_VERSION_RADIO_SX1302);
    (void)iotdata_version_caps_add(&caps, IOTDATA_VERSION_CAP_FEATURES, IOTDATA_VERSION_FEATURE_MESH);

    cJSON *const o = iotdata_version_caps_to_json(&caps);
    CHECK(o != NULL, "encoded");
    char *const txt = cJSON_PrintUnformatted(o);
    CHECK(txt != NULL && strstr(txt, "radio/e22-dip,radio/sx1302,mesh") != NULL, "the names a human reads");
    CHECK(txt != NULL && strstr(txt, "\"category\":\"radio\"") != NULL, "and the raw entries a newer reader needs");
    free(txt);

    /* decoded from the ENTRIES, not the names -- decoding its own rendering would make the format
       depend on how it prints */
    CHECK(iotdata_version_caps_from_json(o, &back), "decoded");
    CHECK(back.count == caps.count, "same number of categories");
    CHECK(iotdata_version_caps_get(&back, IOTDATA_VERSION_CAP_RADIO) == iotdata_version_caps_get(&caps, IOTDATA_VERSION_CAP_RADIO), "radio mask round-trips, both bits");
    CHECK(iotdata_version_caps_get(&back, IOTDATA_VERSION_CAP_FEATURES) == IOTDATA_VERSION_FEATURE_MESH, "and features");
    cJSON_Delete(o);

    /* a category this build has never heard of is SKIPPED, not fatal: a newer node is allowed to
       declare things we cannot name */
    cJSON *const future = cJSON_Parse("{\"entries\":[{\"category\":\"quantum\",\"mask\":7},{\"category\":\"radio\",\"mask\":1}]}");
    CHECK(future != NULL, "parsed");
    CHECK(iotdata_version_caps_from_json(future, &back), "and accepted");
    CHECK(back.count == 1 && iotdata_version_caps_get(&back, IOTDATA_VERSION_CAP_RADIO) == 1, "keeping what it understands");
    cJSON_Delete(future);

    CHECK(iotdata_version_cap_key("radio") == IOTDATA_VERSION_CAP_RADIO, "names map back to keys");
    CHECK(iotdata_version_cap_key("nonesuch") == -1, "and an unknown name does not");

    /* the key hook: it claims capabilities and declines everything else */
    cJSON *const obj = cJSON_CreateObject();
    const uint8_t entry[2] = { 0x00, 0x01 };
    CHECK(iotdata_version_json_key(obj, "capabilities", IOTDATA_NODE_VERSION_CAPABILITIES, entry, 2), "claims capabilities");
    CHECK(!iotdata_version_json_key(obj, "software", IOTDATA_NODE_VERSION_SOFTWARE, entry, 2), "declines the rest, so a caller keeps its generic path");
    const uint8_t odd[3] = { 0, 1, 2 };
    CHECK(iotdata_version_json_key(obj, "bad", IOTDATA_NODE_VERSION_CAPABILITIES, odd, 3), "claims a malformed one too");
    char *const out = cJSON_PrintUnformatted(obj);
    CHECK(out != NULL && strstr(out, "\"bad\":\"malformed\"") != NULL, "and says so rather than guessing");
    free(out);
    cJSON_Delete(obj);
}
#endif

int main(void) {
    printf("iotdata_node_version: what a node says it is\n\n");
    test_caps_entries();
    test_caps_wire();
    test_caps_render();
    test_grammars();
    test_append();
    test_pack();
    test_splitters();
#if !defined(IOTDATA_NO_JSON)
    test_json();
#endif
    char v[IOTDATA_VERSION_STR_MAX + 1];
    printf("\n  this build: %s\n", iotdata_version_str(v, sizeof(v), NULL));
    printf("  stamp is real: %s\n", iotdata_version_stamp_is_real() ? "yes" : "no (unset)");
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
