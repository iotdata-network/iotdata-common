
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PLATFORM_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "rom/ets_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#pragma GCC diagnostic pop
#endif

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

void d_bytes_hex_log(const char *tag, const uint8_t *const data, const int size) {
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
        ESP_LOGD(tag, "%s", line);
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#define _RTC_DATA_STRUCT      RTC_NOINIT_ATTR
#define _RTC_DATA_STAMP_ENTRY uint32_t magic
#define _RTC_DATA_VALID(s, m) ((s)->magic == (m))
#define _RTC_DATA_INIT(s, m) \
    do { \
        memset(s, 0, sizeof(*(s))); \
        (s)->magic = m; \
    } while (0)

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

typedef int64_t __ticks_t;

static inline __ticks_t __ticks_ms(void) {
    return esp_timer_get_time() / US_PER_MS;
}

#define __MILLIS()     ((uint32_t)(esp_timer_get_time() / 1000))

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

/* Busy-waits. Only for sub-tick timing a driver genuinely needs; never for a wait of any length. */
static inline void hw_delay_ms_precise(const uint32_t ms) {
    esp_rom_delay_us(ms * US_PER_MS);
}

#define __SLEEP_MS(ms) hw_delay_ms_yieldable((uint32_t)(ms))

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

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_LINUX
static inline const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:
        return "POWERON (cold boot / power cycle)";
    case ESP_RST_EXT:
        return "EXT (external reset pin)";
    case ESP_RST_SW:
        return "SW (esp_restart / software)";
    case ESP_RST_PANIC:
        return "PANIC (exception/abort crash)";
    case ESP_RST_INT_WDT:
        return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:
        return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP (wake from deep sleep)";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT (power dip — check USB/cable/supply)";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB (reset over USB peripheral)";
    case ESP_RST_JTAG:
        return "JTAG";
    default:
        return "UNKNOWN";
    }
}
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
