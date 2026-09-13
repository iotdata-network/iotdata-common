
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_endpoint.c - host tests for iotdata_node_endpoint.h, the END-DEVICE half of the node
// protocol (a sensor, a simulator, and in due course a node that both senses and relays).
//
// It is tested here rather than in a project because it belongs to neither: both consumers are
// ESP-IDF applications that a host toolchain cannot build, so without this the shared header has
// no coverage at all and a signature change is discovered by flashing.
//
// What is pinned down: STATUS scoping (including the case that a node in no mesh answers the mesh
// scope EMPTY rather than falling back to the node group), CONTROL advertising exactly what the
// application implements, the application control hook, and the always-listening receive mode.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "iotdata_config.h"
#include "iotdata_variant.h"
#include "iotdata.h"
#include "iotdata_node.h"
#include "iotdata_node_version.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"
#include "iotdata_node_endpoint.h"

/* --- stubs standing in for an application ------------------------------------------------- */

static void st_cb(uint16_t s, iotdata_node_status_t *o) { (void)s; o->uptime_s = 99; }
static bool tx_cb(const uint8_t *p, size_t n) { (void)p; (void)n; return true; }
/* a node that senses AND relays fills the mesh group in the same struct: no second callback */
static void both_cb(uint16_t s, iotdata_node_status_t *o) { st_cb(s, o); o->mesh.present = true; o->mesh.state = IOTDATA_NODE_STATUS_MESH_STATE_JOINED; o->mesh.cost = 3; }
static uint8_t seen_key = 0;
static bool ctl_cb(uint16_t s, uint8_t k, const uint8_t *v, uint8_t n) {
    (void)s; (void)v; (void)n;
    if (k == IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR) { seen_key = k; return true; }
    return false;
}
static const uint8_t keys[] = { IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR };
static size_t diag_cb(size_t *cursor, char *out, size_t outsize) { /* two records, then done */
    if (*cursor >= 2) return 0;
    const int n = snprintf(out, outsize, "rec%u", (unsigned)(*cursor)++);
    return (n < 0) ? 0 : (size_t)n;
}
/* what a sensor declares: the rest of VERSION is detected, so there is nothing else to stub */
static iotdata_version_caps_t caps;

static int count_group(const uint8_t *raw, uint8_t rlen, int mesh) {
    size_t cur = 0; uint8_t key, vlen; const uint8_t *val; int n = 0;
    while (iotdata_kvr_next(raw, rlen, &cur, &key, &val, &vlen))
        if ((key >= 0x20 && key < 0x40) == (mesh != 0)) n++;
    return n;
}
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } else printf("  ok: %s\n", m); } while (0)

int main(void) {
    printf("iotdata_node_endpoint: status scoping, control advertisement, app hook, always-on\n\n");
    int fails = 0;
    idep_node_t n; uint8_t buf[IDEP_KV_MAX]; int len;

    /* a node that senses AND relays: both groups, scopable */
    const idep_config_t both = { .caps=&caps, .status=both_cb, .tx=tx_cb, .control=ctl_cb,
                                 .control_keys=keys, .control_keys_count=1, .receive_always=true };
    idep_node_init(&n, 0x0537);
    len = idep_build(&both, &n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), 0);
    CHECK(len > 0 && count_group(buf,(uint8_t)len,0) > 0 && count_group(buf,(uint8_t)len,1) > 0, "no scope = both groups");
    len = idep_build(&both, &n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_MESH);
    CHECK(len > 0 && count_group(buf,(uint8_t)len,0) == 0 && count_group(buf,(uint8_t)len,1) > 0, "scope=mesh = mesh only");
    len = idep_build(&both, &n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_NODE);
    CHECK(len > 0 && count_group(buf,(uint8_t)len,0) > 0 && count_group(buf,(uint8_t)len,1) == 0, "scope=node = node only");

    /* a plain end device: in no mesh, so the mesh scope yields an EMPTY status, not the node group */
    const idep_config_t plain = { .caps=&caps, .status=st_cb, .tx=tx_cb, .receive_always=true };
    len = idep_build(&plain, &n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_MESH);
    CHECK(len == 0, "a sensor asked for the mesh group answers empty, not the node group");

    /* CONTROL advertises what the app implements, and only that */
    len = idep_build(&both, &n, IOTDATA_NODE_TLV_CONTROL, buf, sizeof(buf), 0);
    bool adv = false, adv_plain = false;
    size_t cur=0; uint8_t k,vl; const uint8_t *v;
    while (iotdata_kvr_next(buf,(uint8_t)len,&cur,&k,&v,&vl)) if (k==IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR) adv = true;
    CHECK(adv, "CONTROL advertises the app's key");
    len = idep_build(&plain, &n, IOTDATA_NODE_TLV_CONTROL, buf, sizeof(buf), 0);
    cur=0; while (iotdata_kvr_next(buf,(uint8_t)len,&cur,&k,&v,&vl)) if (k==IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR) adv_plain = true;
    CHECK(!adv_plain, "a node with no hook advertises no app keys");

    /* DIAGNOSTICS is advertised by every node unconditionally, so every node must answer it. A
       device with no recorder answers EMPTY -- absent would look exactly like being ignored. */
    len = idep_build(&plain, &n, IOTDATA_NODE_TLV_DIAGNOSTICS, buf, sizeof(buf), 0);
    CHECK(len == 0, "no recorder: an empty report, which is still a report");
    const idep_config_t recorder = { .caps=&caps, .status=st_cb, .tx=tx_cb, .diag=diag_cb, .receive_always=true };
    len = idep_build(&recorder, &n, IOTDATA_NODE_TLV_DIAGNOSTICS, buf, sizeof(buf), 0);
    int records = 0; bool typed = false;
    cur = 0;
    while (iotdata_kvr_next(buf,(uint8_t)len,&cur,&k,&v,&vl)) {
        if (k == IOTDATA_NODE_DIAGNOSTICS_TYPE) typed = true;
        if (k == IOTDATA_NODE_DIAGNOSTICS_DATA) records++;
    }
    CHECK(typed && records == 2, "a recorder: the type, then every record it had");

    /* the hook receives an unknown key; a key it refuses is counted unknown */
    idep_node_init(&n, 0x0537);
    uint8_t kv[8]; iotdata_kvr_t b; iotdata_kvr_init(&b, kv, sizeof(kv));
    iotdata_kvr_add_flag(&b, IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR);
    /* genuinely unassigned: MESH_STATIONS_REQUEST is no longer a candidate, since it now resolves
       to the MESH_STATIONS type and is therefore a request rather than an unknown key (an end
       device simply has no such table to answer with, which is a different thing) */
    iotdata_kvr_add_flag(&b, 0x7F);
    bool reboot = false;
    idep_process_control(&both, &n, kv, b.len, &reboot);
    CHECK(seen_key == IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR, "the hook got the key it claims");
    CHECK(n.stat_commands == 1, "a claimed key is a command");
    CHECK(n.stat_unknown == 1, "a refused key is unknown");

    /* always listening: nothing is ever scheduled or advertised (a DOWN frame is transmitted
       before it is held, so such a node hears the immediate copy), yet the receiver counts as
       open at every instant */
    idep_node_init(&n, 0x0537);
    CHECK(!idep_window_advance(&both, &n, 10u * 60u * 1000u), "an always-on node never becomes due");
    CHECK(!idep_window_pending(&n), "and so never advertises");
    CHECK(idep_window_active(&both, &n, 123456u), "but is always active");

    /* a sleeping node still schedules and advertises normally */
    const idep_config_t sleeper = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = false, .receive_every_ms = 60000u, .receive_window_ms = 30000u };
    idep_node_init(&n, 0x0538);
    CHECK(!idep_window_advance(&sleeper, &n, 1000u), "not due yet");
    CHECK(idep_window_advance(&sleeper, &n, 60000u), "due after the cadence");
    CHECK(idep_window_pending(&n), "and pending an advertisement");
    CHECK(!idep_window_active(&sleeper, &n, 1000u), "not active until the window opens");
    idep_window_begin(&sleeper, &n, 1000u);
    CHECK(idep_window_active(&sleeper, &n, 2000u), "active inside the window");
    CHECK(!idep_window_active(&sleeper, &n, 1000u + 30000u), "and closed after it");

    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
