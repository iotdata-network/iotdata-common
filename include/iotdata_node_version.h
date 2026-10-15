#ifndef IOTDATA_NODE_VERSION_H
#define IOTDATA_NODE_VERSION_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_version.h - what a node says it is: the single source of truth for VERSION.
//
// An application should not assemble any of these strings itself -- the point of one header is that
// every node in the fleet answers the same question the same way, and that a reader can parse one
// grammar rather than guessing per device.
//
// The key space and the grammars are defined in iotdata_node.h, at the VERSION keys.
//
//   hardware      board/arch            detected: chip on ESP-IDF, device-tree or DMI on Linux
//   firmware      stack/version[+low]   detected: IDF + bootloader, or kernel
//   software      app/semver/stamp      declared: IOTDATA_NODE_VERSION_APP / _SEMVER / _STAMP
//   serial        hex                   detected: eFuse MAC, cpu serial, machine-id
//   capabilities  16-bit entries        declared by the app, seeded from build flags
//   image         hex                   detected: the hash of the bytes that are actually running
//
// Every function here fills a buffer the caller owns and hands it back; where it goes is the
// application's business.
//
// IOTDATA_NODE_VERSION_STAMP comes from the build, regenerated every time, and a build that does
// not set it reports IOTDATA_NODE_VERSION_STAMP_NONE so the omission is visible rather than
// silently inherited from whenever the tree was last touched. A stamp that lies is worse than
// no stamp: it is how an hour goes into debugging a binary that is not the one running.
//
// WHICH IS WHY THERE IS ALSO AN IMAGE HASH. The stamp is the best DECLARED answer and it is still
// only a declaration; the hash is taken from the bytes in flash, so it cannot be forgotten, reused
// or hand-set. Everything else in here describes what a node was built to be -- `image` is what it
// IS. A platform that cannot hash its own image omits the field rather than offering a substitute.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_VERSION_APP
#define IOTDATA_NODE_VERSION_APP "unknown"
#endif

#ifndef IOTDATA_NODE_VERSION_SEMVER
#define IOTDATA_NODE_VERSION_SEMVER "0.0.0"
#endif

/* yyyymmddhhmm, and the ONLY thing here that anything should compare. A build sets it directly --
   on a host that is one `date -u` in the Makefile, regenerated every invocation. Where the build
   system cannot conveniently supply one, a platform may derive it instead: ESP-IDF stamps its app
   descriptor at every link, which is a better source than a define because it cannot be forgotten
   or left behind by an incremental build. Unset and underivable, it reads as _NONE, so the
   omission is visible rather than silently inherited from whenever the tree was last touched. */
#define IOTDATA_NODE_VERSION_STAMP_NONE          "000000000000"
#define IOTDATA_NODE_VERSION_STAMP_LEN           12

/* Field sizes in CHARACTERS, not including terminator */
#define IOTDATA_NODE_VERSION_HARDWARE_MAX        24
#define IOTDATA_NODE_VERSION_FIRMWARE_MAX        24
#define IOTDATA_NODE_VERSION_SOFTWARE_MAX        48
#define IOTDATA_NODE_VERSION_SERIAL_MAX          32 /* a machine-id is 32 hex, and an identity must never be truncated */
#define IOTDATA_NODE_VERSION_CAPS_STR_MAX        96
#define IOTDATA_NODE_VERSION_IMAGE_MAX           (2 * IOTDATA_NODE_VERSION_IMAGE_BYTES) /* the HEX rendering; the wire carries the bytes */
#define IOTDATA_NODE_VERSION_STR_MAX             288

// -----------------------------------------------------------------------------------------------------------------------------------------
// CAPABILITIES
//
// One 16-bit entry per category: [key:4][mask:12], big-endian, matching the table rows in
// iotdata_node.h. Nibble-aligned on purpose -- `1A 2B` reads out of a hex dump as key 1, mask
// 0xA2B, and this fleet is debugged from hex dumps.
//
// A BITMASK AND NOT AN ENUM, because a box can have two of a thing: a gateway with an E22 and an
// SX1302 sets both radio bits. A zero mask is not "category present, empty" -- omit the entry.
//
// EIGHT PUBLIC CATEGORIES, PERMANENTLY: four bits cannot grow. Bit 3 set means PROPRIETARY, free
// for a vendor or application, which follows the same high-bit convention as every other key
// space here. If a category ever outgrows twelve bits, REPEAT THE KEY -- entries with the same key
// OR-merge, so a category can span words without the format changing.
//
// NOT A SECOND REGISTRY FOR THINGS ALREADY DISCOVERABLE. Which commands a node answers is
// advertised by CONTROL, which telemetry it produces by VARIANT, what it will accept while asleep
// by RECEIVE. Capabilities are for what those cannot say: hardware that is present, and build-time
// choices that are not commands.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_VERSION_CAPS_RADIO          0x0
#define IOTDATA_NODE_VERSION_CAPS_DISPLAY        0x1
#define IOTDATA_NODE_VERSION_CAPS_MODEM          0x2
#define IOTDATA_NODE_VERSION_CAPS_SENSOR         0x3
#define IOTDATA_NODE_VERSION_CAPS_STORAGE        0x4
#define IOTDATA_NODE_VERSION_CAPS_POWER          0x5
#define IOTDATA_NODE_VERSION_CAPS_FEATURES       0x6
/* 0x7 unassigned; 0x8..0xF proprietary (bit 3) */
#define IOTDATA_NODE_VERSION_CAPS_PROPRIETARY    0x8
#define IOTDATA_NODE_VERSION_CAPS_COUNT          16
#define IOTDATA_NODE_VERSION_CAPS_MASK_MAX       0x0FFFu

#define IOTDATA_NODE_VERSION_RADIO_E22_DIP       0x001
#define IOTDATA_NODE_VERSION_RADIO_E22_USB       0x002
#define IOTDATA_NODE_VERSION_RADIO_SX1302        0x004
#define IOTDATA_NODE_VERSION_RADIO_RAK3272       0x008

#define IOTDATA_NODE_VERSION_DISPLAY_ILI9488     0x001
#define IOTDATA_NODE_VERSION_DISPLAY_SSD1306     0x002

#define IOTDATA_NODE_VERSION_MODEM_USB_CDC       0x001

#define IOTDATA_NODE_VERSION_SENSOR_BME280       0x001
#define IOTDATA_NODE_VERSION_SENSOR_SDS          0x002
#define IOTDATA_NODE_VERSION_SENSOR_TSA          0x004
#define IOTDATA_NODE_VERSION_SENSOR_WIND         0x008
#define IOTDATA_NODE_VERSION_SENSOR_SOLAR        0x010
#define IOTDATA_NODE_VERSION_SENSOR_LTR390       0x020
#define IOTDATA_NODE_VERSION_SENSOR_GNSS         0x040

#define IOTDATA_NODE_VERSION_FEATURE_MESH        0x001
#define IOTDATA_NODE_VERSION_FEATURE_BLACKBOX    0x002
#define IOTDATA_NODE_VERSION_FEATURE_OTA         0x004
#define IOTDATA_NODE_VERSION_FEATURE_JSON        0x008
#define IOTDATA_NODE_VERSION_FEATURE_FLOAT       0x010
#define IOTDATA_NODE_VERSION_FEATURE_ENCRYPTION  0x020
#define IOTDATA_NODE_VERSION_FEATURE_PERSISTENCE 0x040

typedef struct {
    uint16_t entry[IOTDATA_NODE_VERSION_CAPS_COUNT]; /* at most one per category before OR-merging */
    uint8_t count;
} iotdata_node_version_caps_t;

static inline const char *iotdata_node_version_caps_name(const uint8_t key) {
    switch (key) {
    case IOTDATA_NODE_VERSION_CAPS_RADIO:
        return "radio";
    case IOTDATA_NODE_VERSION_CAPS_DISPLAY:
        return "display";
    case IOTDATA_NODE_VERSION_CAPS_MODEM:
        return "modem";
    case IOTDATA_NODE_VERSION_CAPS_SENSOR:
        return "sensor";
    case IOTDATA_NODE_VERSION_CAPS_STORAGE:
        return "storage";
    case IOTDATA_NODE_VERSION_CAPS_POWER:
        return "power";
    case IOTDATA_NODE_VERSION_CAPS_FEATURES:
        return "features";
    default:
        return (key & IOTDATA_NODE_VERSION_CAPS_PROPRIETARY) != 0 ? "proprietary" : "reserved";
    }
}

static inline const char *iotdata_node_version_caps_bit_name(const uint8_t key, const uint16_t bit) {
    switch (key) {
    case IOTDATA_NODE_VERSION_CAPS_RADIO:
        switch (bit) {
        case IOTDATA_NODE_VERSION_RADIO_E22_DIP:
            return "e22-dip";
        case IOTDATA_NODE_VERSION_RADIO_E22_USB:
            return "e22-usb";
        case IOTDATA_NODE_VERSION_RADIO_SX1302:
            return "sx1302";
        case IOTDATA_NODE_VERSION_RADIO_RAK3272:
            return "rak3272";
        default:
            return NULL;
        }
    case IOTDATA_NODE_VERSION_CAPS_DISPLAY:
        switch (bit) {
        case IOTDATA_NODE_VERSION_DISPLAY_ILI9488:
            return "ili9488";
        case IOTDATA_NODE_VERSION_DISPLAY_SSD1306:
            return "ssd1306";
        default:
            return NULL;
        }
    case IOTDATA_NODE_VERSION_CAPS_MODEM:
        return bit == IOTDATA_NODE_VERSION_MODEM_USB_CDC ? "usb-cdc" : NULL;
    case IOTDATA_NODE_VERSION_CAPS_SENSOR:
        switch (bit) {
        case IOTDATA_NODE_VERSION_SENSOR_BME280:
            return "bme280";
        case IOTDATA_NODE_VERSION_SENSOR_GNSS:
            return "gnss";
        case IOTDATA_NODE_VERSION_SENSOR_LTR390:
            return "ltr390";
        case IOTDATA_NODE_VERSION_SENSOR_SDS:
            return "sds";
        case IOTDATA_NODE_VERSION_SENSOR_TSA:
            return "tsa";
        case IOTDATA_NODE_VERSION_SENSOR_WIND:
            return "wind";
        case IOTDATA_NODE_VERSION_SENSOR_SOLAR:
            return "solar";
        default:
            return NULL;
        }
    case IOTDATA_NODE_VERSION_CAPS_FEATURES:
        switch (bit) {
        case IOTDATA_NODE_VERSION_FEATURE_MESH:
            return "mesh";
        case IOTDATA_NODE_VERSION_FEATURE_BLACKBOX:
            return "bbox";
        case IOTDATA_NODE_VERSION_FEATURE_OTA:
            return "ota";
        case IOTDATA_NODE_VERSION_FEATURE_JSON:
            return "json";
        case IOTDATA_NODE_VERSION_FEATURE_FLOAT:
            return "float";
        case IOTDATA_NODE_VERSION_FEATURE_ENCRYPTION:
            return "crypt";
        case IOTDATA_NODE_VERSION_FEATURE_PERSISTENCE:
            return "persist";
        default:
            return NULL;
        }
    default:
        return NULL;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_version_caps_init(iotdata_node_version_caps_t *const c) {
    *c = (iotdata_node_version_caps_t){ 0 };
    uint16_t f = 0;
#if !defined(IOTDATA_NO_JSON)
    f |= IOTDATA_NODE_VERSION_FEATURE_JSON;
#endif
#if !defined(IOTDATA_NO_FLOATING)
    f |= IOTDATA_NODE_VERSION_FEATURE_FLOAT;
#endif
#if defined(IOTDATA_NODE_VERSION_HAS_MESH)
    f |= IOTDATA_NODE_VERSION_FEATURE_MESH;
#endif
#if defined(IOTDATA_NODE_VERSION_HAS_BLACKBOX) || (defined(IOTDATA_NODE_DIAGNOSTICS) && IOTDATA_NODE_DIAGNOSTICS)
    f |= IOTDATA_NODE_VERSION_FEATURE_BLACKBOX;
#endif
#if defined(IOTDATA_NODE_VERSION_HAS_OTA)
    f |= IOTDATA_NODE_VERSION_FEATURE_OTA;
#endif
    if (f != 0) {
        c->entry[0] = (uint16_t)((IOTDATA_NODE_VERSION_CAPS_FEATURES << 12) | f);
        c->count = 1;
    }
}

static inline bool iotdata_node_version_caps_add(iotdata_node_version_caps_t *const c, const uint8_t key, const uint16_t mask) {
    if (key >= IOTDATA_NODE_VERSION_CAPS_COUNT || (mask & ~IOTDATA_NODE_VERSION_CAPS_MASK_MAX) != 0 || mask == 0)
        return false;
    for (uint8_t i = 0; i < c->count; i++)
        if ((uint8_t)(c->entry[i] >> 12) == key) {
            c->entry[i] |= (uint16_t)(mask & IOTDATA_NODE_VERSION_CAPS_MASK_MAX);
            return true;
        }
    if (c->count >= IOTDATA_NODE_VERSION_CAPS_COUNT)
        return false;
    c->entry[c->count++] = (uint16_t)(((uint16_t)key << 12) | (mask & IOTDATA_NODE_VERSION_CAPS_MASK_MAX));
    return true;
}

static inline uint16_t iotdata_node_version_caps_get(const iotdata_node_version_caps_t *const c, const uint8_t key) {
    for (uint8_t i = 0; i < c->count; i++)
        if ((uint8_t)(c->entry[i] >> 12) == key)
            return (uint16_t)(c->entry[i] & IOTDATA_NODE_VERSION_CAPS_MASK_MAX);
    return 0;
}

static inline int iotdata_node_version_caps_pack(const iotdata_node_version_caps_t *const c, uint8_t *const buf, const size_t size) {
    if (buf == NULL || size < (size_t)c->count * 2u)
        return -1;
    for (uint8_t i = 0; i < c->count; i++) { /* big-endian, as everything else on this wire is */
        buf[i * 2u] = (uint8_t)(c->entry[i] >> 8);
        buf[i * 2u + 1u] = (uint8_t)c->entry[i];
    }
    return (int)c->count * 2;
}

static inline bool iotdata_node_version_caps_parse(const uint8_t *const buf, const size_t len, iotdata_node_version_caps_t *const c) {
    *c = (iotdata_node_version_caps_t){ 0 };
    if (buf == NULL || (len % 2u) != 0)
        return false;
    for (size_t i = 0; i + 1u < len; i += 2u) {
        const uint16_t e = (uint16_t)(((uint16_t)buf[i] << 8) | buf[i + 1u]);
        if (!iotdata_node_version_caps_add(c, (uint8_t)(e >> 12), (uint16_t)(e & IOTDATA_NODE_VERSION_CAPS_MASK_MAX)))
            return false;
    }
    return true;
}

static inline const char *iotdata_node_version_caps_str(const iotdata_node_version_caps_t *const c, char *const buf, const size_t size) {
    if (buf == NULL || size == 0)
        return buf;
    size_t n = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < c->count; i++) {
        const uint8_t key = (uint8_t)(c->entry[i] >> 12);
        const uint16_t mask = (uint16_t)(c->entry[i] & IOTDATA_NODE_VERSION_CAPS_MASK_MAX);
        const bool bare = (key == IOTDATA_NODE_VERSION_CAPS_FEATURES);
        for (uint16_t bit = 1; bit <= IOTDATA_NODE_VERSION_CAPS_MASK_MAX; bit = (uint16_t)(bit << 1))
            if ((mask & bit) != 0) {
                const char *const name = iotdata_node_version_caps_bit_name(key, bit);
                int w;
                if (name != NULL)
                    w = snprintf(buf + n, size - n, "%s%s%s%s", n > 0 ? "," : "", bare ? "" : iotdata_node_version_caps_name(key), bare ? "" : "/", name);
                else /* unnamed, including a proprietary category: say which bit, not a guess */
                    w = snprintf(buf + n, size - n, "%s%s/0x%03x", n > 0 ? "," : "", iotdata_node_version_caps_name(key), (unsigned)bit);
                if (w <= 0 || (size_t)w >= size - n)
                    return buf; /* out of room: what is there stays a valid string */
                n += (size_t)w;
            }
    }
    return buf;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * THE FOUR DETECTED ANSWERS COME FROM THE PLATFORM, and this header only asks.
 *
 * Each is a one-line hand-off to device/d_module_version_<platform>.h, which the application
 * includes beside its other d_ modules. Nothing here knows what a platform IS -- not even by an
 * #ifdef -- so a third platform is a new file in that directory and no change in this one.
 *
 * A build that includes no module still compiles and answers "unknown", because an unanswerable
 * question still has an answer: a node that cannot say what board it is says exactly that. BUT A
 * FORGOTTEN INCLUDE LOOKS IDENTICAL, and is the likelier cause -- a node reporting unknown/unknown
 * on hardware it plainly has is missing the module, not missing a capability.
 */
#if defined(D_MODULE_VERSION_PROVIDED)

static inline const char *iotdata_node_version_hardware(char *const buf, const size_t size) {
    return version_hardware(buf, size);
}
static inline const char *iotdata_node_version_firmware(char *const buf, const size_t size) {
    return version_firmware(buf, size);
}
static inline const char *iotdata_node_version_serial(char *const buf, const size_t size) {
    return version_serial(buf, size);
}
/* IOTDATA_NODE_VERSION_IMAGE_BYTES is the protocol's length and stays the protocol's: the module is
   handed a size and fills that much of its digest. */
static inline bool iotdata_node_version_image(uint8_t *const out, const size_t size) {
    return version_image(out, size);
}

#else

static inline const char *iotdata_node_version_hardware(char *const buf, const size_t size) {
    return snprintf_inline(buf, size, "unknown/unknown");
}
static inline const char *iotdata_node_version_firmware(char *const buf, const size_t size) {
    return snprintf_inline(buf, size, "unknown");
}
static inline const char *iotdata_node_version_serial(char *const buf, const size_t size) {
    return snprintf_inline(buf, size, "unknown");
}
static inline bool iotdata_node_version_image(__attribute__((unused)) uint8_t *const out, __attribute__((unused)) const size_t size) {
    return false;
}

#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------

/* The stamp, from whoever can actually say. A build-supplied define wins; failing that a platform
   may know (ESP-IDF's app descriptor is stamped at link, so it is always THIS build); failing
   both, say so rather than guess. */
/* A stamp is twelve digits or it is not a stamp. Checked because the ways of getting one wrong are
   all SILENT: a `$(shell date)` that failed passes an empty string, which is still "defined" and
   would render as `app/1.0.0/` with nothing after the slash; a truncated or mis-formatted one
   would sort wrongly forever, and sorting is the only thing the stamp is for. */
static inline bool _iotdata_node_version_stamp_valid(const char *const st) {
    if (st == NULL || strlen(st) != IOTDATA_NODE_VERSION_STAMP_LEN)
        return false;
    for (const char *p = st; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *_iotdata_node_version_stamp(char *const buf, const size_t size) {
#if defined(IOTDATA_NODE_VERSION_STAMP)
    if (!_iotdata_node_version_stamp_valid(snprintf_inline(buf, size, "%s", IOTDATA_NODE_VERSION_STAMP)))
        return snprintf_inline(buf, size, "%s", IOTDATA_NODE_VERSION_STAMP_NONE);
    else
        return buf;
#else
    /* Failing a build-supplied one, the PLATFORM may know -- an ESP-IDF app descriptor is stamped at
       link, so when a module answers it is always THIS build. Validated like the declared one: the
       two paths produce the same twelve digits or the same visible nothing. */
#if defined(D_MODULE_VERSION_PROVIDED)
    if (version_stamp(buf, size) && _iotdata_node_version_stamp_valid(buf))
        return buf;
#endif
    return snprintf_inline(buf, size, "%s", IOTDATA_NODE_VERSION_STAMP_NONE);
#endif
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_version_stamp_is_real(void) {
    char st[IOTDATA_NODE_VERSION_STAMP_LEN + 1];
    return strcmp(_iotdata_node_version_stamp(st, sizeof(st)), IOTDATA_NODE_VERSION_STAMP_NONE) != 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *iotdata_node_version_software(char *const buf, const size_t size) {
    char st[IOTDATA_NODE_VERSION_STAMP_LEN + 1];
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    version_append(buf, size, &n, IOTDATA_NODE_VERSION_APP);
    version_sep(buf, size, &n, '/');
    version_append(buf, size, &n, IOTDATA_NODE_VERSION_SEMVER);
    version_sep(buf, size, &n, '/');
    version_append(buf, size, &n, _iotdata_node_version_stamp(st, sizeof(st)));
    return buf;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *iotdata_node_version_image_hex(const uint8_t *const hash, const size_t len, char *const buf, const size_t size) {
    if (buf == NULL || size == 0)
        return "";
    buf[0] = '\0';
    if (hash == NULL || len != IOTDATA_NODE_VERSION_IMAGE_BYTES || size < (2u * len) + 1u)
        return buf;
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        n += (size_t)snprintf(buf + n, size - n, "%02x", hash[i]);
    return buf;
}

static inline const char *iotdata_node_version_image_str(char *const buf, const size_t size) {
    uint8_t hash[IOTDATA_NODE_VERSION_IMAGE_BYTES];
    if (!iotdata_node_version_image(hash, sizeof(hash))) {
        if (buf != NULL && size > 0)
            buf[0] = '\0';
        return buf;
    }
    return iotdata_node_version_image_hex(hash, sizeof(hash), buf, size);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const iotdata_node_version_caps_t *caps; /* NULL on a node with nothing to declare */
} iotdata_node_version_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *iotdata_node_version_str(char *const buf, const size_t size, const iotdata_node_version_t *const v) {
    char hw[IOTDATA_NODE_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_NODE_VERSION_FIRMWARE_MAX + 1], sw[IOTDATA_NODE_VERSION_SOFTWARE_MAX + 1], sn[IOTDATA_NODE_VERSION_SERIAL_MAX + 1], cp[IOTDATA_NODE_VERSION_CAPS_STR_MAX + 1],
        im[IOTDATA_NODE_VERSION_IMAGE_MAX + 1];
    const iotdata_node_version_caps_t *const caps = (v != NULL) ? v->caps : NULL;
    cp[0] = '\0';
    if (caps != NULL)
        (void)iotdata_node_version_caps_str(caps, cp, sizeof(cp));
    (void)iotdata_node_version_image_str(im, sizeof(im));
    return snprintf_inline(buf, size, "%s on %s [%s] sn=%s%s%s%s%s", iotdata_node_version_software(sw, sizeof(sw)), iotdata_node_version_hardware(hw, sizeof(hw)), iotdata_node_version_firmware(fw, sizeof(fw)),
                           iotdata_node_version_serial(sn, sizeof(sn)), im[0] != '\0' ? " img=" : "", im, cp[0] != '\0' ? " caps=" : "", cp);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The idx'th `sep`-delimited part of `s`, copied into `out`. Returns its length, or -1 if there is
   no such part. A part longer than the buffer is truncated rather than refused: these are values
   off a wire, and a reader wanting the first field should not be defeated by a malformed third. */
static inline int iotdata_node_version_part(const char *const s, const char sep, const int idx, char *const out, const size_t size) {
    if (out == NULL || size == 0)
        return -1;
    out[0] = '\0';
    if (s == NULL)
        return -1;
    int at = 0;
    const char *start = s;
    for (const char *p = s;; p++)
        if (*p == sep || *p == '\0') {
            if (at == idx) {
                size_t n = (size_t)(p - start);
                if (n > size - 1u)
                    n = size - 1u;
                memcpy(out, start, n);
                out[n] = '\0';
                return (int)n;
            }
            if (*p == '\0')
                return -1;
            at++;
            start = p + 1;
        }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* "app/semver/stamp". All three are required, so a value missing one is malformed rather than
   partially useful -- an app name with no stamp says nothing about which build it is. */
static inline bool iotdata_node_version_software_split(const char *const sw, char *const app, const size_t app_sz, char *const semver, const size_t semver_sz, char *const stamp, const size_t stamp_sz) {
    return iotdata_node_version_part(sw, '/', 0, app, app_sz) > 0 && iotdata_node_version_part(sw, '/', 1, semver, semver_sz) > 0 && iotdata_node_version_part(sw, '/', 2, stamp, stamp_sz) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* "board/arch", where the board is optional: a host that reports only its architecture is giving
   a true answer, just a less specific one, so a single part IS the arch. */
static inline bool iotdata_node_version_hardware_split(const char *const hw, char *const board, const size_t board_sz, char *const arch, const size_t arch_sz) {
    if (iotdata_node_version_part(hw, '/', 1, arch, arch_sz) > 0)
        return iotdata_node_version_part(hw, '/', 0, board, board_sz) > 0;
    if (board != NULL && board_sz > 0)
        board[0] = '\0';
    return iotdata_node_version_part(hw, '/', 0, arch, arch_sz) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* "stack/version[+low]" -- the low half is the bootloader or equivalent, and is absent more often
   than not (a platform whose module cannot see one reports none). */
static inline bool iotdata_node_version_firmware_split(const char *const fw, char *const stack, const size_t stack_sz, char *const version, const size_t version_sz, char *const low, const size_t low_sz) {
    if (low != NULL && low_sz > 0)
        low[0] = '\0';
    if (iotdata_node_version_part(fw, '/', 0, stack, stack_sz) <= 0)
        return false;
    char rest[IOTDATA_NODE_VERSION_FIRMWARE_MAX + 1];
    if (iotdata_node_version_part(fw, '/', 1, rest, sizeof(rest)) <= 0)
        return false;
    (void)iotdata_node_version_part(rest, '+', 1, low, low_sz); /* absent is normal, not a failure */
    return iotdata_node_version_part(rest, '+', 0, version, version_sz) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

static inline int iotdata_node_version_caps_key(const char *const name) {
    if (name != NULL)
        for (uint8_t k = 0; k < IOTDATA_NODE_VERSION_CAPS_COUNT; k++)
            if (strcmp(iotdata_node_version_caps_name(k), name) == 0)
                return (int)k;
    return -1;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline cJSON *iotdata_node_version_caps_to_json(const iotdata_node_version_caps_t *const caps) {
    cJSON *const o = cJSON_CreateObject();
    if (o == NULL || caps == NULL)
        return o;
    char names[IOTDATA_NODE_VERSION_CAPS_STR_MAX + 1];
    cJSON_AddStringToObject(o, "names", iotdata_node_version_caps_str(caps, names, sizeof(names)));
    cJSON *const arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < caps->count; i++) {
        cJSON *const e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "category", iotdata_node_version_caps_name((uint8_t)(caps->entry[i] >> 12)));
        cJSON_AddNumberToObject(e, "mask", (double)(caps->entry[i] & IOTDATA_NODE_VERSION_CAPS_MASK_MAX));
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "entries", arr);
    return o;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_version_caps_from_json(const cJSON *const obj, iotdata_node_version_caps_t *const caps) {
    *caps = (iotdata_node_version_caps_t){ 0 };
    if (obj == NULL)
        return false;
    const cJSON *const arr = cJSON_GetObjectItemCaseSensitive(obj, "entries");
    if (!cJSON_IsArray(arr))
        return false;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        const cJSON *const cat = cJSON_GetObjectItemCaseSensitive(e, "category");
        const cJSON *const mask = cJSON_GetObjectItemCaseSensitive(e, "mask");
        if (cJSON_IsString(cat) && cJSON_IsNumber(mask)) {
            const int key = iotdata_node_version_caps_key(cat->valuestring);
            if (key >= 0)
                (void)iotdata_node_version_caps_add(caps, (uint8_t)key, (uint16_t)mask->valuedouble);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_version_json_key(cJSON *const obj, const char *const name, const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    if (key == IOTDATA_NODE_VERSION_IMAGE) {
        char hex[IOTDATA_NODE_VERSION_IMAGE_MAX + 1];
        if (vlen != IOTDATA_NODE_VERSION_IMAGE_BYTES)
            cJSON_AddStringToObject(obj, name, "malformed"); /* the wrong width is not a short hash */
        else
            cJSON_AddStringToObject(obj, name, iotdata_node_version_image_hex(val, vlen, hex, sizeof(hex)));
        return true;
    }
    if (key != IOTDATA_NODE_VERSION_CAPABILITIES)
        return false;
    iotdata_node_version_caps_t caps;
    if (iotdata_node_version_caps_parse(val, vlen, &caps))
        cJSON_AddItemToObject(obj, name, iotdata_node_version_caps_to_json(&caps));
    else /* an odd length is malformed, and saying so beats rendering a guess */
        cJSON_AddStringToObject(obj, name, "malformed");
    return true;
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_version_pack(iotdata_kvr_t *const kv, const iotdata_node_version_t *const v) {
    char hw[IOTDATA_NODE_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_NODE_VERSION_FIRMWARE_MAX + 1], sw[IOTDATA_NODE_VERSION_SOFTWARE_MAX + 1], sn[IOTDATA_NODE_VERSION_SERIAL_MAX + 1];
    uint8_t im[IOTDATA_NODE_VERSION_IMAGE_BYTES];
    const iotdata_node_version_caps_t *const caps = (v != NULL) ? v->caps : NULL;
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_HARDWARE, iotdata_node_version_hardware(hw, sizeof(hw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_FIRMWARE, iotdata_node_version_firmware(fw, sizeof(fw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_SOFTWARE, iotdata_node_version_software(sw, sizeof(sw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_SERIAL, iotdata_node_version_serial(sn, sizeof(sn)));
    if (caps != NULL && caps->count > 0) {
        uint8_t packed[IOTDATA_NODE_VERSION_CAPS_COUNT * 2];
        const int n = iotdata_node_version_caps_pack(caps, packed, sizeof(packed));
        if (n > 0)
            iotdata_kvr_add(kv, IOTDATA_NODE_VERSION_CAPABILITIES, packed, (uint8_t)n);
    }
    if (iotdata_node_version_image(im, sizeof(im)))
        iotdata_kvr_add(kv, IOTDATA_NODE_VERSION_IMAGE, im, (uint8_t)sizeof(im));
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_VERSION_H */
