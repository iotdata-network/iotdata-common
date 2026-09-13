// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_control.c - host tests for iotdata_node_control.h.
//
// The property under test throughout is that ONE node driven TWO ways behaves the same: a console
// and an MQTT request must reach the same command, the same arguments and the same bytes. A
// vocabulary that diverges per medium is the failure this header exists to prevent, and it is not
// a failure any single-medium test can see.
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
#include "iotdata_node_control.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

static const iotdata_control_command_t *argv_find(iotdata_control_args_t *const a, int *const used, int argc, ...) {
    char *v[8];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 8; i++)
        v[i] = va_arg(ap, char *);
    va_end(ap);
    return iotdata_control_from_argv(argc, v, a, used);
}

static void test_vocabulary(void) {
    printf("the vocabulary: one set of words, whoever is asking\n");
    CHECK(iotdata_control_find("vers") != NULL, "a four-letter primary resolves");
    CHECK(iotdata_control_find("mesh-peers-update") != NULL, "and a mesh command");
    CHECK(iotdata_control_find("no-such-command") == NULL, "an unknown name does not");
    CHECK(iotdata_control_find(NULL) == NULL, "nor NULL");

    /* every row must name a key the key table knows, or a manager cannot decode what it built */
    for (uint8_t i = 0;; i++) {
        const iotdata_control_command_t *const c = iotdata_control_at(i);
        if (c == NULL)
            break;
        if (c->arg == IOTDATA_CONTROL_ARG_REPORTS)
            continue; /* not one key */
        if (iotdata_node_tlv_key_name(IOTDATA_NODE_TLV_CONTROL, c->key) == NULL) {
            printf("  FAIL: %s emits key 0x%02X, which is not in the key table\n", c->name, c->key);
            fails++;
        }
    }
}

static void test_app_commands(void) {
    printf("a device adds its own, and cannot take a word that is taken\n");
    static const iotdata_control_command_t app[] = {
        { "calibrate", 0x80, IOTDATA_CONTROL_ARG_NONE, 0 }, /* a TSA's, in the proprietary range */
        { "boot", 0x81, IOTDATA_CONTROL_ARG_NONE, 0 },      /* and an attempt to redefine a built-in */
    };
    iotdata_control_init(app, (uint8_t)(sizeof(app) / sizeof(app[0])));

    const iotdata_control_command_t *const c = iotdata_control_find("calibrate");
    CHECK(c != NULL && c->key == 0x80, "the device's own command is found");
    /* built-ins are searched FIRST: an application must not leave an operator holding a word that
       means something else on this node than on every other one */
    const iotdata_control_command_t *const b = iotdata_control_find("boot");
    CHECK(b != NULL && b->key == IOTDATA_NODE_CONTROL_REBOOT, "and cannot shadow `boot`");

    /* the walk covers both, so help and completion see the device's commands too */
    bool saw_app = false, saw_builtin = false;
    for (uint8_t i = 0;; i++) {
        const iotdata_control_command_t *const w = iotdata_control_at(i);
        if (w == NULL)
            break;
        if (strcmp(w->name, "calibrate") == 0)
            saw_app = true;
        if (strcmp(w->name, "vers") == 0)
            saw_builtin = true;
    }
    CHECK(saw_app && saw_builtin, "the walk yields built-ins and the device's own");
    iotdata_control_init(NULL, 0);
    CHECK(iotdata_control_find("calibrate") == NULL, "and they go when the device deregisters");
}

static void test_console(void) {
    printf("a console: argv joined, longest match first\n");
    iotdata_control_args_t a;
    int used = 0;

    const iotdata_control_command_t *c = argv_find(&a, &used, 2, "diag", "enable");
    CHECK(c != NULL && strcmp(c->name, "diag-enable") == 0, "`diag enable` is diag-enable");
    CHECK(used == 2, "both words were the name");

    /* the SAME command typed the other way -- neither spelling is a second vocabulary */
    c = argv_find(&a, &used, 1, "diag-enable");
    CHECK(c != NULL && strcmp(c->name, "diag-enable") == 0, "and so is `diag-enable`");

    /* the longest match matters: the trailing words here are ARGUMENTS, and a greedy join would
       go looking for a command called mesh-peers-update-0x537-remove */
    c = argv_find(&a, &used, 5, "mesh", "peers", "update", "0x537", "remove");
    CHECK(c != NULL && strcmp(c->name, "mesh-peers-update") == 0, "the name stops where the arguments start");
    CHECK(used == 3, "three words of name");
    CHECK(a.station == 0x537, "the station was read");
    CHECK(a.action == IOTDATA_NODE_CONTROL_MESH_PEER_NONE, "and `remove` is the action");

    c = argv_find(&a, &used, 5, "mesh", "filters", "update", "0xABC", "block");
    CHECK(c != NULL && a.action == IOTDATA_NODE_CONTROL_MESH_FILTERS_BLOCK, "`block` is an action");
    c = argv_find(&a, &used, 3, "mesh", "filters", "clear");
    CHECK(c != NULL && a.scope_filter == IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_ALL, "no scope word = every entry");
    c = argv_find(&a, &used, 4, "mesh", "filters", "clear", "manual");
    CHECK(c != NULL && a.scope_filter == IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_MANUAL, "and a scope word is read");
    c = argv_find(&a, &used, 2, "stat", "mesh");
    CHECK(c != NULL && a.scope_status == IOTDATA_NODE_STATUS_SCOPE_MESH, "`stat mesh` scopes the status");

    /* the trap in longest-match: `mesh peers` IS a command (the request), so a mistyped or legacy
       removal must not quietly resolve to it with the two words dropped on the floor */
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "remove", "0x537") == NULL, "words that cannot belong to the name reject it");
    CHECK(argv_find(&a, &used, 3, "diag", "enable", "please") == NULL, "an argument to a command that takes none");
    CHECK(argv_find(&a, &used, 4, "mesh", "filters", "clear", "sideways") == NULL, "a scope that is not a scope");
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "update", "everyone") == NULL, "an update with no station");
    CHECK(argv_find(&a, &used, 5, "mesh", "peers", "update", "0x537", "sideways") == NULL, "an action that is not an action");
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "update", "0x537") != NULL, "but the station alone is enough");

    CHECK(argv_find(&a, &used, 1, "nonesuch") == NULL, "an unknown verb resolves to nothing");
    CHECK(argv_find(&a, &used, 2, "mesh", "nonesuch") == NULL, "and so does an unknown subcommand");
    /* absent target is BROADCAST, as it is over MQTT: "this command, to whoever hears it" */
    c = argv_find(&a, &used, 1, "vers");
    CHECK(c != NULL && a.target == IOTDATA_STATION_BROADCAST, "no target named means broadcast");
}

/* THE point of the header: two media, one result. */
static void test_media_agree(void) {
    printf("two media, identical bytes\n");
#if !defined(IOTDATA_NO_JSON)
    static const struct {
        const char *json;
        int argc;
        const char *argv[5];
    } cases[] = {
        { "{\"cmd\":\"mesh-peers-update\",\"station\":\"0x537\",\"action\":\"remove\"}", 5, { "mesh", "peers", "update", "0x537", "remove" } },
        { "{\"cmd\":\"mesh-filters-update\",\"station\":\"0xABC\",\"action\":\"block\"}", 5, { "mesh", "filters", "update", "0xABC", "block" } },
        { "{\"cmd\":\"mesh-filters-clear\",\"scope\":\"manual\"}", 4, { "mesh", "filters", "clear", "manual" } },
        { "{\"cmd\":\"diag-enable\"}", 2, { "diag", "enable" } },
        { "{\"cmd\":\"stat\",\"scope\":\"mesh\"}", 2, { "stat", "mesh" } },
        { "{\"cmd\":\"vers\"}", 1, { "vers" } },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *const root = cJSON_Parse(cases[i].json);
        iotdata_control_args_t ja, ca;
        const iotdata_control_command_t *const jc = iotdata_control_from_json(root, &ja);
        char *v[5];
        for (int k = 0; k < cases[i].argc; k++)
            v[k] = (char *)(uintptr_t)cases[i].argv[k];
        const iotdata_control_command_t *const cc = iotdata_control_from_argv(cases[i].argc, v, &ca, NULL);
        if (jc == NULL || cc == NULL || jc != cc) {
            printf("  FAIL: %s resolved differently by the two media\n", cases[i].json);
            fails++;
            cJSON_Delete(root);
            continue;
        }
        uint8_t jb[64], cb[64];
        iotdata_kvr_t jk, ck;
        iotdata_kvr_init(&jk, jb, sizeof(jb));
        iotdata_kvr_init(&ck, cb, sizeof(cb));
        (void)iotdata_control_build(&jk, jc, &ja);
        (void)iotdata_control_build(&ck, cc, &ca);
        if (jk.len != ck.len || memcmp(jb, cb, jk.len) != 0) {
            printf("  FAIL: %s built different bytes from the console form\n", cases[i].json);
            fails++;
        }
        cJSON_Delete(root);
    }
#else
    printf("  (skipped: built without JSON, so there is only one medium here)\n");
#endif
}

static void test_report(void) {
    printf("the report: what a node says it accepts\n");
    static const uint8_t own[] = { 0x80, 0x81 };
    uint8_t buf[128];
    iotdata_kvr_t kv;

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    const iotdata_control_report_t plain = { .tables = false, .keys = NULL, .keys_count = 0 };
    CHECK(iotdata_control_pack(&kv, &plain) > 0, "packed");
    /* the seven every node answers */
    uint8_t seen = 0;
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen))
        seen++;
    CHECK(seen == 7, "seven unconditional: five reports, plus diagnostics and reboot");

    /* a mesh node also answers for its tables -- the capability, whether or not they are empty */
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    const iotdata_control_report_t meshy = { .tables = true, .keys = own, .keys_count = (uint8_t)sizeof(own) };
    CHECK(iotdata_control_pack(&kv, &meshy) > 0, "packed");
    bool tables = false, mine = false;
    cur = 0;
    while (iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen)) {
        if (key == iotdata_node_tlv_control_key(IOTDATA_NODE_TLV_MESH_PEERS))
            tables = true;
        if (key == 0x81)
            mine = true;
    }
    CHECK(tables, "the table requests are advertised");
    CHECK(mine, "and the device's own keys");

    /* a buffer too small must fail rather than advertise half a list -- a truncated report is a
       node claiming less than it can do, which is a worse lie than claiming more */
    uint8_t small[4];
    iotdata_kvr_init(&kv, small, sizeof(small));
    CHECK(iotdata_control_pack(&kv, &meshy) == -1, "a short buffer is an error");
}

int main(void) {
    printf("iotdata_node_control: one node, driven two ways\n\n");
    test_vocabulary();
    test_app_commands();
    test_console();
    test_media_agree();
    test_report();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
