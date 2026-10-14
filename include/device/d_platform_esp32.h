
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_platform_esp32.h
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_ESP32
#define PLATFORM_ESP32
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wredundant-decls"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "rom/ets_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* <time.h> FOR hw_clock() BELOW, and it needs the suppression: IDF carries its own
   esp_libc/platform_include/time.h which redeclares clock_settime/clock_gettime/clock_getres on top
   of newlib's, and the project builds with -Werror=redundant-decls. The demotion is scoped to this
   one include rather than being turned off for the component -- nothing of ours is excused by it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <time.h>
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

/* THE WALL CLOCK, and the one place a port has to think about one. hw_time_ms() is what anything
   that must HAPPEN runs on -- it exists from reset and never steps -- so this is for the other two
   jobs only: an instant a reader reads as a date, and a stamp another host compares against its
   own clock. time_t is the platform's own, 2038 and all: when that has to change, it changes here.
   See hw_clock_is_set() in d_common.h -- the value may be nothing but seconds since boot. */
static inline time_t hw_clock(void) {
    return time(NULL);
}

#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

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

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
