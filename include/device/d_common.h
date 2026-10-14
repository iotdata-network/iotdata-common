
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef MIN_INT
#define MIN_INT(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX_INT
#define MAX_INT(a, b) ((a) > (b) ? (a) : (b))
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define DEV_ERR_BASE        0x10000
#define DEV_ERR_NOT_READY   (DEV_ERR_BASE + 1)
#define DEV_ERR_BAD_READING (DEV_ERR_BASE + 2)
#define DEV_ERR_BAD_QUALITY (DEV_ERR_BASE + 3)
#define DEV_ERR_PRODUCT_ID  (DEV_ERR_BASE + 4)
#define DEV_ERR_CHECKSUM    (DEV_ERR_BASE + 5)
#define DEV_ERR_NO_FIX      (DEV_ERR_BASE + 6)
#define DEV_ERR_PARSE       (DEV_ERR_BASE + 7)
#define DEV_ERR_TIMEOUT     (DEV_ERR_BASE + 8)
#define DEV_ERR_RTC         (DEV_ERR_BASE + 9)

typedef struct {
    bool passed;
    char detail[64];
} device_test_result_t;

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define D_WAIT_READY_FUNC(func_name, ready_func, ready_delay_ms, wait_after_ready, tag) \
    bool func_name(const uint32_t timeout_ms) { \
        uint32_t elapsed = 0; \
        while (!ready_func()) { \
            if (elapsed >= timeout_ms) { \
                ESP_LOGW(tag, "%s: timeout after %" PRIu32 "ms", __func__, timeout_ms); \
                return false; \
            } \
            hw_delay_ms_yieldable(ready_delay_ms); \
            elapsed += ready_delay_ms; \
        } \
        if (wait_after_ready) \
            hw_delay_ms_yieldable(ready_delay_ms); \
        return true; \
    }

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

const char *d_bytes_hex_str(char *const buf, const int buf_size, const uint8_t *const data, const int len, const char *const sep) {
    for (int i = 0, p = 0; i < len && p < buf_size - 1; i++)
        p += snprintf(buf + p, (size_t)(buf_size - p), "%s%02" PRIX8, (i > 0 && sep) ? sep : "", data[i]);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* A LINE AT A TIME, THROUGH A CALLBACK, and not ESP_LOGD. This used to log directly, which made
   d_common.h -- a header of pure byte and string helpers -- unusable anywhere a log macro is not
   already in scope: a host suite that wanted a checksum from this file had to drag a platform
   header in to satisfy a hex dumper it was never going to call. Worse, on a build where the debug
   level compiles the macro away, `tag` became an unused parameter and the whole header stopped
   compiling under -Werror.
 *
 * The caller owns its tag and its log level, which is where that knowledge belongs; this just
   formats the rows. It is the same emit seam the recorder and the content store use. */
typedef void (*d_bytes_emit_fn)(const char *line);

void d_bytes_hex_log(const d_bytes_emit_fn emit, const uint8_t *const data, const int size) {
    if (emit == NULL || data == NULL)
        return;
    const int bpl = 16;
    for (int offset = 0; offset < size; offset += bpl) {
        char line[128];
        int pos = snprintf(line, sizeof(line), "%04X: ", offset);
        for (int i = 0; i < bpl; i++)
            if (offset + i < size)
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%s%02" PRIX8 " ", i == bpl / 2 ? " " : "", data[offset + i]);
            else
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%s   ", i == bpl / 2 ? " " : "");
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " ");
        for (int i = 0; i < bpl && offset + i < size; i++)
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%s%c", i == bpl / 2 ? " " : "", isprint(data[offset + i]) ? (char)data[offset + i] : '.');
        emit(line);
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define _RTC_DATA_STRUCT     RTC_NOINIT_ATTR
#define _RTC_DATA_STRUCT_ISR RTC_NOINIT_ATTR
#define _RTC_DATA_STAMP_ENTRY \
    uint32_t magic; \
    uint32_t size
#define _RTC_DATA_VALID(s, m) ((s)->magic == (m) && (s)->size == (uint32_t)sizeof(*(s)))
#define _RTC_DATA_INIT(s, m) \
    do { \
        memset(s, 0, sizeof(*(s))); \
        (s)->magic = m; \
        (s)->size = (uint32_t)sizeof(*(s)); \
    } while (0)
#define _RTC_DATA_INIT_ISR(s, m) _RTC_DATA_INIT(s, m)

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define ESP_ERROR_CHECK_BOOLEAN(cond) \
    do { \
        if (!(cond)) { \
            ESP_LOGE("CHECK", "%s(%d): %s", __FILE__, __LINE__, #cond); \
            abort(); \
        } \
    } while (0)

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define US_PER_MS  1000
#define MS_PER_SEC 1000

static inline int64_t hw_ticks_ms(void) {
    return esp_timer_get_time() / US_PER_MS;
}

static inline uint32_t hw_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / US_PER_MS);
}

/* hw_clock() IS THE PLATFORM'S, defined in d_platform_{linux,esp32}.h, because <time.h> cannot be
   included from here: IDF carries its own platform_include/time.h and pulling in newlib's early
   makes its clock_gettime/settime/getres declarations redundant, which -Werror=redundant-decls
   then refuses. Each platform header has the libc declarations in scope already.
   A node with no SNTP counts from the epoch at boot, so hw_clock() is SMALL rather than wrong --
   this is how a caller asks whether it means anything before publishing it as a date. */
#define HW_CLOCK_SET_FROM 1000000000 /* 2001-09-09: anything earlier is a clock nobody ever set */

static inline bool hw_clock_is_set(void) {
    return hw_clock() >= (time_t)HW_CLOCK_SET_FROM;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define HW_WDT_FEED_MS 1000U

static inline void hw_delay_ms_yieldable(const uint32_t ms) {
    uint32_t remain = ms;
    do {
        const uint32_t chunk = remain < HW_WDT_FEED_MS ? remain : HW_WDT_FEED_MS;
        (void)esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(chunk));
        remain -= chunk;
    } while (remain > 0);
}

static inline void hw_delay_ms_precise(const uint32_t ms) {
    esp_rom_delay_us(ms * US_PER_MS);
}

static inline void hw_delay_us_precise(const uint32_t us) {
    esp_rom_delay_us(us);
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline int32_t hw_desync_offset(const uint32_t unit_stable, const uint32_t per_cycle, const uint32_t spread) {
    uint32_t h = unit_stable ^ (per_cycle * 2654435761u);
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return spread == 0 ? 0 : (int32_t)(h % spread) - (int32_t)(spread / 2u);
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline uint32_t __JITTER(void) {
    uint32_t acc = 0;
    for (int i = 0; i < 256; i++) {
        const uint32_t a = esp_cpu_get_cycle_count();
        ets_delay_us(1);
        const uint32_t b = esp_cpu_get_cycle_count();
        acc ^= (b - a);
        acc = (acc << 1) | (acc >> 31); // rotate
    }
    return acc;
}

#define __RANDOM() (__JITTER() ^ (uint32_t)esp_timer_get_time() ^ (uint32_t)esp_random())

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef _IOTDATA_CRC64_TABLE
#define _IOTDATA_CRC64_TABLE 1
#endif

/* The reflected form of 0x42F0E1EBA9EA3693 (CRC-64/ECMA-182's polynomial, which XZ shares). The
   suite asserts the reversal rather than taking it on trust, and anchors the normal form on
   ECMA-182's own published check value. */
#define _IOTDATA_CRC64_POLY 0xC96C5795D7870F42ULL

/* The running value to start from, and -- because of the convention above -- also the answer for no
   bytes at all, which liblzma agrees with. */
#define _IOTDATA_CRC64_INIT 0x0000000000000000ULL

// -----------------------------------------------------------------------------------------------------------------------------------------

#if _IOTDATA_CRC64_TABLE

/* crc64(nibble), four shifts each. Generated from _IOTDATA_CRC64_POLY, and the suite
   REGENERATES it bitwise and compares -- so a typo in a literal below cannot survive a test run. */
static const uint64_t _iotdata_crc64_table[16] = {
    0x0000000000000000ULL, 0x7D9BA13851336649ULL, 0xFB374270A266CC92ULL, 0x86ACE348F355AADBULL, 0x64B62BCAEBC387A1ULL, 0x192D8AF2BAF0E1E8ULL, 0x9F8169BA49A54B33ULL, 0xE21AC88218962D7AULL,
    0xC96C5795D7870F42ULL, 0xB4F7F6AD86B4690BULL, 0x325B15E575E1C3D0ULL, 0x4FC0B4DD24D2A599ULL, 0xADDA7C5F3C4488E3ULL, 0xD041DD676D77EEAAULL, 0x56ED3E2F9E224471ULL, 0x2B769F17CF112238ULL,
};

#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint64_t _iotdata_crc64(const void *const data, const size_t len, const uint64_t crc_in) {
    if (data == NULL || len == 0u)
        return crc_in;
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = ~crc_in;
    for (size_t i = 0; i < len; i++) {
#if _IOTDATA_CRC64_TABLE
        crc = (crc >> 4) ^ _iotdata_crc64_table[(crc ^ (uint64_t)p[i]) & 0xFu];
        crc = (crc >> 4) ^ _iotdata_crc64_table[(crc ^ ((uint64_t)p[i] >> 4)) & 0xFu];
#else
        crc ^= (uint64_t)p[i];
        for (int bit = 0; bit < 8; bit++)
            /* a conditional rather than the sign-extended-mask trick: the same instructions after
               any optimiser, and no signed arithmetic for -Wconversion to object to */
            crc = (crc & 1u) ? ((crc >> 1) ^ _IOTDATA_CRC64_POLY) : (crc >> 1);
#endif
    }
    return ~crc;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
