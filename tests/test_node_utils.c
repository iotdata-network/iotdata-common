//
// test_node_utils.c - the small shared helpers: ordered insert, and node identity.
//
// A skeleton, deliberately. iotdata_node_order_insert is pure and gets covered properly; the identity
// helpers derive from the host, so they can only be checked for the properties that must hold
// wherever they run -- determinism, range, and agreement with each other.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* gethostname, which the host branch of iotdata_node_mac uses */

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "device/d_format.h"
#include "iotdata_node_utils.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static void test_order_insert(void) {
    printf("\nordered insert keeps the list sorted and carries the slot with the key\n");
    iotdata_node_order_t ord[8];
    int n = 0;
    /* arrive out of order; the list must read back in key order */
    const uint16_t keys[] = { 0x300, 0x100, 0x200 };
    for (int i = 0; i < 3; i++)
        n = iotdata_node_order_insert(ord, n, 8, keys[i], (uint16_t)i);
    CHECK(n == 3, "three inserted");
    CHECK(ord[0].key == 0x100 && ord[1].key == 0x200 && ord[2].key == 0x300, "sorted by key");
    /* the slot is the point: it says WHERE the entry lives, and must travel with its key */
    CHECK(ord[0].slot == 1, "0x100 came from slot 1");
    CHECK(ord[1].slot == 2, "0x200 came from slot 2");
    CHECK(ord[2].slot == 0, "0x300 came from slot 0");

    printf("\nand refuses to overflow rather than writing past the end\n");
    iotdata_node_order_t small[3];
    int m = 0;
    for (int i = 0; i < 5; i++)
        m = iotdata_node_order_insert(small, m, 3, (uint16_t)(100 - i), (uint16_t)i);
    CHECK(m == 3, "stops at max");
    CHECK(small[0].key == 98 && small[2].key == 100, "and what it did take is still ordered");

    CHECK(iotdata_node_order_insert(NULL, 0, 8, 1, 1) == 0, "a null list returns the count unchanged");

    printf("\nequal keys do not lose an entry\n");
    iotdata_node_order_t dup[4];
    int d = 0;
    d = iotdata_node_order_insert(dup, d, 4, 0x500, 1);
    d = iotdata_node_order_insert(dup, d, 4, 0x500, 2);
    CHECK(d == 2, "both kept");
    CHECK(dup[0].key == 0x500 && dup[1].key == 0x500, "both present");

    printf("\nalready-sorted and reverse-sorted input both come out sorted\n");
    iotdata_node_order_t up[4], down[4];
    int u = 0, w = 0;
    for (int i = 0; i < 4; i++)
        u = iotdata_node_order_insert(up, u, 4, (uint16_t)(i + 1), (uint16_t)i);
    for (int i = 0; i < 4; i++)
        w = iotdata_node_order_insert(down, w, 4, (uint16_t)(4 - i), (uint16_t)i);
    CHECK(u == 4 && w == 4, "both took everything");
    for (int i = 0; i < 4; i++)
        CHECK(up[i].key == down[i].key, "and agree on the result");
}

static void test_identity(void) {
    printf("\nidentity is derived, so it must at least be stable and in range\n");
    uint8_t a[6], b[6];
    const bool ok_a = iotdata_node_mac(a), ok_b = iotdata_node_mac(b);
    CHECK(ok_a == ok_b, "the mac read agrees with itself");
    if (ok_a) {
        CHECK(memcmp(a, b, 6) == 0, "and returns the same mac twice");
        /* all-zero would mean "we made one up", which is worse than failing honestly */
        const uint8_t zero[6] = { 0 };
        CHECK(memcmp(a, zero, 6) != 0, "a derived mac is not all zero");
    } else
        printf("  (no mac available on this host -- content checks skipped)\n");

    const uint32_t m1 = iotdata_node_mac32(), m2 = iotdata_node_mac32();
    CHECK(m1 == m2, "mac32 is stable");

    const uint16_t s1 = iotdata_node_station_from_mac("test"), s2 = iotdata_node_station_from_mac("test");
    CHECK(s1 == s2, "a station derived from the mac is stable -- it IS the node's name");
    CHECK(iotdata_station_is_assignable(s1), "and lands in the assignable range");
}

static void test_reset_reason(void) {
    printf("\nthe reset reason is always answerable, even when the answer is unknown\n");
    const uint8_t r1 = iotdata_node_reason_reset(), r2 = iotdata_node_reason_reset();
    CHECK(r1 == r2, "and does not change between calls");
}

int main(void) {
    printf("test_node_utils\n");
    test_order_insert();
    test_identity();
    test_reset_reason();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
