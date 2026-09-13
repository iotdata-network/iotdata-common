// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_status.c - host tests for iotdata_node_status.h.
//
// STATUS is the report every node sends on a period whether or not anyone asked, and the one a
// fleet view is assembled from, so what these pin is SAMENESS: three very different devices --
// a relay, a mains gateway, a sleeping sensor -- filling in what is true of each and coming out
// with the same keys, in the same order, meaning the same thing.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "iotdata_node_status.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* the three devices, as each would fill the struct */
static void a_relay(iotdata_node_status_t *const s) {
    memset(s, 0, sizeof(*s));
    s->uptime_s = 4242;
    s->reason = IOTDATA_NODE_REASON_POWER_ON;
    s->has_heap = true;
    s->heap_free = 100000;
    s->heap_min = 90000;
    s->mesh.present = true;
    s->mesh.state = IOTDATA_NODE_STATUS_MESH_STATE_JOINED;
    s->mesh.parent = 0x0537;
    s->mesh.cost = 2;
    s->mesh.peers = 3;
    s->mesh.parent_rssi = -71;
}
static void a_gateway(iotdata_node_status_t *const s) {
    memset(s, 0, sizeof(*s));
    s->uptime_s = 86400;
    s->reason = IOTDATA_NODE_REASON_UNKNOWN; /* a process cannot know */
    s->mesh.present = true;
    s->mesh.state = IOTDATA_NODE_STATUS_MESH_STATE_GATEWAY;
    s->mesh.accepting = true;
}
static void a_sensor(iotdata_node_status_t *const s) {
    memset(s, 0, sizeof(*s));
    s->uptime_s = 12;
    s->reason = IOTDATA_NODE_REASON_DEEPSLEEP;
    s->has_restarts = true;
    s->restarts = 991;
    s->has_supply = true;
    s->supply_mv = 3742;
    /* no mesh: .present stays false */
}

static int count_keys(const uint8_t *const raw, const uint8_t len, const bool mesh_group) {
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    int n = 0;
    while (iotdata_kvr_next(raw, len, &cur, &key, &val, &vlen))
        if ((key >= 0x20 && key < 0x40) == mesh_group)
            n++;
    return n;
}

static bool has_key(const uint8_t *const raw, const uint8_t len, const uint8_t want) {
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(raw, len, &cur, &key, &val, &vlen))
        if (key == want)
            return true;
    return false;
}

static void test_one_shape(void) {
    printf("three devices, one shape\n");
    uint8_t buf[256];
    iotdata_kvr_t kv;
    iotdata_node_status_t s;

    /* whatever the device, every key it emits is a declared STATUS key at its declared width, and
       they come out in key order -- which is what lets a reader parse one thing */
    void (*const devices[])(iotdata_node_status_t *) = { a_relay, a_gateway, a_sensor };
    const char *const names[] = { "relay", "gateway", "sensor" };
    for (size_t d = 0; d < sizeof(devices) / sizeof(devices[0]); d++) {
        devices[d](&s);
        iotdata_kvr_init(&kv, buf, sizeof(buf));
        CHECK(iotdata_status_pack(&kv, &s, 0) > 0, "packed");
        size_t cur = 0;
        uint8_t key, vlen, last = 0;
        const uint8_t *val;
        bool ordered = true, declared = true, sized = true;
        while (iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen)) {
            if (key < last)
                ordered = false;
            if (iotdata_node_tlv_key_name(IOTDATA_NODE_TLV_STATUS, key) == NULL)
                declared = false;
            if (vlen != iotdata_node_tlv_key_width(IOTDATA_NODE_TLV_STATUS, key))
                sized = false;
            last = key;
        }
        if (!ordered || !declared || !sized)
            printf("  FAIL: %s emitted keys out of order/undeclared/mis-sized\n", names[d]), fails++;
    }

    /* the two every node owes, whoever it is */
    for (size_t d = 0; d < sizeof(devices) / sizeof(devices[0]); d++) {
        devices[d](&s);
        iotdata_kvr_init(&kv, buf, sizeof(buf));
        (void)iotdata_status_pack(&kv, &s, 0);
        if (!has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_UPTIME) || !has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_REASON))
            printf("  FAIL: %s omitted uptime or reason\n", names[d]), fails++;
    }
}

static void test_presence(void) {
    printf("what is optional is declared, not inferred\n");
    uint8_t buf[256];
    iotdata_kvr_t kv;
    iotdata_node_status_t s;

    /* the gateway has no supply and no heap: those keys must be ABSENT, not zero. A zero here
       reads as a flat battery or an exhausted heap, which is a fault report, not a silence. */
    a_gateway(&s);
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    (void)iotdata_status_pack(&kv, &s, 0);
    CHECK(!has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_SUPPLY), "no supply reading, no supply key");
    CHECK(!has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_HEAP_FREE), "no heap figure, no heap key");
    CHECK(!has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_RESTARTS), "and no restart count");

    /* the value being zero is not the same as having no value */
    a_sensor(&s);
    s.supply_mv = 0;
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    (void)iotdata_status_pack(&kv, &s, 0);
    CHECK(has_key(buf, (uint8_t)kv.len, IOTDATA_NODE_STATUS_SUPPLY), "a declared zero IS reported");
}

static void test_scope(void) {
    printf("the scope: which groups were asked for\n");
    uint8_t buf[256];
    iotdata_kvr_t kv;
    iotdata_node_status_t s;
    a_relay(&s);

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    (void)iotdata_status_pack(&kv, &s, 0);
    CHECK(count_keys(buf, (uint8_t)kv.len, false) > 0 && count_keys(buf, (uint8_t)kv.len, true) > 0, "no scope = both groups");

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    (void)iotdata_status_pack(&kv, &s, IOTDATA_NODE_STATUS_SCOPE_MESH);
    CHECK(count_keys(buf, (uint8_t)kv.len, false) == 0 && count_keys(buf, (uint8_t)kv.len, true) > 0, "scope=mesh = mesh only");

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    (void)iotdata_status_pack(&kv, &s, IOTDATA_NODE_STATUS_SCOPE_NODE);
    CHECK(count_keys(buf, (uint8_t)kv.len, true) == 0, "scope=node = node only");

    /* a sensor asked for the mesh group answers EMPTY -- it is in no mesh -- rather than sending
       the node group it was not asked for. Empty is an answer; -1 would be a failure. */
    a_sensor(&s);
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    CHECK(iotdata_status_pack(&kv, &s, IOTDATA_NODE_STATUS_SCOPE_MESH) == 0, "no mesh, mesh scope: empty, not an error");

    /* a buffer too small is an error, not a truncated report */
    uint8_t small[3];
    a_relay(&s);
    iotdata_kvr_init(&kv, small, sizeof(small));
    CHECK(iotdata_status_pack(&kv, &s, 0) == -1, "a short buffer is an error");
}

static void test_words(void) {
    printf("the scope words, and the values a decoder meets\n");
    char b[16];
    CHECK(iotdata_status_scope_from_name("mesh") == IOTDATA_NODE_STATUS_SCOPE_MESH, "`mesh`");
    CHECK(iotdata_status_scope_from_name("node") == IOTDATA_NODE_STATUS_SCOPE_NODE, "`node`");
    CHECK(iotdata_status_scope_from_name("node,mesh") == (IOTDATA_NODE_STATUS_SCOPE_NODE | IOTDATA_NODE_STATUS_SCOPE_MESH), "both");
    CHECK(iotdata_status_scope_from_name("all") == 0, "`all` is 0, which already means everything");
    CHECK(iotdata_status_scope_is_name("all") && !iotdata_status_scope_is_name("sideways"), "a word is ours, or it is not");
    /* round trip, so a console can echo back what it understood */
    CHECK(strcmp(iotdata_status_scope_name(IOTDATA_NODE_STATUS_SCOPE_MESH, b, sizeof(b)), "mesh") == 0, "renders back");
    CHECK(strcmp(iotdata_status_scope_name(0, b, sizeof(b)), "all") == 0, "and 0 renders as all");

    CHECK(strcmp(iotdata_node_tlv_status_reason_str(IOTDATA_NODE_REASON_WATCHDOG), "watchdog") == 0, "a reason has a name");
    CHECK(strcmp(iotdata_node_tlv_status_reason_str(0xEE), "unknown") == 0, "an unassigned one is unknown");
    CHECK(strcmp(iotdata_node_tlv_status_mesh_state_str(IOTDATA_NODE_STATUS_MESH_STATE_GATEWAY), "gateway") == 0, "so has a mesh state");
    CHECK(strcmp(iotdata_node_tlv_status_mesh_state_str(0xEE), "unknown") == 0, "and an unassigned one");
}

static void test_render(void) {
    printf("one line, for a console\n");
    char line[IOTDATA_STATUS_STR_MAX];
    iotdata_node_status_t s;

    a_relay(&s);
    (void)iotdata_status_str(&s, line, sizeof(line));
    printf("    relay:   %s\n", line);
    CHECK(strstr(line, "up=4242s") != NULL && strstr(line, "reason=power_on") != NULL, "the two every node has");
    CHECK(strstr(line, "mesh=joined") != NULL && strstr(line, "parent=0537@-71dBm") != NULL, "the mesh it is in");
    CHECK(strstr(line, "supply=") == NULL, "and nothing it did not report");

    a_sensor(&s);
    (void)iotdata_status_str(&s, line, sizeof(line));
    printf("    sensor:  %s\n", line);
    CHECK(strstr(line, "supply=3742mV") != NULL && strstr(line, "restarts=991") != NULL, "what it did report");
    CHECK(strstr(line, "mesh=") == NULL, "no mesh, nothing said about one");

    a_gateway(&s);
    (void)iotdata_status_str(&s, line, sizeof(line));
    printf("    gateway: %s\n", line);
    CHECK(strstr(line, "mesh=gateway") != NULL && strstr(line, "parent=") == NULL, "a root has no parent to name");
    CHECK(strstr(line, "cost=0") != NULL, "but it does have a distance to itself");

    /* a node still looking has neither: the sentinel cost must not read as a real one */
    a_relay(&s);
    s.mesh.state = IOTDATA_NODE_STATUS_MESH_STATE_SEARCHING;
    s.mesh.cost = 0xFF;
    (void)iotdata_status_str(&s, line, sizeof(line));
    printf("    orphan:  %s\n", line);
    CHECK(strstr(line, "cost=") == NULL && strstr(line, "parent=") == NULL, "searching: no place in the tree to report");
    CHECK(strstr(line, "peers=3") != NULL, "though it does have neighbours");

    /* a buffer far too small must still come back NUL-terminated and not run past its end */
    char tiny[12];
    memset(tiny, 'X', sizeof(tiny));
    a_relay(&s);
    (void)iotdata_status_str(&s, tiny, sizeof(tiny));
    CHECK(memchr(tiny, '\0', sizeof(tiny)) != NULL, "a tiny buffer is still terminated");
}

#if !defined(IOTDATA_NO_JSON)
static void test_json(void) {
    printf("JSON: the keys that mean something other than their value\n");
    cJSON *const o = cJSON_CreateObject();
    const uint8_t reason = IOTDATA_NODE_REASON_BROWNOUT, state = IOTDATA_NODE_STATUS_MESH_STATE_ORPHANED, yes = 1, uptime[4] = { 0, 0, 0, 9 };

    CHECK(iotdata_status_json_key(o, "reason", IOTDATA_NODE_STATUS_REASON, &reason, 1), "reason is ours");
    CHECK(iotdata_status_json_key(o, "mesh-state", IOTDATA_NODE_STATUS_MESH_STATE, &state, 1), "mesh state is ours");
    CHECK(iotdata_status_json_key(o, "accepting", IOTDATA_NODE_STATUS_MESH_ACCEPTING, &yes, 1), "accepting is ours");
    /* a counter is left to the generic renderer: it IS its value */
    CHECK(!iotdata_status_json_key(o, "uptime", IOTDATA_NODE_STATUS_UPTIME, uptime, 4), "a counter is not ours");
    /* malformed: the wrong width is handed back rather than guessed at */
    CHECK(!iotdata_status_json_key(o, "reason", IOTDATA_NODE_STATUS_REASON, uptime, 4), "a mis-sized enum is not ours either");

    char *const out = cJSON_PrintUnformatted(o);
    printf("    %s\n", out);
    CHECK(strstr(out, "\"reason\":\"brownout\"") != NULL, "the reason reads as a word");
    CHECK(strstr(out, "\"mesh-state\":\"orphaned\"") != NULL, "so does the state");
    CHECK(strstr(out, "\"accepting\":true") != NULL, "and a flag as a bool");
    free(out);
    cJSON_Delete(o);
}
#endif

int main(void) {
    printf("iotdata_node_status: one report, however different the node\n\n");
    test_one_shape();
    test_presence();
    test_scope();
    test_words();
    test_render();
#if !defined(IOTDATA_NO_JSON)
    test_json();
#endif
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
