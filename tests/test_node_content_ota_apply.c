//
// test_node_content_ota_apply.c - the OTA consumer, on a host, with no flash anywhere near it.
//
// THIS IS THE SUITE THE SEAMS EXIST FOR. The module's job is to decide things -- which bundles to
// take, what to do when one will not write, how many times to try before a bundle is poison -- and
// every one of those decisions is reachable here because the slot and the hash are vtables the test
// supplies. The scripted slot can be told to fail at begin, at write, at end or at activate, which
// is four failure paths that on a device would each need a deliberately broken bundle and a cable.
//
// THE HASH IS SCRIPTED RATHER THAN REAL, and that is the stronger test: what this module owes its
// caller is that it compares the payload digest against the header and refuses a mismatch, and
// proving that needs a digest the test controls, not a real sha256 it would have to collide.
//
// The ATTEMPT COUNTER is the reason the module exists in this shape, so most of the second half is
// about it: that it is written down BEFORE the slot is activated, that it survives into a new
// process, that the three-way "did it boot" answer is right in each case, and that a bundle which
// has used up its attempts is refused rather than retried for ever.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

/* iotdata.c for the kvr reader the OTA header parses with, and the variant it needs to compile --
   the same pair test_node_content_ota.c takes, and for the same reason. */
#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"

/* A SMALL CHUNK ON PURPOSE. The real one is 4 KB, which would move any payload this suite can
   afford to build in a single call -- and the whole point of work() being ticked is that it does
   NOT. At 256 a 1500-byte image takes six passes, so the resumption between them is under test
   rather than assumed. */
#define IOTDATA_NODE_OTA_APPLY_CHUNK 256u

/* CRC-64 LIVES IN d_common.h, which needs a platform under it for its timer and delay helpers --
   so the host platform goes in first, exactly as the gateway does it. This suite calls neither;
   it is here for the checksum iotdata_node_content.h sums an upload with. */
#include "d_platform_linux.h"
#include "d_common.h"
#include "d_module_filestore.h" /* the seam: content.h is written against it */
#include "iotdata_node_content.h"
#include "d_module_otaslot.h"
#include "iotdata_node_content_ota.h"
#include "d_module_filestore_ram.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define R_MINE   IOTDATA_NODE_CONTENT_R_MINE
#define R_DELETE IOTDATA_NODE_CONTENT_R_DELETE
#define R_WORK   IOTDATA_NODE_CONTENT_R_WORK
#define R_REBOOT IOTDATA_NODE_CONTENT_R_REBOOT

/* ------------------------------------------------------------------------------------------- */
/* THE SCRIPTED SLOT. Counts what it was asked and refuses whatever the test told it to.       */

typedef struct {
    uint8_t image[4096];
    uint32_t len, declared;
    bool open, ended, active;
    int fail_begin, fail_write, fail_end, fail_activate; /* >0: refuse the next N calls */
    int n_begin, n_write, n_end, n_activate, n_abort;
} fake_slot_t;

static fake_slot_t g_slot;

static bool fs_begin(void *ctx, const uint32_t size) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_begin++;
    if (s->fail_begin > 0) {
        s->fail_begin--;
        return false;
    }
    s->declared = size;
    s->len = 0u;
    s->open = true;
    s->ended = false;
    return true;
}
static int fs_write(void *ctx, const void *buf, const size_t len) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_write++;
    if (s->fail_write > 0) {
        s->fail_write--;
        return -1;
    }
    if (!s->open || s->len + len > sizeof(s->image))
        return -1;
    memcpy(&s->image[s->len], buf, len);
    s->len += (uint32_t)len;
    return (int)len;
}
static bool fs_end(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_end++;
    if (s->fail_end > 0) {
        s->fail_end--;
        return false;
    }
    s->ended = true;
    return true;
}
static bool fs_activate(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_activate++;
    if (s->fail_activate > 0) {
        s->fail_activate--;
        return false;
    }
    s->active = true;
    return true;
}
static void fs_abort(void *ctx) {
    fake_slot_t *const s = (fake_slot_t *)ctx;
    s->n_abort++;
    s->open = false;
    s->len = 0u;
}

static const ota_slot_t g_slot_vt = { .ctx = &g_slot, .begin = fs_begin, .write = fs_write, .end = fs_end, .activate = fs_activate, .abort = fs_abort };

/* ------------------------------------------------------------------------------------------- */
/* THE SCRIPTED HASH. A sum, not a digest: deterministic, and the test can poison it.          */

typedef struct {
    uint32_t acc;
    bool poison; /* return a digest that cannot match, to reach the mismatch path */
} fake_hash_t;

static fake_hash_t g_hash;

static void fh_begin(void *ctx) {
    ((fake_hash_t *)ctx)->acc = 0u;
}
static void fh_update(void *ctx, const void *buf, const size_t len) {
    fake_hash_t *const h = (fake_hash_t *)ctx;
    const uint8_t *const p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        h->acc = h->acc * 31u + p[i];
}
static void fh_final(void *ctx, uint8_t *const out, const size_t outlen) {
    fake_hash_t *const h = (fake_hash_t *)ctx;
    memset(out, 0, outlen);
    if (h->poison) {
        memset(out, 0xEE, outlen);
        return;
    }
    for (size_t i = 0; i < 4u && i < outlen; i++)
        out[i] = (uint8_t)(h->acc >> (8u * i));
}

static const ota_hash_t g_hash_vt = { .ctx = &g_hash, .begin = fh_begin, .update = fh_update, .final = fh_final };

/* What the scripted hash will say about a payload, so a bundle can carry a correct PAYLOAD_HASH. */
static void expected_hash(const uint8_t *const p, const uint32_t n, uint8_t *const out) {
    uint32_t acc = 0u;
    for (uint32_t i = 0; i < n; i++)
        acc = acc * 31u + p[i];
    memset(out, 0, IOTDATA_NODE_OTA_HASH_LEN);
    for (size_t i = 0; i < 4u; i++)
        out[i] = (uint8_t)(acc >> (8u * i));
}

/* ------------------------------------------------------------------------------------------- */

static filestore_ram_t g_fs;
static iotdata_node_content_t g_store;
static iotdata_node_ota_apply_t g_apply;

/* WHAT SURVIVES THE REBOOT, which here is simply a buffer that the rig does not clear. The real
   node writes this into NVS or a file; what is under test is the consumer's use of it, so the
   medium is deliberately the dullest thing that persists. `refuse` is the other half of the seam:
   a save that will not land, which on a device is a full or broken store. */
static struct {
    uint8_t blob[64];
    bool refuse;
    int saves;
} g_durable;

static bool durable_save(void *const ctx) {
    (void)ctx;
    g_durable.saves++;
    if (g_durable.refuse)
        return false;
    size_t size = 0u;
    const void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    memcpy(g_durable.blob, rec, size);
    return true;
}

static const ota_record_t g_record_vt = { .ctx = NULL, .save = durable_save };

#define MY_APP   "relay"
#define MY_STAMP "202610110000"

static const iotdata_node_ota_device_t g_device = {
    .app = MY_APP,
    .stamp = MY_STAMP,
    .bootloader = 1u,
    .slot_size = 4096u,
    .base_hash = NULL,
    .codec_deflate = false,
    .codec_heatshrink = false,
};

/* A bundle, built the way ota-bundle builds one: magic, a kvr header, END, then the payload. */
static uint32_t bundle(const uint32_t id, const char *const app, const char *const stamp, const uint8_t form, const uint8_t flags, const uint32_t paylen, const bool good_hash) {
    static uint8_t blob[3072];
    static uint8_t payload[2048];
    for (uint32_t i = 0; i < paylen && i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7u + 3u);

    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, form);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_NONE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, flags);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, paylen);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paylen);
    uint8_t ph[IOTDATA_NODE_OTA_HASH_LEN];
    expected_hash(payload, paylen, ph);
    if (!good_hash)
        ph[0] = (uint8_t)(ph[0] ^ 0xFFu);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_HASH, ph, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], payload, paylen);
    return filestore_ram_plant(&g_fs, id, blob, head + paylen) ? head : 0u;
}

/* A delta bundle with a BASE_HASH, for the one case that has to be refused by FORM rather than by
   anything iotdata_node_ota_check would notice. */
static uint32_t bundle_based(const uint32_t id, const char *const app, const char *const stamp, const uint32_t paylen, const uint8_t *const base) {
    static uint8_t blob[3072];
    static uint8_t payload[2048];
    for (uint32_t i = 0; i < paylen && i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7u + 3u);
    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, stamp);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_DELTA);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_NONE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, paylen);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paylen);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_BASE_HASH, base, IOTDATA_NODE_OTA_HASH_LEN);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    memcpy(&blob[head], payload, paylen);
    return filestore_ram_plant(&g_fs, id, blob, head + paylen) ? head : 0u;
}

/* What iotdata_node_ota_check makes of a stored bundle, so a test can assert that the FORMAT half
   passes something the CONSUMER half must still refuse. */
static bool iotdata_node_ota_check_passes(const uint32_t id, const iotdata_node_ota_device_t *const dev) {
    uint8_t peek[IOTDATA_NODE_CONTENT_BLOCK_BYTES];
    uint32_t total = 0u, held = 0u;
    if (!iotdata_node_content_stat(&g_store, id, &total, &held))
        return false;
    const uint32_t want = (held < (uint32_t)sizeof(peek)) ? held : (uint32_t)sizeof(peek);
    if (iotdata_node_content_read(&g_store, id, 0u, peek, want) != (int)want)
        return false;
    iotdata_node_ota_header_t h;
    if (iotdata_node_ota_parse(peek, (size_t)want, &h) != IOTDATA_NODE_OTA_OK)
        return false;
    return iotdata_node_ota_check(&h, dev, (size_t)total) == IOTDATA_NODE_OTA_OK;
}

/* A fresh everything, as a restarted process has -- except the durable record, which is the point. */
static void rig_with(const bool keep_state, const bool enabled, const char *const my_stamp, const iotdata_node_ota_device_t *const dev) {
    memset(&g_slot, 0, sizeof(g_slot));
    memset(&g_hash, 0, sizeof(g_hash));
    if (!keep_state)
        memset(&g_durable, 0, sizeof(g_durable));
    g_durable.refuse = false;
    (void)filestore_ram_init(&g_fs);
    iotdata_node_content_init(&g_store, &g_fs.backend);
    CHECK(iotdata_node_content_ota_apply_begin(&g_apply, &g_store, &g_record_vt, dev, &g_slot_vt, &g_hash_vt, my_stamp, enabled), "the consumer registers");
    /* the load, which on a device is iotdata_node_state_load putting the block back */
    size_t size = 0u;
    void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    memcpy(rec, g_durable.blob, size);
}

static void rig_as(const bool keep_state, const bool enabled, const char *const my_stamp) {
    rig_with(keep_state, enabled, my_stamp, &g_device);
}

static void rig(const bool keep_state, const bool enabled) {
    rig_as(keep_state, enabled, MY_STAMP);
}

/* Drive the store until it stops having work, bounded so a stuck consumer fails rather than hangs. */
static iotdata_node_content_event_t run(const uint32_t now_ms, const int max) {
    iotdata_node_content_event_t last = IOTDATA_NODE_CONTENT_EVENT_NONE;
    for (int i = 0; i < max; i++) {
        const iotdata_node_content_event_t ev = iotdata_node_content_tick(&g_store, now_ms);
        if (ev != IOTDATA_NODE_CONTENT_EVENT_NONE)
            last = ev;
        if (ev == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED || ev == IOTDATA_NODE_CONTENT_EVENT_WORK_DONE)
            break;
    }
    return last;
}

/* ------------------------------------------------------------------------------------------- */

static void test_not_ours(void) {
    printf("\nwhat it leaves alone, which is most of what a store holds\n");
    rig(false, true);
    uint8_t plain[200];
    for (unsigned i = 0; i < sizeof(plain); i++)
        plain[i] = (uint8_t)i;
    CHECK(filestore_ram_plant(&g_fs, 0x053701u, plain, sizeof(plain)), "an ordinary file");
    CHECK(bundle(0x053702u, "gateway", "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 100u, true) > 0u, "and a bundle for another application");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 2, "both are offered");
    CHECK(g_store.owns == 0u, "and NEITHER is claimed -- not firmware, and not ours");
    CHECK(g_slot.n_begin == 0, "nothing went near the slot");
    iotdata_node_content_end(&g_store);
}

static void test_applies(void) {
    printf("\na bundle for this build is written, verified and activated\n");
    rig(false, true);
    CHECK(bundle(0x053711u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 1500u, true) > 0u, "a newer bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed by the firmware consumer");
    CHECK(g_slot.n_begin == 0, "but claim() touches no flash: the slot is opened by work()");

    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "ticking it through asks for a restart");
    CHECK(g_slot.len == 1500u, "the whole payload reached the slot");
    CHECK(g_slot.declared == 1500u, "opened at the size the header declared");
    CHECK(g_slot.ended && g_slot.active, "finalised, and then selected for the next boot");
    CHECK(g_slot.n_write == 6, "in six pieces of 256 -- the loop is never held for a whole image");
    CHECK(g_apply.stat_applied == 1u, "counted");

    /* and the bytes are the payload, not the header */
    CHECK(g_slot.image[0] == 3u && g_slot.image[1] == 10u, "and they are the PAYLOAD, from after the header");

    /* THE ATTEMPT WAS WRITTEN DOWN, and before the activate rather than after it */
    CHECK(g_apply.persist.id == 0x053711u, "the attempt names the bundle");
    CHECK(g_apply.persist.attempts == 1u, "the first attempt");
    CHECK(strcmp(g_apply.persist.stamp, "202610120000") == 0, "and the stamp it is trying to run");
    iotdata_node_content_end(&g_store);
}

static void test_the_record_precedes_the_activate(void) {
    printf("\nthe attempt is recorded BEFORE the slot is activated, not after\n");
    rig(false, true);
    CHECK(bundle(0x053721u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 600u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    /* A STORE THAT WILL NOT WRITE is how the order becomes observable: if the record were written
       after the activate, a failed flush would leave a node booting an image nobody counted. */
    g_durable.refuse = true; /* the durable store will not take it, so the record does not land */
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "a flush that fails does NOT lead to a reboot");
    CHECK(!g_slot.active, "and the slot was never activated");
    CHECK(g_slot.n_activate == 0, "not even attempted");
    iotdata_node_content_end(&g_store);
}

static void test_slot_refusals(void) {
    printf("\nevery way the slot can refuse, and none of them boots anything\n");
    const char *const names[] = { "begin", "write", "end", "activate" };
    for (int which = 0; which < 4; which++) {
        rig(false, true);
        g_slot.fail_begin = (which == 0) ? 1 : 0;
        g_slot.fail_write = (which == 1) ? 1 : 0;
        g_slot.fail_end = (which == 2) ? 1 : 0;
        g_slot.fail_activate = (which == 3) ? 1 : 0;
        CHECK(bundle(0x053731u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 800u, true) > 0u, names[which]);
        (void)iotdata_node_content_begin(&g_store, 1000u);
        CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "refused somewhere, and no restart asked for");
        CHECK(!g_slot.active, "and nothing was selected to boot");
        CHECK(g_apply.stat_rejected == 1u, "counted as a rejection");
        /* AND IT IS NOT TRIED AGAIN THIS BOOT: a slot that just refused will refuse again, and a
           consumer that kept trying would spend the node's life writing flash. */
        const int writes = g_slot.n_write;
        (void)run(2000u, 20);
        CHECK(g_slot.n_write == writes, "and not retried for the rest of this boot");
        iotdata_node_content_end(&g_store);
    }
}

static void test_payload_hash_mismatch(void) {
    printf("\nthe right number of the wrong bytes is a file that gets destroyed\n");
    rig(false, true);
    CHECK(bundle(0x053741u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 700u, false) > 0u, "a bundle whose PAYLOAD_HASH does not match its payload");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "it is not applied");
    CHECK(!g_slot.active, "nothing is selected to boot");
    CHECK(g_slot.n_abort == 1, "the half-written slot is abandoned");
    /* The store's own CRC already passed, so this is not a transfer fault -- the file is not what
       it says it is, and keeping it would mean trying it again for ever. */
    CHECK(!iotdata_node_content_stat(&g_store, 0x053741u, NULL, NULL), "and the file is DESTROYED, not kept");
    iotdata_node_content_end(&g_store);
}

static void test_claimed_but_refused(void) {
    printf("\na bundle plainly for this node, and plainly not takeable, is claimed and explained\n");
    rig(false, true);
    /* older, with no downgrade flag: a POLICY refusal, and the one that can stop being one */
    CHECK(bundle(0x053751u, MY_APP, "202610100000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 300u, true) > 0u, "an older bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "CLAIMED -- so inspect has somebody to ask");
    CHECK(g_slot.n_begin == 0, "and never written");
    char what[160];
    CHECK(iotdata_node_content_describe(&g_store, 0x053751u, what, sizeof(what)) > 0, "it describes itself");
    CHECK(strstr(what, "older") != NULL, "naming the reason it is sitting there");
    CHECK(iotdata_node_content_stat(&g_store, 0x053751u, NULL, NULL), "AND IT IS KEPT: a refusal can stop being one");

    /* the same bundle re-cut with the flag IS taken, which is what makes keeping it worthwhile */
    rig(false, true);
    CHECK(bundle(0x053752u, MY_APP, "202610100000", IOTDATA_NODE_OTA_FORM_FULL, IOTDATA_NODE_OTA_FLAG_DOWNGRADE, 300u, true) > 0u, "re-cut with the downgrade flag");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and the same downgrade is applied");
    iotdata_node_content_end(&g_store);
}

static void test_delta_is_refused_for_now(void) {
    printf("\ndelta is refused, however good it looks, because there is no patcher\n");
    rig(false, true);
    CHECK(bundle(0x053761u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_DELTA, 0u, 300u, true) > 0u, "a delta bundle with no BASE_HASH");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "claimed: it is ours, we simply cannot take it");
    CHECK(g_slot.n_begin == 0, "and nothing was written");
    char what[160];
    CHECK(iotdata_node_content_describe(&g_store, 0x053761u, what, sizeof(what)) > 0, "described");
    CHECK(strstr(what, "base") != NULL, "as a base problem, which is the truthful reason");
    iotdata_node_content_end(&g_store);

    /* AND A DELTA THAT PASSES EVERY CHECK, which is the dangerous one. iotdata_node_ota_check
       judges a delta on its own terms and is right to pass this -- the base matches what we claim
       to be running -- so if the consumer trusted that verdict it would stream the PATCH into the
       slot as though it were an image. A slot full of patch is an image that may well still boot. */
    static const uint8_t base[IOTDATA_NODE_OTA_HASH_LEN] = { 0xAA, 0xBB, 0xCC, 0xDD };
    static iotdata_node_ota_device_t with_base;
    with_base = g_device;
    with_base.base_hash = base;
    rig_with(false, true, MY_STAMP, &with_base);
    CHECK(bundle_based(0x053762u, MY_APP, "202610120000", 300u, base) > 0u, "a delta whose BASE_HASH is the image we are running");
    CHECK(iotdata_node_ota_check_passes(0x053762u, &with_base), "and iotdata_node_ota_check passes it, correctly");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "claimed");
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and it is NOT applied");
    CHECK(g_slot.n_begin == 0, "not one byte of the patch reached the slot");
    (void)iotdata_node_content_describe(&g_store, 0x053762u, what, sizeof(what));
    CHECK(strstr(what, "form") != NULL, "and the reason given is the FORM, not the base");
    iotdata_node_content_end(&g_store);
}

static void test_disabled(void) {
    printf("\nswitched off, it holds a bundle without running it\n");
    rig(false, false);
    CHECK(bundle(0x053771u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 400u, true) > 0u, "a perfectly good bundle");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed -- holding is not running");
    CHECK(run(1000u, 10) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "but not applied");
    CHECK(g_slot.n_begin == 0, "the slot is untouched");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x053771u, what, sizeof(what));
    CHECK(strstr(what, "switched off") != NULL, "and says why");

    /* Turned on at run time it takes the bundle without a restart -- but on the store's RE-OFFER
       cadence, not instantly, because an owner that answered a bare MINE is asked again later and
       that is the only moment its answer can change. */
    iotdata_node_content_ota_apply_enable(&g_apply, true);
    CHECK(run(1000u + 1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "a second later, nothing has been asked again");
    const uint32_t after_reoffer = 1000u + (uint32_t)IOTDATA_NODE_CONTENT_REOFFER_S * 1000u + 1u;
    CHECK(run(after_reoffer, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "and on the next re-offer it is applied");
    iotdata_node_content_end(&g_store);
}

/* ------------------------------------------------------------------------------------------- */
/* THE ATTEMPT COUNTER ACROSS REBOOTS. The datastore is kept, everything else is rebuilt --     */
/* which is exactly what a restart is.                                                          */

static void test_attempt_survives_and_is_judged(void) {
    printf("\nthe attempt counter, across the reboot it was written for\n");

    /* boot 1: apply it */
    rig(false, true);
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    iotdata_node_content_end(&g_store);

    /* boot 2: the OLD image is running again -- it did not stick. */
    rig(true, true);
    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record survived the restart");
    CHECK(strstr(line, "did NOT run") != NULL, "and the stamp is not ours, so it did not run");
    CHECK(strstr(line, "try again") != NULL, "with one go left");
    CHECK(g_apply.persist.attempts == 1u, "one attempt so far");
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle is still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "so it is tried again");
    CHECK(g_apply.persist.attempts == 2u, "and counted again");
    iotdata_node_content_end(&g_store);

    /* boot 3: still the old image. The third attempt is the last. */
    rig(true, true);
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "the third attempt is allowed");
    CHECK(g_apply.persist.attempts == 3u, "three");
    iotdata_node_content_end(&g_store);

    /* boot 4: THE LIMIT. It is refused, kept, and says so. */
    rig(true, true);
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record is still there");
    CHECK(strstr(line, "giving up") != NULL, "and the verdict is to stop");
    CHECK(bundle(0x053781u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle too");
    CHECK(iotdata_node_content_begin(&g_store, 1000u) == 1, "offered");
    CHECK(g_store.owns == 1u, "and claimed, because it is still ours");
    CHECK(run(1000u, 20) != IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "but NOT applied a fourth time");
    CHECK(g_slot.n_begin == 0, "nothing written");
    char what[160];
    (void)iotdata_node_content_describe(&g_store, 0x053781u, what, sizeof(what));
    CHECK(strstr(what, "did not run") != NULL, "and inspect explains it");
    iotdata_node_content_end(&g_store);
}

/* THE ORDER A REAL NODE IS FORCED INTO, which is not the one rig() uses above. A device's state
   layer takes every block, then restores them all in one shot, and refuses a block offered after
   that restore -- and the restore happens early, long before a content store that is deliberately
   mounted after the radio. So the record is REGISTERED and FILLED before the consumer begins, and
   begin() must keep what it finds there.
 *
 * This is here because the relay could not follow the documented order and the warning that said so
 * was the only sign that the attempt counter -- the sole protection against an image that boots,
 * dies, and is applied again for ever -- had silently not been armed. Every other test in this file
 * loads the record AFTER begin, so none of them could have caught it. */
static void test_the_record_is_loaded_before_begin(void) {
    printf("\nthe durable record may be filled BEFORE begin, as a node's state layer requires\n");

    /* boot 1: make a real attempt, so g_durable holds a record worth preserving */
    rig(false, true);
    CHECK(bundle(0x053782u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied, restart asked for");
    iotdata_node_content_end(&g_store);
    CHECK(g_apply.persist.attempts == 1u, "one attempt recorded");

    /* boot 2, in the device's order: the block is addressed and filled, and only THEN does the
       consumer begin. Nothing here calls begin() first. */
    memset(&g_slot, 0, sizeof(g_slot));
    memset(&g_hash, 0, sizeof(g_hash));
    memset(&g_apply, 0, sizeof(g_apply)); /* a restarted process: the struct is bss */
    size_t size = 0u;
    void *const rec = iotdata_node_content_ota_apply_record(&g_apply, &size);
    CHECK(rec != NULL && size == sizeof(g_apply.persist), "the block can be addressed before begin");
    memcpy(rec, g_durable.blob, size); /* iotdata_node_state_load, which runs before the mount */
    CHECK(g_apply.persist.attempts == 1u, "and the restore lands in it");

    (void)filestore_ram_init(&g_fs);
    iotdata_node_content_init(&g_store, &g_fs.backend);
    CHECK(iotdata_node_content_ota_apply_begin(&g_apply, &g_store, &g_record_vt, &g_device, &g_slot_vt, &g_hash_vt, MY_STAMP, true), "the consumer registers");
    CHECK(g_apply.persist.attempts == 1u, "AND BEGIN DID NOT WIPE IT, which is the whole point");

    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "so the last boot can still be reported");
    CHECK(strstr(line, "did NOT run") != NULL, "truthfully");

    /* and the counter still counts from where it was, rather than starting again */
    CHECK(bundle(0x053782u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "the bundle is still there");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "tried again");
    CHECK(g_apply.persist.attempts == 2u, "two, not one -- the count was not lost");
    iotdata_node_content_end(&g_store);
}

static void test_booted_image_clears_only_on_confirmation(void) {
    printf("\nthe record is cleared by CONFIRMATION, and by nothing else\n");

    /* boot 1: apply a bundle whose stamp is what the NEXT build will have */
    rig(false, true);
    CHECK(bundle(0x053791u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    iotdata_node_content_end(&g_store);

    /* boot 2: we ARE the new image now -- a different build, with that stamp compiled into it. */
    rig_as(true, true, "202610120000");
    char line[160];
    CHECK(iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the record is there");
    CHECK(strstr(line, "BOOTED") != NULL, "and says the update booted");
    CHECK(strstr(line, "awaiting confirmation") != NULL, "but is not finished with");
    CHECK(g_apply.persist.attempts == 1u, "SO THE RECORD STAYS: an image that cleared it and then rolled back would be applied again");

    /* the bootloader confirms it, and only now is the attempt over */
    iotdata_node_content_ota_apply_confirmed(&g_apply);
    CHECK(g_apply.persist.attempts == 0u, "confirmation clears it");
    CHECK(g_apply.persist.id == 0u, "entirely");
    CHECK(!iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "and there is nothing left to report");

    /* boot 3: a clean start, still the new build */
    rig_as(true, true, "202610120000");
    CHECK(!iotdata_node_content_ota_apply_pending(&g_apply, line, sizeof(line)), "the cleared record persisted as cleared");
    iotdata_node_content_end(&g_store);
}

static void test_confirmation_of_someone_else(void) {
    printf("\na confirmation that is not the attempted image leaves the record alone\n");
    rig(false, true);
    CHECK(bundle(0x0537a1u, MY_APP, "202610120000", IOTDATA_NODE_OTA_FORM_FULL, 0u, 500u, true) > 0u, "a bundle");
    (void)iotdata_node_content_begin(&g_store, 1000u);
    CHECK(run(1000u, 20) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "applied");
    /* A cable flash lands a DIFFERENT build, which confirms itself on its first boot. That must not
       be read as the attempted update having succeeded -- the attempt genuinely never ran. */
    rig_as(true, true, "202610119999");
    iotdata_node_content_ota_apply_confirmed(&g_apply);
    CHECK(g_apply.persist.attempts == 1u, "the record is untouched");
    CHECK(strcmp(g_apply.persist.stamp, "202610120000") == 0, "and still names the image it was trying to run");
    iotdata_node_content_end(&g_store);
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_content_ota_apply: the OTA consumer\n");
    test_not_ours();
    test_applies();
    test_the_record_precedes_the_activate();
    test_slot_refusals();
    test_payload_hash_mismatch();
    test_claimed_but_refused();
    test_delta_is_refused_for_now();
    test_disabled();
    test_attempt_survives_and_is_judged();
    test_the_record_is_loaded_before_begin();
    test_booted_image_clears_only_on_confirmation();
    test_confirmation_of_someone_else();
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
