#ifndef D_MODULE_VERSION_ESP32_H
#define D_MODULE_VERSION_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_version_esp32.h -- what this platform can say about ITSELF, for the VERSION report.
//
// The four DETECTED answers, and the build stamp where the platform keeps one. Everything above
// this -- the grammars, the key space, what is declared rather than detected -- belongs to
// iotdata_node_version.h, which asks these questions and never knows which platform answered:
//
//   version_hardware(buf, size)  board/arch
//   version_firmware(buf, size)  stack/version[+low]: what runs beneath the application
//   version_serial(buf, size)    the board's own identity, never a removable one
//   version_image(out, size)     the digest of the bytes actually running, false where there is none
//   version_stamp(buf, size)     twelve digits, false where the platform cannot say
//
// An application includes this beside its other d_ modules. A build that includes none still
// compiles: iotdata_node_version.h answers "unknown" -- which is also what a FORGOTTEN include
// looks like, so check this is in the list before believing it.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d_format.h" /* snprintf_inline, version_append, version_sep */

/* THE MARKER iotdata_node_version.h TESTS. Not a platform macro: that header is not allowed to know
   what a platform is, so what it asks is "did somebody answer", which is this. */
#define D_MODULE_VERSION_PROVIDED 1

// ------------------------------------------------------------------------------------------------------------------------

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
 *   app_update              esp_ota_ops.h               -> image (which partition is running)
 *   esp_partition           esp_partition.h             -> image (the digest of it)
 *
 * The bootloader one is the quiet case: it is probed with __has_include, so a project that omits
 * the component still compiles and simply reports no bootloader version. That is how `idf/6.1`
 * appeared where `idf/6.1+bl1` was expected.
 */

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
/* The bootloader descriptor is what supplies the [+low] half of `firmware`, and it arrived in a
   later IDF -- so it is probed rather than assumed. Without this include the guard below would be
   false for the wrong reason and the bootloader would be silently absent from every report. */
#if defined(__has_include)
#if __has_include("esp_bootloader_desc.h")
#include "esp_bootloader_desc.h"
#endif
#endif

// ------------------------------------------------------------------------------------------------------------------------

static inline const char *version_hardware(char *const buf, const size_t size) {
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
    return snprintf_inline(buf, size, "%s/%s", chip, arch);
}

// ------------------------------------------------------------------------------------------------------------------------

/* IDF version plus the bootloader, which is the ESP32's genuine "firmware beneath the app" -- the
   thing that is flashed separately and outlives an OTA. */
static inline const char *version_firmware(char *const buf, const size_t size) {
    const esp_app_desc_t *const app = esp_app_get_description();
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    version_append(buf, size, &n, "idf");
    version_sep(buf, size, &n, '/');
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
    version_append(buf, size, &n, r > 0 ? ver : "unknown");
#if defined(ESP_BOOTLOADER_DESC_MAGIC_WORD)
    /* the bootloader is the [+low] part: flashed separately, and it outlives an OTA */
    const esp_bootloader_desc_t *const bl = esp_bootloader_get_description();
    if (bl != NULL) {
        char blv[12];
        version_sep(buf, size, &n, '+');
        version_append(buf, size, &n, snprintf_inline(blv, sizeof(blv), "bl%u", (unsigned)bl->version));
    }
#endif
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const char *version_serial(char *const buf, const size_t size) {
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return snprintf_inline(buf, size, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool version_image(uint8_t *const out, const size_t size) {
    static uint8_t hash[32]; /* a sha256, read back once: the partition is not going to change under us */
    static bool done = false, have = false;
    if (!done) {
        const esp_partition_t *const running = esp_ota_get_running_partition();
        if (running != NULL && esp_partition_get_sha256(running, hash) == ESP_OK)
            have = true;
        done = true;
    }
    /* THE CALLER SAYS HOW MUCH OF THE DIGEST IT WANTS, because the field's width is the protocol's
       business and not this header's -- it takes the leading bytes, which is what a truncated
       digest means everywhere else. */
    if (out == NULL || size == 0 || size > sizeof(hash) || !have)
        return false;
    memcpy(out, hash, size);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------

static inline int _version_month(const char *const mon) {
    static const char names[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    char low[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 3 && mon[i] != '\0'; i++)
        low[i] = (mon[i] >= 'A' && mon[i] <= 'Z') ? (char)(mon[i] + ('a' - 'A')) : mon[i];
    for (int m = 0; m < 12; m++)
        if (strncmp(names + m * 3, low, 3) == 0)
            return m + 1;
    return 0;
}
// ------------------------------------------------------------------------------------------------------------------------

/* ESP-IDF stamps its app descriptor at link, so this is always THIS build rather than whenever the
   tree was last touched -- which is the whole reason a platform is asked at all. */
static inline bool version_stamp(char *const buf, const size_t size) {
    const esp_app_desc_t *const app = esp_app_get_description();
    int mday = 0, year = 0, hour = 0, minute = 0;
    /* app->date is __DATE__, "Sep 12 2026", with the day space-padded; app->time is "17:45:09" */
    const int m = (app != NULL) ? _version_month(app->date) : 0;
    if (m <= 0 || sscanf(app->date + 3, "%d %d", &mday, &year) != 2 || sscanf(app->time, "%d:%d", &hour, &minute) != 2)
        return false;
    (void)snprintf_inline(buf, size, "%04d%02d%02d%02d%02d", year, m, mday, hour, minute);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_VERSION_ESP32_H */
