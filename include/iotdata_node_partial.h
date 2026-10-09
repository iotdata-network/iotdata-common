#ifndef IOTDATA_NODE_PARTIAL_H
#define IOTDATA_NODE_PARTIAL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_partial.h - the PARTIAL marker: "the TLV after this one carries only some of its
// records".
//
// THE APPLICATION DRIVES THE LOOP, and that is not merely a convenience. Ten frames emitted
// back-to-back at 2.4kbps, against a measured 180ms inter-frame floor and the module's own
// listen-before-transmit, is a self-collision storm. So a report returns "more remains" and the
// caller sends the next chunk on its own transmit cadence:
//
//     iotdata_node_partial_t p = { 0 };
//     do { ok = iotdata_node_report_paged(n, IOTDATA_NODE_TLV_VARIANT, 0, &p); } while (ok && p.more);
//                                                           ^ one per cycle, not one per loop pass
//
// `cursor` is the BUILDER's, `index`/`total`/`more` are the loop's and the wire's. Keeping them
// apart is what lets a builder resume by whatever it iterates -- VARIANT walks variant ids and
// skips the unassigned ones, so its resume point is not its record count.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint16_t id;     /* same value => same content; lets a receiver combine two ATTEMPTS at one report */
    uint8_t total;   /* records in the whole report */
    uint8_t index;   /* first record carried by this chunk */
    uint8_t chunk;   /* records the builder actually packed into it */
    uint32_t cursor; /* the builder's private resume point; 0 to start. NOT on the wire, so it is
                        as wide as the widest thing a builder resumes by -- a variant id, a table
                        row, a byte offset into a recorder. */
    bool more;       /* the builder found records it could not fit */
} iotdata_node_partial_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// WRITING
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Whether this chunk needs a marker at all. NOT `total > 1`: a report of five records that all fit
   in one frame is complete and says so by carrying no marker. Only a chunk that is missing
   something -- before it, after it, or both -- is partial. */
static inline bool iotdata_node_partial_needed(const iotdata_node_partial_t *const p) {
    return p != NULL && (p->more || p->index > 0);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_PARTIAL_SIZE 4 /* id(16) | total(8) | index(8) */

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_partial_pack(uint8_t *const buf, const size_t size, const iotdata_node_partial_t *const p) {
    if (buf == NULL || p == NULL || size < IOTDATA_NODE_PARTIAL_SIZE)
        return -1;
    buf[0] = (uint8_t)(p->id >> 8);
    buf[1] = (uint8_t)(p->id & 0xFFu);
    buf[2] = p->total;
    buf[3] = p->index;
    return IOTDATA_NODE_PARTIAL_SIZE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_partial_unpack(const uint8_t *const val, const size_t vlen, iotdata_node_partial_t *const out) {
    if (val == NULL || out == NULL || vlen < IOTDATA_NODE_PARTIAL_SIZE)
        return false;
    *out = (iotdata_node_partial_t){ 0 };
    out->id = (uint16_t)(((uint16_t)val[0] << 8) | (uint16_t)val[1]);
    out->total = val[2];
    out->index = val[3];
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_partial_emit(iotdata_encoder_t *const enc, const iotdata_node_partial_t *const p) {
    uint8_t buf[IOTDATA_NODE_PARTIAL_SIZE];
    return iotdata_node_partial_pack(buf, sizeof(buf), p) > 0 && iotdata_encode_tlv(enc, IOTDATA_TLV_TYPE_PARTIAL, buf, (uint8_t)sizeof(buf)) == IOTDATA_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_partial_is(const iotdata_decoder_tlv_t *const t) {
    return t != NULL && t->type == IOTDATA_TLV_TYPE_PARTIAL && t->format == IOTDATA_TLV_FMT_RAW && t->length >= IOTDATA_NODE_PARTIAL_SIZE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_partial_of(const iotdata_decoder_t *const dec, const uint8_t idx, iotdata_node_partial_t *const out) {
    if (dec == NULL || idx == 0 || idx >= dec->tlv_count)
        return false;
    const iotdata_decoder_tlv_t *const prev = &dec->tlv[idx - 1];
    return iotdata_node_partial_is(prev) && iotdata_node_partial_unpack(prev->raw, prev->length, out);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_partial_begin(iotdata_node_partial_t *const p, const uint16_t fallback_id) {
    if (p == NULL)
        return;
    if (p->cursor == 0 && p->index == 0)
        p->id = fallback_id;
    p->more = false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_partial_sent(iotdata_node_partial_t *const p) {
    if (p == NULL)
        return;
    if (p->more) {
        p->index = (uint8_t)(p->index + p->chunk);
    } else {
        p->index = 0;
        p->cursor = 0;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_PARTIAL_H */
