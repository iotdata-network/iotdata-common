#ifndef D_MODULE_VERSION_LINUX_H
#define D_MODULE_VERSION_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_version_linux.h -- what this platform can say about ITSELF, for the VERSION report.
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
#include <sys/utsname.h>

#include "d_format.h" /* snprintf_inline, version_append, version_sep */

/* THE MARKER iotdata_node_version.h TESTS. Not a platform macro: that header is not allowed to know
   what a platform is, so what it asks is "did somebody answer", which is this. */
#define D_MODULE_VERSION_PROVIDED 1

// ------------------------------------------------------------------------------------------------------------------------

#include <sys/utsname.h>

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _version_slurp(const char *const path, char *const dst, const size_t size) {
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
    version_append(dst, size, &n, raw);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Board from the device tree where there is one (Pi, Rock) and from DMI on a PC, falling back to
   the architecture alone -- which is still true, just less specific. */
static inline const char *version_hardware(char *const buf, const size_t size) {
    struct utsname u;
    const char *arch = (uname(&u) == 0) ? u.machine : "unknown";
    char board[24] = { 0 };
    if (!_version_slurp("/proc/device-tree/model", board, sizeof(board)) && !_version_slurp("/sys/class/dmi/id/product_name", board, sizeof(board)))
        board[0] = '\0';
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    if (board[0] != '\0') {
        version_append(buf, size, &n, board);
        version_sep(buf, size, &n, '/');
    }
    version_append(buf, size, &n, arch);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The kernel is what runs beneath the application here. The Pi's VideoCore blob is the closer
   analogue of a bootloader, but vcgencmd is a binary a slimmed image may not carry, so it is not
   worth a fork per boot -- kernel alone, and the board string already says it is a Pi. */
static inline const char *version_firmware(char *const buf, const size_t size) {
    struct utsname u;
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    version_append(buf, size, &n, "linux");
    version_sep(buf, size, &n, '/');
    if (uname(&u) != 0) {
        version_append(buf, size, &n, "unknown");
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
    version_append(buf, size, &n, r > 0 ? rel : "unknown");
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

/* NEVER a NIC MAC: those are removable, and a box whose identity changes when a card is swapped
   cannot be tracked across the swap. The CPU serial is the board; machine-id is the install. */
static inline const char *version_serial(char *const buf, const size_t size) {
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
                    version_append(raw, sizeof(raw), &rn, v);
                }
                break;
            }
        (void)fclose(f);
    }
    if (raw[0] == '\0' && !_version_slurp("/proc/device-tree/serial-number", raw, sizeof(raw)))
        (void)_version_slurp("/etc/machine-id", raw, sizeof(raw));
    /* trim leading zeros a Pi pads its serial with, but never to nothing */
    const char *p = raw;
    while (*p == '0' && *(p + 1) != '\0')
        p++;
    size_t n = 0;
    if (size > 0)
        buf[0] = '\0';
    version_append(buf, size, &n, p[0] != '\0' ? p : "unknown");
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool version_image(__attribute__((unused)) uint8_t *const out, __attribute__((unused)) const size_t size) {
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------

/* A HOST BUILD HAS NO STAMPED DESCRIPTOR. The compiler's __DATE__ would stamp the translation unit
   rather than the build, and a `date` at run time says when it was asked, not when it was made --
   so this says nothing and lets the build-supplied IOTDATA_NODE_VERSION_STAMP be the only answer. */
static inline bool version_stamp(__attribute__((unused)) char *const buf, __attribute__((unused)) const size_t size) {
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_VERSION_LINUX_H */
