#ifndef IOTDATA_NODE_PARTIAL_H
#define IOTDATA_NODE_PARTIAL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_partial.h - the PARTIAL marker: "the TLV after this one carries only some of its
// records".
//
// A FRAMEWORK concept, and the whole of it is here. The protocol layer neither builds it nor reads
// it: to iotdata.c a PARTIAL is an ordinary raw TLV like any other, encoded by iotdata_encode_tlv
// and decoded into dec->tlv[] where this header gives it meaning. iotdata_node.h carries the type
// number and nothing else, because that is vocabulary; everything that makes it MEAN something is
// framework and belongs above the protocol, not inside it.
//
// So the two sides are symmetric, and both are the caller's to do:
//
//   emitting  -- put the marker in BEFORE the TLV it describes (the binding is positional)
//   reading   -- a marker is an ordinary entry; the one it describes is the entry AFTER it
//
// which is why a reader has to skip marker entries rather than find them pre-absorbed. That is the
// price of the protocol layer staying ignorant, and it is the right price: the alternative had
// iotdata_fields.c knowing what a node report is.
//
// NOTHING THAT DOES NOT PAGE PAYS ANYTHING. A builder handed a NULL partial behaves exactly as it
// always did: pack what there is, fail if it does not fit. A report that fits in one frame emits
// no marker, so absence means "complete" -- which is only safe because understanding PARTIAL is
// mandatory across the system type range. A receiver that ignored it would read a truncated
// report as a whole one.
//
// THE APPLICATION DRIVES THE LOOP, and that is not merely a convenience. Ten frames emitted
// back-to-back at 2.4kbps, against a measured 180ms inter-frame floor and the module's own
// listen-before-transmit, is a self-collision storm. So a report returns "more remains" and the
// caller sends the next chunk on its own transmit cadence:
//
//     iotdata_partial_t p = { 0 };
//     do { ok = idep_report_paged(cfg, n, IOTDATA_NODE_TLV_VARIANT, 0, &p); } while (ok && p.more);
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
} iotdata_partial_t;

/* Whether this chunk needs a marker at all. NOT `total > 1`: a report of five records that all fit
   in one frame is complete and says so by carrying no marker. Only a chunk that is missing
   something -- before it, after it, or both -- is partial. */
static inline bool iotdata_partial_needed(const iotdata_partial_t *const p) {
    return p != NULL && (p->more || p->index > 0);
}

#define IOTDATA_NODE_PARTIAL_SIZE 4 /* id(16) | total(8) | index(8) */

static inline int iotdata_partial_pack(uint8_t *const buf, const size_t size, const iotdata_partial_t *const p) {
    if (buf == NULL || p == NULL || size < IOTDATA_NODE_PARTIAL_SIZE)
        return -1;
    buf[0] = (uint8_t)(p->id >> 8);
    buf[1] = (uint8_t)(p->id & 0xFFu);
    buf[2] = p->total;
    buf[3] = p->index;
    return IOTDATA_NODE_PARTIAL_SIZE;
}

static inline bool iotdata_partial_unpack(const uint8_t *const val, const size_t vlen, iotdata_partial_t *const out) {
    if (val == NULL || out == NULL || vlen < IOTDATA_NODE_PARTIAL_SIZE)
        return false;
    *out = (iotdata_partial_t){ 0 };
    out->id = (uint16_t)(((uint16_t)val[0] << 8) | (uint16_t)val[1]);
    out->total = val[2];
    out->index = val[3];
    return true;
}

/* Add the marker to a frame under construction. MUST be called immediately before the TLV it
   describes -- the binding is positional, so anything emitted between them would steal it. */
static inline bool iotdata_partial_emit(iotdata_encoder_t *const enc, const iotdata_partial_t *const p) {
    uint8_t buf[IOTDATA_NODE_PARTIAL_SIZE];
    if (iotdata_partial_pack(buf, sizeof(buf), p) < 0)
        return false;
    /* An ordinary TLV: the encoder is not being taught anything. */
    return iotdata_encode_tlv(enc, IOTDATA_NODE_TLV_PARTIAL, buf, (uint8_t)sizeof(buf)) == IOTDATA_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// Reading. A marker is an ordinary decoded entry, so a consumer walking dec->tlv[] must SKIP the
// markers and, for each real entry, look back one place to see whether it was described.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Is this decoded entry a marker rather than a report? Anything iterating dec->tlv[] and acting on
   system types needs this, or it will publish a PARTIAL as though it were a report of its own. */
static inline bool iotdata_partial_is(const iotdata_decoder_tlv_t *const t) {
    return t != NULL && t->type == IOTDATA_NODE_TLV_PARTIAL && t->format == IOTDATA_TLV_FMT_RAW && t->length >= IOTDATA_NODE_PARTIAL_SIZE;
}

/* The marker describing entry `idx`, if one precedes it. False means the report is complete, which
   is the answer for everything that does not page -- and is only safe to read that way because
   understanding PARTIAL is mandatory across the system type range. */
static inline bool iotdata_partial_of(const iotdata_decoder_t *const dec, const uint8_t idx, iotdata_partial_t *const out) {
    if (dec == NULL || idx == 0 || idx >= dec->tlv_count)
        return false;
    const iotdata_decoder_tlv_t *const prev = &dec->tlv[idx - 1];
    if (!iotdata_partial_is(prev))
        return false;
    return iotdata_partial_unpack(prev->raw, prev->length, out);
}

/* Begin, or continue, a report. Called by the report path before the builder runs: a fresh report
   (nothing walked yet) gets an identifier, a continuing one keeps the one it started with, so
   every chunk of one report carries the same tag. A builder with a real generation -- config --
   overwrites it, which is what makes cross-attempt assembly work there. */
static inline void iotdata_partial_begin(iotdata_partial_t *const p, const uint16_t fallback_id) {
    if (p == NULL)
        return;
    if (p->cursor == 0 && p->index == 0)
        p->id = fallback_id;
    p->more = false;
}

/* Account for a chunk that has GONE OUT. Only on a successful send: advancing on a build would
   skip records whenever the radio refused the frame, and there is no acknowledgement here to
   notice it. A report that has run out resets, so the next call begins a fresh one with a fresh
   identifier rather than continuing a finished report forever. */
static inline void iotdata_partial_sent(iotdata_partial_t *const p) {
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
