
#ifndef IOTDATA_NODE_UTILS_H
#define IOTDATA_NODE_UTILS_H

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

/*
 * NEEDS, FROM THE INCLUDER: d_platform_<platform>.h -- platform_identity_mac(),
 * platform_identity_alt32(), platform_identity_alt_name(), platform_reset_reason(); and d_format.h
 * for snprintf_inline().
 *
 * NOTHING HERE KNOWS WHICH PLATFORM IT IS ON, and that is the point: the board's identity and the
 * reason it booted are the two questions only a platform can answer, so they are asked of the d_
 * layer and this header does the part that is the same everywhere -- deriving a station id from an
 * identity, and mapping a reason onto the protocol's own codes.
 *
 * AND NOTHING HERE WRITES A LOG LINE. A function returns what it found and the caller says it,
 * after the call, in whichever logging the caller has -- which is also why the tag argument is
 * gone. Wording that would otherwise drift across four applications stays here as a formatter,
 * iotdata_node_identity_str(), on the same pattern as iotdata_node_version_str().
 */

// ---------------------------------------------------------------------------------------------------------------------------
// The node's own hardware identity, and the station id derived from it.
// ---------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_mac(uint8_t mac[6]) {
    return platform_identity_mac(mac);
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

static inline uint32_t iotdata_node_mac32(void) {
    uint8_t mac[6];
    return iotdata_node_mac(mac) ? ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5] : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

/* WHICH OF THE THREE SOURCES A STATION ID CAME FROM. The caller needs this to say the right thing
   at the right level: only the first is unremarkable, and the other two are a warning about an
   identity that can move. */
typedef enum {
    IOTDATA_NODE_IDENTITY_NONE = 0, /* nothing stable to derive from -- station 1, legal and nothing more */
    IOTDATA_NODE_IDENTITY_MAC,      /* the board's own address, burned in and not removable */
    IOTDATA_NODE_IDENTITY_ALT,      /* a second source -- a hostname, say -- which moves if anybody renames it */
} iotdata_node_identity_src_t;

typedef struct {
    uint16_t station;                   /* what was derived: always assignable, never 0, never broadcast */
    iotdata_node_identity_src_t source; /* where it came from, for whoever reports it */
    uint8_t mac[6];                     /* the address, for MAC; zeroed otherwise */
} iotdata_node_identity_t;

#define IOTDATA_NODE_IDENTITY_STR_MAX 112 /* the ALT sentence with a 64-byte name is the longest */

/*
 * The station id this board defaults to, always assignable: 1..4094, never 0, never broadcast.
 *
 * A DEFAULT, not a decision -- SETTINGS STATION overrides it and persists, and anything that has
 * to be addressable across a re-image should carry one rather than rely on this. The hostname
 * fallback exists so that a host with no permanent NIC still comes up with a legal station rather
 * than with 0, which is not a station at all; it is the weakest of the three, because renaming the
 * host moves the identity.
 *
 * `id` is where the derivation is reported, for a caller that wants to say what it got and from
 * what; NULL for one that only wants the number, which is the honest answer for a --help listing
 * or a test and stops either of them logging a boot line it has no business writing.
 */
static inline uint16_t iotdata_node_station_from_mac(iotdata_node_identity_t *const id) {
    iotdata_node_identity_t got = { .station = 1u, .source = IOTDATA_NODE_IDENTITY_NONE, .mac = { 0 } };
    if (iotdata_node_mac(got.mac)) {
        got.source = IOTDATA_NODE_IDENTITY_MAC;
        got.station = iotdata_station_from_id(((uint32_t)got.mac[2] << 24) | ((uint32_t)got.mac[3] << 16) | ((uint32_t)got.mac[4] << 8) | got.mac[5]);
    } else {
        /* NO MAC IS NOT NO IDENTITY. A platform with a second source -- a hostname, say -- still
           derives something stable from it, and whoever reports it names it, because that identity
           moves if anybody renames the host. A platform with no second source keeps station 1. */
        for (int i = 0; i < 6; i++)
            got.mac[i] = 0;
        const uint32_t alt = platform_identity_alt32();
        if (alt != 0u) {
            got.source = IOTDATA_NODE_IDENTITY_ALT;
            got.station = iotdata_station_from_id(alt);
        }
    }
    if (id != NULL)
        *id = got;
    return got.station;
}

/* THE WORDS, IN ONE PLACE, so that four applications report the same derivation the same way --
   the caller supplies only the level, which is what it alone knows how to do. The ALT name is
   asked of the platform here rather than carried in the struct: it is stable, and 64 bytes of it
   would otherwise ride along on every caller's stack for the one case that uses it. */
static inline const char *iotdata_node_identity_str(char *const buf, const size_t size, const iotdata_node_identity_t *const id) {
    if (id == NULL)
        return snprintf_inline(buf, size, "no identity");
    char name[64];
    switch (id->source) {
    case IOTDATA_NODE_IDENTITY_MAC:
        return snprintf_inline(buf, size, "mac=%02X:%02X:%02X:%02X:%02X:%02X station=%03" PRIX16, (unsigned)id->mac[0], (unsigned)id->mac[1], (unsigned)id->mac[2], (unsigned)id->mac[3], (unsigned)id->mac[4], (unsigned)id->mac[5],
                               id->station);
    case IOTDATA_NODE_IDENTITY_ALT:
        return snprintf_inline(buf, size, "no permanent mac, station=%03" PRIX16 " from '%s'", id->station, platform_identity_alt_name(name, sizeof(name)));
    case IOTDATA_NODE_IDENTITY_NONE:
    default:
        return snprintf_inline(buf, size, "no permanent mac, station=%03" PRIX16, id->station);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

/* THE MAPPING IS THE NODE LAYER'S JOB: the platform says why it booted in its own terms, and the
   protocol's codes are this header's to assign. One switch, in one place, rather than a vendor
   enum travelling upward into a report. */
static inline uint8_t iotdata_node_reason_reset(void) {
    switch (platform_reset_reason()) {
    case PLATFORM_RESET_POWER_ON:
        return IOTDATA_NODE_REASON_POWER_ON;
    case PLATFORM_RESET_SOFTWARE:
        return IOTDATA_NODE_REASON_SOFTWARE;
    case PLATFORM_RESET_WATCHDOG:
        return IOTDATA_NODE_REASON_WATCHDOG;
    case PLATFORM_RESET_BROWNOUT:
        return IOTDATA_NODE_REASON_BROWNOUT;
    case PLATFORM_RESET_PANIC:
        return IOTDATA_NODE_REASON_PANIC;
    case PLATFORM_RESET_DEEPSLEEP:
        return IOTDATA_NODE_REASON_DEEPSLEEP;
    case PLATFORM_RESET_EXTERNAL:
        return IOTDATA_NODE_REASON_EXTERNAL;
    case PLATFORM_RESET_UNKNOWN:
    default:
        return IOTDATA_NODE_REASON_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint16_t key;  /* what the list is ordered by -- a station id */
    uint16_t slot; /* the table slot that entry lives in */
} iotdata_node_order_t;

static inline int iotdata_node_order_insert(iotdata_node_order_t *const ord, const int n, const int max, const uint16_t key, const uint16_t slot) {
    if (ord == NULL || n >= max)
        return n;
    int pos = n;
    while (pos > 0 && ord[pos - 1].key > key) {
        ord[pos] = ord[pos - 1];
        pos--;
    }
    ord[pos].key = key;
    ord[pos].slot = slot;
    return n + 1;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_UTILS_H */
