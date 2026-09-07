/*
 * iotdata_blackbox.h — iotdata adapter for the blackbox recorder (iotdata-depend/blackbox).
 *
 * Provides the iotdata *backend + clock commonality* and the one shared record type (a lifecycle
 * event). Records themselves are otherwise per-project. Include THIS instead of blackbox.h.
 *
 * Single-header / unity: in the app's ONE unity TU, define IOTDATA_BLACKBOX_IMPLEMENTATION before
 * including — that pulls in the blackbox implementation and this adapter's definitions. Other TUs
 * just #include it for the declarations.
 *
 *   platform backend:  esp32 → NONE (RTC pool ring; ESP_FLASH is P3)   linux → FILE
 *   clock:             esp32 → "seq:up_ms"                             linux → epoch ms
 *
 * A project sets its own backend by #defining BLACKBOX_PERSIST before including (the defaults below
 * are #ifndef-guarded).
 */
#ifndef IOTDATA_BLACKBOX_H
#define IOTDATA_BLACKBOX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* -------- platform backend + sizes + clock (compile-time; override before including) ---------- */
#if defined(ESP_PLATFORM)
#ifndef BLACKBOX_PERSIST
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE /* P1: RTC pool ring; ESP_FLASH → P3 */
#endif
#ifndef IOTDATA_BLACKBOX_POOL_SZ
#define IOTDATA_BLACKBOX_POOL_SZ 2048u
#endif
#else /* host / linux */
#ifndef BLACKBOX_PERSIST
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_FILE
#endif
#ifndef IOTDATA_BLACKBOX_POOL_SZ
#define IOTDATA_BLACKBOX_POOL_SZ 4096u
#endif
#endif

#ifndef BLACKBOX_CLOCK
#define BLACKBOX_CLOCK iotdata_blackbox_clock
#endif

/* stb-style: BLACKBOX_IMPLEMENTATION must be set BEFORE the first blackbox.h include. */
#ifdef IOTDATA_BLACKBOX_IMPLEMENTATION
#ifndef BLACKBOX_IMPLEMENTATION
#define BLACKBOX_IMPLEMENTATION
#endif
#endif
#include "blackbox.h"

/* -------- the one shared record: a lifecycle event -------------------------------------------- */
typedef enum {
    IOTDATA_BB_LC_BOOT = 0, /* cold boot / power-on                     */
    IOTDATA_BB_LC_START,    /* app started (post-init)                  */
    IOTDATA_BB_LC_STOP,     /* app stopping / shutdown                  */
    IOTDATA_BB_LC_SLEEP,    /* entering (deep) sleep                    */
    IOTDATA_BB_LC_WAKE,     /* woke from sleep                          */
    IOTDATA_BB_LC_RESET,    /* reset (reason in `reason`)               */
    IOTDATA_BB_LC_ERROR,    /* error / fault (code in `reason`)         */
} iotdata_bb_lc_event_t;

// @blackbox tag=LC
typedef struct {
    uint8_t event;
    uint8_t reason;
} iotdata_bb_lifecycle_t;

extern const blackbox_struct_config_t iotdata_blackbox_config_lifecycle;

/* iotdata_blackbox_clock (the BLACKBOX_CLOCK hook) is already forward-declared by blackbox.h. */

/* Seed the clock. Call ONCE before blackbox_init(). On esp32 the clock's `seq` lives in RTC_NOINIT,
 * so this zeroes it on a true cold start (detected with a magic word -- see the implementation for
 * why esp_reset_reason() cannot be used for that). A no-op on the host. */
void iotdata_blackbox_begin(void);

/* Convenience: record a lifecycle event. */
static inline int iotdata_blackbox_lifecycle(blackbox_handle_t *h, iotdata_bb_lc_event_t ev, uint8_t reason) {
    const iotdata_bb_lifecycle_t r = { (uint8_t)ev, reason };
    return blackbox_insert(h, &iotdata_blackbox_config_lifecycle, &r);
}

/* ============================ implementation ============================ */
#ifdef IOTDATA_BLACKBOX_IMPLEMENTATION

static int iotdata_bb__enc_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_bb_lifecycle_t *r = (const iotdata_bb_lifecycle_t *)data;
    return snprintf(out, n, "%u,%u", (unsigned)r->event, (unsigned)r->reason);
}
static int iotdata_bb__dec_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_bb_lifecycle_t *r = (iotdata_bb_lifecycle_t *)data;
    unsigned e = 0, rs = 0;
    if (sscanf(in, "%u,%u", &e, &rs) != 2)
        return -1;
    r->event = (uint8_t)e;
    r->reason = (uint8_t)rs;
    return 0;
}
const blackbox_struct_config_t iotdata_blackbox_config_lifecycle = { "LC", iotdata_bb__enc_lifecycle, iotdata_bb__dec_lifecycle };

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_timer.h"

/* esp32 clock = "seq:up_ms". `seq` is a monotonic counter in RTC_NOINIT so it survives deep sleep and
 * resets -- which also means it holds GARBAGE on a true cold start.
 *
 * You cannot detect that from esp_reset_reason(): a USB-powered ESP32-C3 reports ESP_RST_USB on a
 * genuine power-up, NOT ESP_RST_POWERON (the ROM logs it as rst:0x15 USB_UART_CHIP_RESET), so an
 * `if (reason == ESP_RST_POWERON) seq = 0;` guard silently never fires and the garbage persists
 * through every later reset. Pair it with a magic word instead, exactly as an RTC state struct is.
 *
 * Both variables are owned HERE rather than by each app, so this cannot be got wrong per project.
 * Call iotdata_blackbox_begin() once before blackbox_init(). */
#define IOTDATA_BLACKBOX_SEQ_MAGIC 0x1D5EC10Cu /* 'IDSEC-CLOC(k)' */

RTC_NOINIT_ATTR uint32_t iotdata_blackbox_seq;
RTC_NOINIT_ATTR static uint32_t iotdata_blackbox_seq_magic;

void iotdata_blackbox_begin(void) {
    if (iotdata_blackbox_seq_magic != IOTDATA_BLACKBOX_SEQ_MAGIC) { /* RTC was garbage: cold start */
        iotdata_blackbox_seq = 0;
        iotdata_blackbox_seq_magic = IOTDATA_BLACKBOX_SEQ_MAGIC;
    }
}

int iotdata_blackbox_clock(char *out, size_t n) {
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    return snprintf(out, n, "%u:%u", (unsigned)(++iotdata_blackbox_seq), (unsigned)up);
}
#else
void iotdata_blackbox_begin(void) { /* host: the clock is a real timestamp, nothing to seed */
}
#include <time.h>
int iotdata_blackbox_clock(char *out, size_t n) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC); /* C11 standard — no POSIX feature-test macro needed */
    const long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return snprintf(out, n, "%lld", ms);
}
#endif

#endif /* IOTDATA_BLACKBOX_IMPLEMENTATION */

#endif /* IOTDATA_BLACKBOX_H */
