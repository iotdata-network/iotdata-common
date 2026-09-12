#ifndef IOTDATA_NODE_VERSION_H
#define IOTDATA_NODE_VERSION_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_version.h - what a node says it is: the single source of truth for VERSION.
//
// An application includes this, declares its name and release line, and gets the whole VERSION
// TLV and a printable line for its console. It should not assemble any of these strings itself --
// the point of one header is that every node in the fleet answers the same question the same way,
// and that a reader can parse one grammar rather than guessing per device.
//
// FOUR BUCKETS AND AN INVENTORY. The key space and the grammars are defined in iotdata_node.h, at
// the VERSION keys; the reasoning for the split is there too. This header owns the ENCODING and
// the PER-PLATFORM DETECTION:
//
//   hardware      board/arch            detected: chip on ESP-IDF, device-tree or DMI on Linux
//   firmware      stack/version[+low]   detected: IDF + bootloader, or kernel
//   software      app/semver/stamp      declared: IOTDATA_VERSION_APP / _SEMVER / _STAMP
//   serial        hex                   detected: eFuse MAC, cpu serial, machine-id
//   capabilities  16-bit entries        declared by the app, seeded from build flags
//
// FORMATS, NEVER LOGS. Every function here fills a buffer the caller owns and hands it back; where
// it goes is the application's business, because an ESP-IDF app wants ESP_LOGI, a Linux daemon
// wants PRINTF_INFO, and a serial command wants a tagged emit callback. Same rule as d_format.h,
// and it is what makes this header shareable between them.
//
// THE STAMP MUST BE REAL. IOTDATA_VERSION_STAMP comes from the build, regenerated every time, and
// a build that does not set it reports IOTDATA_VERSION_STAMP_NONE so the omission is visible
// rather than silently inherited from whenever the tree was last touched. A stamp that lies is
// worse than no stamp: it is how an hour goes into debugging a binary that is not the one running.
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

#ifndef IOTDATA_VERSION_APP
#define IOTDATA_VERSION_APP "unknown"
#endif

/* Normative and hand-set at release points, NOT the ordering key -- see iotdata_node.h. A dev
   build still tracks a release line, so this is always present rather than optional. */
#ifndef IOTDATA_VERSION_SEMVER
#define IOTDATA_VERSION_SEMVER "0.0.0"
#endif

/* yyyymmddhhmm, and the ONLY thing here that anything should compare. A build sets it directly --
   on a host that is one `date -u` in the Makefile, regenerated every invocation. Where the build
   system cannot conveniently supply one, a platform may derive it instead: ESP-IDF stamps its app
   descriptor at every link, which is a better source than a define because it cannot be forgotten
   or left behind by an incremental build. Unset and underivable, it reads as _NONE, so the
   omission is visible rather than silently inherited from whenever the tree was last touched. */
#define IOTDATA_VERSION_STAMP_NONE         "000000000000"
#define IOTDATA_VERSION_STAMP_LEN          12

/* Field sizes in CHARACTERS -- a buffer for one is declared [SIZE + 1] for the terminator, so
   these read as the lengths they are rather than as one less than they look. Generous against the grammars (a `board/arch` runs ~15, a `software` ~24) and cheap:
   the whole TLV is ~80 bytes of kvr, one frame, sent at startup and on request. */
#define IOTDATA_VERSION_HARDWARE_MAX       24
#define IOTDATA_VERSION_FIRMWARE_MAX       24
#define IOTDATA_VERSION_SOFTWARE_MAX       48
#define IOTDATA_VERSION_SERIAL_MAX         32 /* a machine-id is 32 hex, and an identity must never be truncated */
#define IOTDATA_VERSION_CAPS_STR_MAX       96
#define IOTDATA_VERSION_STR_MAX            256

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

#define IOTDATA_VERSION_CAP_RADIO          0x0
#define IOTDATA_VERSION_CAP_DISPLAY        0x1
#define IOTDATA_VERSION_CAP_MODEM          0x2
#define IOTDATA_VERSION_CAP_SENSOR         0x3
#define IOTDATA_VERSION_CAP_STORAGE        0x4
#define IOTDATA_VERSION_CAP_POWER          0x5
#define IOTDATA_VERSION_CAP_FEATURES       0x6
/* 0x7 unassigned; 0x8..0xF proprietary (bit 3) */
#define IOTDATA_VERSION_CAP_PROPRIETARY    0x8
#define IOTDATA_VERSION_CAP_COUNT          16
#define IOTDATA_VERSION_CAP_MASK_MAX       0x0FFFu

#define IOTDATA_VERSION_RADIO_E22_DIP      0x001
#define IOTDATA_VERSION_RADIO_E22_USB      0x002
#define IOTDATA_VERSION_RADIO_SX1302       0x004
#define IOTDATA_VERSION_RADIO_RAK3272      0x008

#define IOTDATA_VERSION_DISPLAY_ILI9488    0x001
#define IOTDATA_VERSION_DISPLAY_SSD1306    0x002

#define IOTDATA_VERSION_MODEM_USB_CDC      0x001

#define IOTDATA_VERSION_SENSOR_BME280      0x001
#define IOTDATA_VERSION_SENSOR_SDS         0x002
#define IOTDATA_VERSION_SENSOR_TSA         0x004
#define IOTDATA_VERSION_SENSOR_WIND        0x008
#define IOTDATA_VERSION_SENSOR_SOLAR       0x010

#define IOTDATA_VERSION_FEATURE_MESH       0x001
#define IOTDATA_VERSION_FEATURE_BLACKBOX   0x002
#define IOTDATA_VERSION_FEATURE_OTA        0x004
#define IOTDATA_VERSION_FEATURE_JSON       0x008
#define IOTDATA_VERSION_FEATURE_FLOAT      0x010
#define IOTDATA_VERSION_FEATURE_ENCRYPTION 0x020

typedef struct {
    uint16_t entry[IOTDATA_VERSION_CAP_COUNT]; /* at most one per category before OR-merging */
    uint8_t count;
} iotdata_version_caps_t;

static inline const char *iotdata_version_cap_name(const uint8_t key) {
    switch (key) {
    case IOTDATA_VERSION_CAP_RADIO:
        return "radio";
    case IOTDATA_VERSION_CAP_DISPLAY:
        return "display";
    case IOTDATA_VERSION_CAP_MODEM:
        return "modem";
    case IOTDATA_VERSION_CAP_SENSOR:
        return "sensor";
    case IOTDATA_VERSION_CAP_STORAGE:
        return "storage";
    case IOTDATA_VERSION_CAP_POWER:
        return "power";
    case IOTDATA_VERSION_CAP_FEATURES:
        return "features";
    default:
        return (key & IOTDATA_VERSION_CAP_PROPRIETARY) != 0 ? "proprietary" : "reserved";
    }
}

static inline const char *iotdata_version_cap_bit_name(const uint8_t key, const uint16_t bit) {
    switch (key) {
    case IOTDATA_VERSION_CAP_RADIO:
        switch (bit) {
        case IOTDATA_VERSION_RADIO_E22_DIP:
            return "e22-dip";
        case IOTDATA_VERSION_RADIO_E22_USB:
            return "e22-usb";
        case IOTDATA_VERSION_RADIO_SX1302:
            return "sx1302";
        case IOTDATA_VERSION_RADIO_RAK3272:
            return "rak3272";
        default:
            return NULL;
        }
    case IOTDATA_VERSION_CAP_DISPLAY:
        switch (bit) {
        case IOTDATA_VERSION_DISPLAY_ILI9488:
            return "ili9488";
        case IOTDATA_VERSION_DISPLAY_SSD1306:
            return "ssd1306";
        default:
            return NULL;
        }
    case IOTDATA_VERSION_CAP_MODEM:
        return bit == IOTDATA_VERSION_MODEM_USB_CDC ? "usb-cdc" : NULL;
    case IOTDATA_VERSION_CAP_SENSOR:
        switch (bit) {
        case IOTDATA_VERSION_SENSOR_BME280:
            return "bme280";
        case IOTDATA_VERSION_SENSOR_SDS:
            return "sds";
        case IOTDATA_VERSION_SENSOR_TSA:
            return "tsa";
        case IOTDATA_VERSION_SENSOR_WIND:
            return "wind";
        case IOTDATA_VERSION_SENSOR_SOLAR:
            return "solar";
        default:
            return NULL;
        }
    case IOTDATA_VERSION_CAP_FEATURES:
        switch (bit) {
        case IOTDATA_VERSION_FEATURE_MESH:
            return "mesh";
        case IOTDATA_VERSION_FEATURE_BLACKBOX:
            return "bbox";
        case IOTDATA_VERSION_FEATURE_OTA:
            return "ota";
        case IOTDATA_VERSION_FEATURE_JSON:
            return "json";
        case IOTDATA_VERSION_FEATURE_FLOAT:
            return "float";
        case IOTDATA_VERSION_FEATURE_ENCRYPTION:
            return "crypt";
        default:
            return NULL;
        }
    default:
        return NULL;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_version_caps_init(iotdata_version_caps_t *const c) {
    memset(c, 0, sizeof(*c));
    uint16_t f = 0;
#if !defined(IOTDATA_NO_JSON)
    f |= IOTDATA_VERSION_FEATURE_JSON;
#endif
#if !defined(IOTDATA_NO_FLOATING)
    f |= IOTDATA_VERSION_FEATURE_FLOAT;
#endif
#if defined(IOTDATA_VERSION_HAS_MESH)
    f |= IOTDATA_VERSION_FEATURE_MESH;
#endif
#if defined(IOTDATA_VERSION_HAS_BLACKBOX)
    f |= IOTDATA_VERSION_FEATURE_BLACKBOX;
#endif
#if defined(IOTDATA_VERSION_HAS_OTA)
    f |= IOTDATA_VERSION_FEATURE_OTA;
#endif
    if (f != 0) {
        c->entry[0] = (uint16_t)((IOTDATA_VERSION_CAP_FEATURES << 12) | f);
        c->count = 1;
    }
}

/* OR-merges into an existing category rather than adding a second entry, so a caller can declare
   capabilities from several places without having to collect them first. */
static inline bool iotdata_version_caps_add(iotdata_version_caps_t *const c, const uint8_t key, const uint16_t mask) {
    if (key >= IOTDATA_VERSION_CAP_COUNT || (mask & ~IOTDATA_VERSION_CAP_MASK_MAX) != 0 || mask == 0)
        return false;
    for (uint8_t i = 0; i < c->count; i++)
        if ((uint8_t)(c->entry[i] >> 12) == key) {
            c->entry[i] |= (uint16_t)(mask & IOTDATA_VERSION_CAP_MASK_MAX);
            return true;
        }
    if (c->count >= IOTDATA_VERSION_CAP_COUNT)
        return false;
    c->entry[c->count++] = (uint16_t)(((uint16_t)key << 12) | (mask & IOTDATA_VERSION_CAP_MASK_MAX));
    return true;
}

static inline uint16_t iotdata_version_caps_get(const iotdata_version_caps_t *const c, const uint8_t key) {
    for (uint8_t i = 0; i < c->count; i++)
        if ((uint8_t)(c->entry[i] >> 12) == key)
            return (uint16_t)(c->entry[i] & IOTDATA_VERSION_CAP_MASK_MAX);
    return 0;
}

static inline int iotdata_version_caps_pack(const iotdata_version_caps_t *const c, uint8_t *const buf, const size_t size) {
    if (buf == NULL || size < (size_t)c->count * 2u)
        return -1;
    for (uint8_t i = 0; i < c->count; i++) { /* big-endian, as everything else on this wire is */
        buf[i * 2u] = (uint8_t)(c->entry[i] >> 8);
        buf[i * 2u + 1u] = (uint8_t)c->entry[i];
    }
    return (int)c->count * 2;
}

static inline bool iotdata_version_caps_parse(const uint8_t *const buf, const size_t len, iotdata_version_caps_t *const c) {
    memset(c, 0, sizeof(*c));
    if (buf == NULL || (len % 2u) != 0)
        return false;
    for (size_t i = 0; i + 1u < len; i += 2u) {
        const uint16_t e = (uint16_t)(((uint16_t)buf[i] << 8) | buf[i + 1u]);
        if (!iotdata_version_caps_add(c, (uint8_t)(e >> 12), (uint16_t)(e & IOTDATA_VERSION_CAP_MASK_MAX)))
            return false;
    }
    return true;
}

static inline const char *iotdata_version_caps_str(const iotdata_version_caps_t *const c, char *const buf, const size_t size) {
    if (buf == NULL || size == 0)
        return buf;
    size_t n = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < c->count; i++) {
        const uint8_t key = (uint8_t)(c->entry[i] >> 12);
        const uint16_t mask = (uint16_t)(c->entry[i] & IOTDATA_VERSION_CAP_MASK_MAX);
        const bool bare = (key == IOTDATA_VERSION_CAP_FEATURES);
        for (uint16_t bit = 1; bit <= IOTDATA_VERSION_CAP_MASK_MAX; bit = (uint16_t)(bit << 1)) {
            if ((mask & bit) != 0) {
                const char *const name = iotdata_version_cap_bit_name(key, bit);
                int w;
                if (name != NULL)
                    w = snprintf(buf + n, size - n, "%s%s%s%s", n > 0 ? "," : "", bare ? "" : iotdata_version_cap_name(key), bare ? "" : "/", name);
                else /* unnamed, including a proprietary category: say which bit, not a guess */
                    w = snprintf(buf + n, size - n, "%s%s/0x%03x", n > 0 ? "," : "", iotdata_version_cap_name(key), (unsigned)bit);
                if (w <= 0 || (size_t)w >= size - n)
                    return buf; /* out of room: what is there stays a valid string */
                n += (size_t)w;
            }
        }
    }
    return buf;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_version_append(char *const dst, const size_t size, size_t *const n, const char *const src) {
    if (dst == NULL || size == 0)
        return;
    for (const char *p = src; p != NULL && *p != '\0' && *n + 1u < size; p++)
        dst[(*n)++] = (*p == '/' || *p == '+' || *p == ',' || *p == ' ' || *p < 0x20) ? '-' : ((*p >= 'A' && *p <= 'Z') ? (char)(*p + ('a' - 'A')) : *p);
    dst[*n] = '\0';
}

static inline void _iotdata_version_sep(char *const dst, const size_t size, size_t *const n, const char ch) {
    if (dst == NULL || size == 0 || *n + 1u >= size)
        return;
    dst[(*n)++] = ch;
    dst[*n] = '\0';
}

/*
 * ESP-IDF COMPONENTS THIS NEEDS. IDF only puts a component's headers on the include path when the
 * component is declared, so a project including this header must list these in its main
 * CMakeLists.txt -- otherwise the build fails on a missing header, or worse, quietly loses a field:
 *
 *     PRIV_REQUIRES ... esp_hw_support esp_app_format esp_bootloader_format
 *
 *   esp_hw_support          esp_chip_info.h, esp_mac.h  -> hardware, serial
 *   esp_app_format          esp_app_desc.h              -> firmware (the IDF version)
 *   esp_bootloader_format   esp_bootloader_desc.h       -> firmware's "+bl" half
 *
 * The bootloader one is the quiet case: it is probed with __has_include, so a project that omits
 * the component still compiles and simply reports no bootloader version. That is how `idf/6.1`
 * appeared where `idf/6.1+bl1` was expected.
 */
#if defined(PLATFORM_ESP32)

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_system.h"
/* The bootloader descriptor is what supplies the [+low] half of `firmware`, and it arrived in a
   later IDF -- so it is probed rather than assumed. Without this include the guard below would be
   false for the wrong reason and the bootloader would be silently absent from every report. */
#if defined(__has_include)
#if __has_include("esp_bootloader_desc.h")
#include "esp_bootloader_desc.h"
#endif
#endif

static inline const char *iotdata_version_hardware(char *const buf, const size_t size) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char *chip = "esp32", *arch = "xtensa";
    switch (info.model) {
    case CHIP_ESP32:
        chip = "esp32";
        break;
    case CHIP_ESP32S2:
        chip = "esp32s2";
        break;
    case CHIP_ESP32S3:
        chip = "esp32s3";
        break;
    case CHIP_ESP32C3:
        chip = "esp32c3";
        arch = "riscv32";
        break;
    default:
        break;
    }
#if defined(CONFIG_IDF_TARGET_ARCH_RISCV)
    arch = "riscv32";
#endif
    (void)snprintf(buf, size, "%s/%s", chip, arch);
    return buf;
}

/* IDF version plus the bootloader, which is the ESP32's genuine "firmware beneath the app" -- the
   thing that is flashed separately and outlives an OTA. */
static inline const char *iotdata_version_firmware(char *const buf, const size_t size) {
    const esp_app_desc_t *const app = esp_app_get_description();
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    _iotdata_version_append(buf, size, &n, "idf");
    _iotdata_version_sep(buf, size, &n, '/');
    /* idf_ver is "v6.1" or "v6.1-dev-1234-g..." -- the leading 'v' is noise next to a field that
       already says which stack this is, and everything from the first non-version character is
       provenance rather than version */
    const char *v = (app != NULL && app->idf_ver[0] != '\0') ? app->idf_ver : "unknown";
    if (*v == 'v' || *v == 'V')
        v++;
    char ver[16];
    size_t r = 0;
    for (; *v != '\0' && r + 1u < sizeof(ver); v++) {
        if ((*v < '0' || *v > '9') && *v != '.')
            break;
        ver[r++] = *v;
    }
    ver[r] = '\0';
    _iotdata_version_append(buf, size, &n, r > 0 ? ver : "unknown");
#if defined(ESP_BOOTLOADER_DESC_MAGIC_WORD)
    /* the bootloader is the [+low] part: flashed separately, and it outlives an OTA */
    const esp_bootloader_desc_t *const bl = esp_bootloader_get_description();
    if (bl != NULL) {
        char blv[12];
        (void)snprintf(blv, sizeof(blv), "bl%u", (unsigned)bl->version);
        _iotdata_version_sep(buf, size, &n, '+');
        _iotdata_version_append(buf, size, &n, blv);
    }
#endif
    return buf;
}

static inline const char *iotdata_version_serial(char *const buf, const size_t size) {
    uint8_t mac[6] = { 0 };
    /* the eFuse MAC: burned in at manufacture, so it survives a reflash and there is no removable
       interface it can be confused with */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        (void)memset(mac, 0, sizeof(mac));
    (void)snprintf(buf, size, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

#elif defined(PLATFORM_LINUX)

#include <sys/utsname.h>

static inline bool _iotdata_version_slurp(const char *const path, char *const dst, const size_t size) {
    FILE *const f = fopen(path, "r");
    if (f == NULL)
        return false;
    char raw[128];
    const bool got = fgets(raw, (int)sizeof(raw), f) != NULL;
    (void)fclose(f);
    if (!got)
        return false;
    raw[strcspn(raw, "\r\n")] = '\0';
    if (raw[0] == '\0')
        return false;
    size_t n = 0;
    dst[0] = '\0';
    _iotdata_version_append(dst, size, &n, raw);
    return true;
}

/* Board from the device tree where there is one (Pi, Rock) and from DMI on a PC, falling back to
   the architecture alone -- which is still true, just less specific. */
static inline const char *iotdata_version_hardware(char *const buf, const size_t size) {
    struct utsname u;
    const char *arch = (uname(&u) == 0) ? u.machine : "unknown";
    char board[24] = { 0 };
    if (!_iotdata_version_slurp("/proc/device-tree/model", board, sizeof(board)) && !_iotdata_version_slurp("/sys/class/dmi/id/product_name", board, sizeof(board)))
        board[0] = '\0';
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    if (board[0] != '\0') {
        _iotdata_version_append(buf, size, &n, board);
        _iotdata_version_sep(buf, size, &n, '/');
    }
    _iotdata_version_append(buf, size, &n, arch);
    return buf;
}

/* The kernel is what runs beneath the application here. The Pi's VideoCore blob is the closer
   analogue of a bootloader, but vcgencmd is a binary a slimmed image may not carry, so it is not
   worth a fork per boot -- kernel alone, and the board string already says it is a Pi. */
static inline const char *iotdata_version_firmware(char *const buf, const size_t size) {
    struct utsname u;
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    _iotdata_version_append(buf, size, &n, "linux");
    _iotdata_version_sep(buf, size, &n, '/');
    if (uname(&u) != 0) {
        _iotdata_version_append(buf, size, &n, "unknown");
        return buf;
    }
    /* The kernel VERSION only: `uname -r` is "6.12.96+deb13-amd64" on Debian, and everything from
       the first non-version character is packaging rather than kernel. Dropping it matters twice
       over -- it is noise, and that '+' is this grammar's own separator for the [+low] field, so
       carrying it through would make the value parse as a bootloader that does not exist. */
    char rel[20];
    size_t r = 0;
    for (const char *p = u.release; *p != '\0' && r + 1u < sizeof(rel); p++) {
        if ((*p < '0' || *p > '9') && *p != '.')
            break;
        rel[r++] = *p;
    }
    rel[r] = '\0';
    _iotdata_version_append(buf, size, &n, r > 0 ? rel : "unknown");
    return buf;
}

/* NEVER a NIC MAC: those are removable, and a box whose identity changes when a card is swapped
   cannot be tracked across the swap. The CPU serial is the board; machine-id is the install. */
static inline const char *iotdata_version_serial(char *const buf, const size_t size) {
    char raw[64] = { 0 };
    FILE *const f = fopen("/proc/cpuinfo", "r");
    if (f != NULL) {
        char line[160];
        while (fgets(line, (int)sizeof(line), f) != NULL)
            if (strncmp(line, "Serial", 6) == 0) {
                const char *const colon = strchr(line, ':');
                if (colon != NULL) {
                    const char *v = colon + 1;
                    while (*v == ' ' || *v == '\t')
                        v++;
                    size_t rn = 0;
                    raw[0] = '\0';
                    _iotdata_version_append(raw, sizeof(raw), &rn, v);
                }
                break;
            }
        (void)fclose(f);
    }
    if (raw[0] == '\0' && !_iotdata_version_slurp("/proc/device-tree/serial-number", raw, sizeof(raw)))
        (void)_iotdata_version_slurp("/etc/machine-id", raw, sizeof(raw));
    /* trim leading zeros a Pi pads its serial with, but never to nothing */
    const char *p = raw;
    while (*p == '0' && *(p + 1) != '\0')
        p++;
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    _iotdata_version_append(buf, size, &n, p[0] != '\0' ? p : "unknown");
    return buf;
}

#else

static inline const char *iotdata_version_hardware(char *const buf, const size_t size) {
    (void)snprintf(buf, size, "unknown/unknown");
    return buf;
}
static inline const char *iotdata_version_firmware(char *const buf, const size_t size) {
    (void)snprintf(buf, size, "unknown");
    return buf;
}
static inline const char *iotdata_version_serial(char *const buf, const size_t size) {
    (void)snprintf(buf, size, "unknown");
    return buf;
}

#endif

/* Three letters to a month number, for the one place a C build hands over a date as English. */
static inline int _iotdata_version_month(const char *const mon) {
    static const char names[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    char low[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 3 && mon[i] != '\0'; i++)
        low[i] = (mon[i] >= 'A' && mon[i] <= 'Z') ? (char)(mon[i] + ('a' - 'A')) : mon[i];
    for (int m = 0; m < 12; m++)
        if (strncmp(names + m * 3, low, 3) == 0)
            return m + 1;
    return 0;
}

/* The stamp, from whoever can actually say. A build-supplied define wins; failing that a platform
   may know (ESP-IDF's app descriptor is stamped at link, so it is always THIS build); failing
   both, say so rather than guess. */
/* A stamp is twelve digits or it is not a stamp. Checked because the ways of getting one wrong are
   all SILENT: a `$(shell date)` that failed passes an empty string, which is still "defined" and
   would render as `app/1.0.0/` with nothing after the slash; a truncated or mis-formatted one
   would sort wrongly forever, and sorting is the only thing the stamp is for. */
static inline bool _iotdata_version_stamp_valid(const char *const st) {
    if (st == NULL || strlen(st) != IOTDATA_VERSION_STAMP_LEN)
        return false;
    for (const char *p = st; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

static inline const char *_iotdata_version_stamp(char *const buf, const size_t size) {
#if defined(IOTDATA_VERSION_STAMP)
    (void)snprintf(buf, size, "%s", IOTDATA_VERSION_STAMP);
    if (!_iotdata_version_stamp_valid(buf))
        (void)snprintf(buf, size, "%s", IOTDATA_VERSION_STAMP_NONE);
#elif defined(PLATFORM_ESP32)
    const esp_app_desc_t *const app = esp_app_get_description();
    int mday = 0, year = 0, hour = 0, minute = 0;
    /* app->date is __DATE__, "Sep 12 2026", with the day space-padded; app->time is "17:45:09" */
    const int m = (app != NULL) ? _iotdata_version_month(app->date) : 0;
    if (m > 0 && sscanf(app->date + 3, "%d %d", &mday, &year) == 2 && sscanf(app->time, "%d:%d", &hour, &minute) == 2)
        (void)snprintf(buf, size, "%04d%02d%02d%02d%02d", year, m, mday, hour, minute);
    else
        (void)snprintf(buf, size, "%s", IOTDATA_VERSION_STAMP_NONE);
#else
    (void)snprintf(buf, size, "%s", IOTDATA_VERSION_STAMP_NONE);
#endif
    return buf;
}

static inline bool iotdata_version_stamp_is_real(void) {
    char st[IOTDATA_VERSION_STAMP_LEN + 1];
    return strcmp(_iotdata_version_stamp(st, sizeof(st)), IOTDATA_VERSION_STAMP_NONE) != 0;
}

static inline const char *iotdata_version_software(char *const buf, const size_t size) {
    char st[IOTDATA_VERSION_STAMP_LEN + 1];
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    _iotdata_version_append(buf, size, &n, IOTDATA_VERSION_APP);
    _iotdata_version_sep(buf, size, &n, '/');
    _iotdata_version_append(buf, size, &n, IOTDATA_VERSION_SEMVER);
    _iotdata_version_sep(buf, size, &n, '/');
    _iotdata_version_append(buf, size, &n, _iotdata_version_stamp(st, sizeof(st)));
    return buf;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *iotdata_version_str(char *const buf, const size_t size, const iotdata_version_caps_t *const caps) {
    char hw[IOTDATA_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_VERSION_FIRMWARE_MAX + 1], sw[IOTDATA_VERSION_SOFTWARE_MAX + 1], sn[IOTDATA_VERSION_SERIAL_MAX + 1], cp[IOTDATA_VERSION_CAPS_STR_MAX + 1];
    cp[0] = '\0';
    if (caps != NULL)
        (void)iotdata_version_caps_str(caps, cp, sizeof(cp));
    (void)snprintf(buf, size, "%s on %s [%s] sn=%s%s%s", iotdata_version_software(sw, sizeof(sw)), iotdata_version_hardware(hw, sizeof(hw)), iotdata_version_firmware(fw, sizeof(fw)), iotdata_version_serial(sn, sizeof(sn)),
                   cp[0] != '\0' ? " caps=" : "", cp);
    return buf;
}

/* The TLV packer needs the kvr writer, so it appears only where the node protocol has been
   included. Everything above is pure formatting and needs nothing -- which is what lets a host
   tool include this header on its own just to say what it is. */
// -----------------------------------------------------------------------------------------------------------------------------------------
// TAKING THEM APART AGAIN
//
// The grammars are defined here, so the splitters belong here too -- otherwise every reader
// reimplements them and they drift. All three use '/' as the separator, which is what lets one
// helper serve them all.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The idx'th `sep`-delimited part of `s`, copied into `out`. Returns its length, or -1 if there is
   no such part. A part longer than the buffer is truncated rather than refused: these are values
   off a wire, and a reader wanting the first field should not be defeated by a malformed third. */
static inline int iotdata_version_part(const char *const s, const char sep, const int idx, char *const out, const size_t size) {
    if (out == NULL || size == 0)
        return -1;
    out[0] = '\0';
    if (s == NULL)
        return -1;
    int at = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p != sep && *p != '\0')
            continue;
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

/* "app/semver/stamp". All three are required, so a value missing one is malformed rather than
   partially useful -- an app name with no stamp says nothing about which build it is. */
static inline bool iotdata_version_software_split(const char *const sw, char *const app, const size_t app_sz, char *const semver, const size_t semver_sz, char *const stamp, const size_t stamp_sz) {
    return iotdata_version_part(sw, '/', 0, app, app_sz) > 0 && iotdata_version_part(sw, '/', 1, semver, semver_sz) > 0 && iotdata_version_part(sw, '/', 2, stamp, stamp_sz) > 0;
}

/* "board/arch", where the board is optional: a host that reports only its architecture is giving
   a true answer, just a less specific one, so a single part IS the arch. */
static inline bool iotdata_version_hardware_split(const char *const hw, char *const board, const size_t board_sz, char *const arch, const size_t arch_sz) {
    if (iotdata_version_part(hw, '/', 1, arch, arch_sz) > 0)
        return iotdata_version_part(hw, '/', 0, board, board_sz) > 0;
    if (board != NULL && board_sz > 0)
        board[0] = '\0';
    return iotdata_version_part(hw, '/', 0, arch, arch_sz) > 0;
}

/* "stack/version[+low]" -- the low half is the bootloader or equivalent, and is absent more often
   than not (a project that did not declare esp_bootloader_format reports none). */
static inline bool iotdata_version_firmware_split(const char *const fw, char *const stack, const size_t stack_sz, char *const version, const size_t version_sz, char *const low, const size_t low_sz) {
    if (low != NULL && low_sz > 0)
        low[0] = '\0';
    if (iotdata_version_part(fw, '/', 0, stack, stack_sz) <= 0)
        return false;
    char rest[IOTDATA_VERSION_FIRMWARE_MAX + 1];
    if (iotdata_version_part(fw, '/', 1, rest, sizeof(rest)) <= 0)
        return false;
    (void)iotdata_version_part(rest, '+', 1, low, low_sz); /* absent is normal, not a failure */
    return iotdata_version_part(rest, '+', 0, version, version_sz) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

/* The reverse of iotdata_version_cap_name, for reading a report back. -1 if unknown. */
static inline int iotdata_version_cap_key(const char *const name) {
    if (name != NULL)
        for (uint8_t k = 0; k < IOTDATA_VERSION_CAP_COUNT; k++)
            if (strcmp(iotdata_version_cap_name(k), name) == 0)
                return (int)k;
    return -1;
}

static inline cJSON *iotdata_version_caps_to_json(const iotdata_version_caps_t *const caps) {
    cJSON *const o = cJSON_CreateObject();
    if (o == NULL || caps == NULL)
        return o;
    char names[IOTDATA_VERSION_CAPS_STR_MAX + 1];
    cJSON_AddStringToObject(o, "names", iotdata_version_caps_str(caps, names, sizeof(names)));
    cJSON *const arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < caps->count; i++) {
        cJSON *const e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "category", iotdata_version_cap_name((uint8_t)(caps->entry[i] >> 12)));
        cJSON_AddNumberToObject(e, "mask", (double)(caps->entry[i] & IOTDATA_VERSION_CAP_MASK_MAX));
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "entries", arr);
    return o;
}

static inline bool iotdata_version_caps_from_json(const cJSON *const obj, iotdata_version_caps_t *const caps) {
    memset(caps, 0, sizeof(*caps));
    if (obj == NULL)
        return false;
    const cJSON *const arr = cJSON_GetObjectItemCaseSensitive(obj, "entries");
    if (!cJSON_IsArray(arr))
        return false;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        const cJSON *const cat = cJSON_GetObjectItemCaseSensitive(e, "category");
        const cJSON *const mask = cJSON_GetObjectItemCaseSensitive(e, "mask");
        if (!cJSON_IsString(cat) || !cJSON_IsNumber(mask))
            continue;
        const int key = iotdata_version_cap_key(cat->valuestring);
        if (key >= 0)
            (void)iotdata_version_caps_add(caps, (uint8_t)key, (uint16_t)mask->valuedouble);
    }
    return true;
}

static inline bool iotdata_version_json_key(cJSON *const obj, const char *const name, const uint8_t key, const uint8_t *const val, const uint8_t vlen) {
    if (key != IOTDATA_NODE_VERSION_CAPABILITIES)
        return false;
    iotdata_version_caps_t caps;
    if (iotdata_version_caps_parse(val, vlen, &caps))
        cJSON_AddItemToObject(obj, name, iotdata_version_caps_to_json(&caps));
    else /* an odd length is malformed, and saying so beats rendering a guess */
        cJSON_AddStringToObject(obj, name, "malformed");
    return true;
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_version_pack(iotdata_kvr_t *const kv, const iotdata_version_caps_t *const caps) {
    char hw[IOTDATA_VERSION_HARDWARE_MAX + 1], fw[IOTDATA_VERSION_FIRMWARE_MAX + 1], sw[IOTDATA_VERSION_SOFTWARE_MAX + 1], sn[IOTDATA_VERSION_SERIAL_MAX + 1];
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_HARDWARE, iotdata_version_hardware(hw, sizeof(hw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_FIRMWARE, iotdata_version_firmware(fw, sizeof(fw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_SOFTWARE, iotdata_version_software(sw, sizeof(sw)));
    iotdata_kvr_add_str(kv, IOTDATA_NODE_VERSION_SERIAL, iotdata_version_serial(sn, sizeof(sn)));
    if (caps != NULL && caps->count > 0) {
        uint8_t packed[IOTDATA_VERSION_CAP_COUNT * 2];
        const int n = iotdata_version_caps_pack(caps, packed, sizeof(packed));
        if (n > 0)
            iotdata_kvr_add(kv, IOTDATA_NODE_VERSION_CAPABILITIES, packed, (uint8_t)n);
    }
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_VERSION_H */
