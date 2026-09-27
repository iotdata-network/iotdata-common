#ifndef IOTDATA_NODE_VARIANT_H
#define IOTDATA_NODE_VARIANT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_variant.h - building the VARIANT report from a node's compiled-in variant suite.
//
// A VARIANT ID IS LOCAL, A FIELD ID IS GLOBAL (iotdata_node.h). This builds the translation: one
// key-value pair per variant this node PRODUCES, the key being the variant number and the value a
// u16 field id per presence slot.
//
// PRODUCES, not decodes, and the distinction is the whole reason this is not simply a walk of
// iotdata_get_variant(). A gateway is compiled with the variant maps of every node it must READ,
// so its table is full while it originates no telemetry at all; the same is true of a relay, whose
// only variant is the mesh one and which carries no telemetry either. Both answer the request with
// an EMPTY report rather than with somebody else's suite. Only a node that actually emits
// telemetry calls this.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Which keys a request wants, as a bitmask over the VARIANT key space: bit N selects key N. Zero
   is the default, which is ENTRY alone -- the ids are what make telemetry readable, and the names
   are several times their size, the same trade STATUS makes by leaving the tables out of its
   default scope. */
#define IOTDATA_VARIANT_WANT_ENTRY   (1u << IOTDATA_NODE_VARIANT_ENTRY)
#define IOTDATA_VARIANT_WANT_NAMES   (1u << IOTDATA_NODE_VARIANT_NAMES)
#define IOTDATA_VARIANT_WANT_DEFAULT IOTDATA_VARIANT_WANT_ENTRY

/*
 * Pack as many variant definitions as fit, resuming from the partial's cursor.
 *
 * The cursor is (variant << 1) | phase, where phase 0 is the ENTRY and 1 the NAMES, so a variant
 * whose entry fitted but whose names did not resumes at its names rather than repeating itself.
 * Each pair is self-contained, so a suite too big for one frame simply continues in the next: one
 * either arrives whole or arrives later, and the receiver merges by the variant id inside the
 * value. What tells a receiver it has them ALL is the partial's own total and index -- there is no
 * manifest, because answering that question in two layers only lets the two disagree.
 *
 * A defined variant whose slots are all empty still emits its ENTRY, carrying just its number: the
 * pair's PRESENCE is the declaration that the variant exists, so dropping it would say something
 * different -- that the node does not define it.
 */
static inline int iotdata_variant_pack(uint8_t *const buf, const size_t size, const uint8_t want, iotdata_partial_t *const p) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    const uint8_t sel = (want == 0u) ? (uint8_t)IOTDATA_VARIANT_WANT_DEFAULT : want;
    const bool entries = (sel & IOTDATA_VARIANT_WANT_ENTRY) != 0u, names = (sel & IOTDATA_VARIANT_WANT_NAMES) != 0u;
    uint8_t total = 0;
    for (uint8_t v = 0; v <= IOTDATA_VARIANT_MAX; v++)
        if (iotdata_get_variant(v) != NULL)
            total = (uint8_t)(total + (entries ? 1u : 0u) + (names ? 1u : 0u));
    uint16_t cur = (p != NULL) ? (uint16_t)p->cursor : 0u;
    uint8_t packed = 0;
    bool more = false;
    for (; (cur >> 1) <= IOTDATA_VARIANT_MAX; cur++) {
        const uint8_t v = (uint8_t)(cur >> 1), phase = (uint8_t)(cur & 1u);
        const iotdata_variant_def_t *const d = iotdata_get_variant(v);
        if (d != NULL) {
            if (!((phase == 0u && !entries) || (phase == 1u && !names))) {
                uint8_t val[IOTDATA_TLV_VALUE_MAX];
                const size_t room = (size > kv.len + 2u) ? size - kv.len - 2u : 0u;
                const size_t n = (phase == 0u) ? iotdata_node_variant_encode(d, v, val, room < sizeof(val) ? room : sizeof(val)) : iotdata_node_variant_encode_names(d, v, val, room < sizeof(val) ? room : sizeof(val));
                if (n == 0) {
                    more = true;
                    break; /* it did not fit what is left: leave it for the next frame */
                }
                iotdata_kvr_add(&kv, (phase == 0u) ? IOTDATA_NODE_VARIANT_ENTRY : IOTDATA_NODE_VARIANT_NAMES, val, (uint8_t)n);
                packed++;
            }
        }
    }
    if (p != NULL) {
        p->total = total;
        p->chunk = packed;
        p->more = more;
        p->cursor = more ? (uint32_t)cur : 0u; /* 0: the suite is done, a later request restarts it */
    }
    return kv.overflow ? -1 : (int)kv.len;
}

static inline int iotdata_variant_pack_none(uint8_t *const buf, const size_t size) {
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    return kv.overflow ? -1 : (int)kv.len;
}

#endif /* IOTDATA_NODE_VARIANT_H */
