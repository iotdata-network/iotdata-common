
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
