#ifndef IOTDATA_NODE_CRC64_H
#define IOTDATA_NODE_CRC64_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_crc64.h - CRC-64/XZ, which is CONTENT's integrity algorithm everywhere it has one.
//
// Not the console's, and not the OTA blob's. NOTES_CONTENT commits the protocol to this in two
// places -- the manifest's gross hash over the whole on-air object (§3), and the block-hash
// signature stream, 8 bytes per 2 KiB block (§7) -- so it sits BELOW both the wire path and the
// console path, and a node with no console still needs it to verify a manifest. The console
// insert's checksum is this same function over the same bytes on purpose: one check reachable two
// ways, rather than two that have to agree. An out-of-band checksum handed in from elsewhere (an
// assembly step, a sidecar file) compares against the same running value.
//
// WHY A CRC AND NOT A HASH, since sha256 is already in the tree for the OTA header. §7's reasoning,
// which is worth not re-deciding: a CRC gives PROVABLE detection of short bursts, which is what
// frame corruption looks like, where a general-purpose hash gives only a probability; a different
// polynomial from the link's CRC-16 makes the two detections largely independent, and an escape
// from the link layer is exactly what this exists to catch; and it has published vectors, so a
// gateway in C on Linux and a sensor on an 8-bit part can be PROVEN to agree rather than assumed to.
// Eight bytes is chosen for integrity, not authenticity, and should not be "upgraded" to 32 without
// recognising that it quadruples §7's stream.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE CONVENTION IS liblzma's, DELIBERATELY
//
// Start from IOTDATA_NODE_CRC64_INIT (0), feed as many runs as you like, and the value you hold is
// ALREADY the answer -- there is no separate finalise step to forget, and a partially accumulated
// value is directly comparable with a stored one. The complementing that CRC-64/XZ's init and
// xorout call for happens at each entry and exit instead, which is what makes that work.
//
// It also means `iotdata_node_crc64(buf, len, 0)` is bit-for-bit `lzma_crc64(buf, len, 0)`, so the
// reference implementation is a one-line cross-check on any Linux box rather than a table of
// numbers somebody has to trust. The suite's vectors were generated from it.
//
//     uint64_t crc = IOTDATA_NODE_CRC64_INIT;
//     while ((n = next_batch(buf, sizeof buf)) > 0)
//         crc = iotdata_node_crc64(buf, n, crc);
//     if (crc != expected) ...
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE TABLE IS THE ONE KNOB, and it is 128 bytes rather than §9's 2 KiB
//
// NOTES_CONTENT §9 frames the choice as "2 KiB table-driven against a bitwise loop". A NIBBLE table
// -- 16 entries of 8 bytes, two lookups per byte -- is a third point that dominates both for this
// workload, so it is the default and the 2 KiB byte-wise table is not offered at all:
//
//   on air     224 bytes per 0.8 s frame is ~280 B/s, so even the bitwise loop is free; the table
//              buys nothing a radio can measure
//   on console a 200 KB upload over USB is the only place throughput is visible, and 2 lookups per
//              byte against 8 shifts is most of the available win for 1/16th of the flash
//
// IOTDATA_NODE_CRC64_TABLE 0 selects the bitwise loop for a part that cannot spare even 128 bytes.
// Both paths are checked against the same vectors by the suite, because an untested alternative
// implementation is worse than no alternative.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CRC64_TABLE
#define IOTDATA_NODE_CRC64_TABLE 1
#endif

/* The reflected form of 0x42F0E1EBA9EA3693 (CRC-64/ECMA-182's polynomial, which XZ shares). The
   suite asserts the reversal rather than taking it on trust, and anchors the normal form on
   ECMA-182's own published check value. */
#define IOTDATA_NODE_CRC64_POLY 0xC96C5795D7870F42ULL

/* The running value to start from, and -- because of the convention above -- also the answer for no
   bytes at all, which liblzma agrees with. */
#define IOTDATA_NODE_CRC64_INIT 0x0000000000000000ULL

// -----------------------------------------------------------------------------------------------------------------------------------------

#if IOTDATA_NODE_CRC64_TABLE

/* crc64(nibble), four shifts each. Generated from IOTDATA_NODE_CRC64_POLY, and the suite
   REGENERATES it bitwise and compares -- so a typo in a literal below cannot survive a test run. */
static const uint64_t _iotdata_node_crc64_table[16] = {
    0x0000000000000000ULL, 0x7D9BA13851336649ULL, 0xFB374270A266CC92ULL, 0x86ACE348F355AADBULL, 0x64B62BCAEBC387A1ULL, 0x192D8AF2BAF0E1E8ULL, 0x9F8169BA49A54B33ULL, 0xE21AC88218962D7AULL,
    0xC96C5795D7870F42ULL, 0xB4F7F6AD86B4690BULL, 0x325B15E575E1C3D0ULL, 0x4FC0B4DD24D2A599ULL, 0xADDA7C5F3C4488E3ULL, 0xD041DD676D77EEAAULL, 0x56ED3E2F9E224471ULL, 0x2B769F17CF112238ULL,
};

#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint64_t iotdata_node_crc64(const void *const data, const size_t len, const uint64_t crc_in) {
    if (data == NULL || len == 0u)
        return crc_in;
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = ~crc_in;
    for (size_t i = 0; i < len; i++) {
#if IOTDATA_NODE_CRC64_TABLE
        crc = (crc >> 4) ^ _iotdata_node_crc64_table[(crc ^ (uint64_t)p[i]) & 0xFu];
        crc = (crc >> 4) ^ _iotdata_node_crc64_table[(crc ^ ((uint64_t)p[i] >> 4)) & 0xFu];
#else
        crc ^= (uint64_t)p[i];
        for (int bit = 0; bit < 8; bit++)
            /* a conditional rather than the sign-extended-mask trick: the same instructions after
               any optimiser, and no signed arithmetic for -Wconversion to object to */
            crc = (crc & 1u) ? ((crc >> 1) ^ IOTDATA_NODE_CRC64_POLY) : (crc >> 1);
#endif
    }
    return ~crc;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CRC64_H */
