
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * IoT Sensor Telemetry Protocol
 * Copyright(C) 2026 Matthew Gream (https://libiotdata.org)
 *
 * app_template_simple_lora_esp32.c
 *
 */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#pragma GCC diagnostic pop

// -----------------------------------------------------------------------------------------------------------------------------------------
// TUNABLE
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef BATTERY_SELFTEST_MS
#define BATTERY_SELFTEST_MS 0
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// HARDWARE
// -----------------------------------------------------------------------------------------------------------------------------------------

#include "d_platform_esp32.h"
#include "d_common.h"
#include "d_format.h"
#include "d_module_datastore_esp32.h"
#include "d_readings.h"
#include "d_hardware_gpio.h"
#if !defined(PIN_DEVICE_UART_TX) || !defined(PIN_DEVICE_UART_RX) || !defined(PIN_DEVICE_LORA_AUX) || !defined(PIN_DEVICE_LORA_M0) || !defined(PIN_DEVICE_LORA_M1)
#error "must define all of PIN_DEVICE_UART_TX/PIN_DEVICE_UART_RX/PIN_DEVICE_LORA_AUX/PIN_DEVICE_LORA_M0/PIN_DEVICE_LORA_M1"
#endif
#include "d_hardware_uart.h"
#include "d_interface_e22900t22.h"
#ifdef NO_BATTERY_PROBE
#define BATTERY_PROBE 0
#ifndef PIN_BATTERY_EN
#define PIN_BATTERY_EN GPIO_NUM_NC
#endif
#ifndef PIN_BATTERY_ADC
#define PIN_BATTERY_ADC GPIO_NUM_NC
#endif
#else
#define BATTERY_PROBE 1
#if !defined(PIN_BATTERY_EN) || !defined(PIN_BATTERY_ADC)
#error "must define all of PIN_BATTERY_EN/PIN_BATTERY_ADC"
#endif
#endif
#include "d_hardware_adc.h"
#include "d_interface_batt.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// PROTOCOL
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * Strip everything except the encoder for a minimal ESP32 build:
 *   - NO_JSON:     no cJSON dependency
 *   - NO_DUMP:     no dump output
 *   - NO_PRINT:    no print output
 *   - NO_FLOATING: iotdata_float_t = int32_t (value * 100)
 */
#define IOTDATA_NO_JSON
#define IOTDATA_NO_DUMP
#define IOTDATA_NO_PRINT
#define IOTDATA_NO_FLOATING
#include "iotdata_variant.h"
#include "iotdata.h"
#include "iotdata.c"
#include "iotdata_node.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// PLATFORM
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_DIAGNOSTICS_FLUSH BLACKBOX_FLUSH_MANUAL /* a sleeping node flushes before it sleeps */
#include "iotdata_node_platform.h"
static void _iapp_diagnostics_emit(const char *const line) {
    ESP_LOGI("app", "%s", line); // XXX
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool _iapp_on_lora_changed(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *was, const iotdata_node_config_info_t *info);
#define IOTDATA_NODE_CONFIG_LORA_NOTIFY _iapp_on_lora_changed
#include "iotdata_node_config_device_e22900t22.h"
static bool _iapp_on_batt_changed(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *was, const iotdata_node_config_info_t *info);
#define IOTDATA_NODE_CONFIG_BATT_NOTIFY _iapp_on_batt_changed
#include "iotdata_node_config_device_batt.h"

#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    APP_CONFIG_ENTRIES(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_LORA(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_BATT(X)

#include "iotdata_node_config.h"
#include "iotdata_node_config_device_e22900t22.h"
static bool _iapp_on_lora_changed(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *was, __attribute__((unused)) const iotdata_node_config_info_t *info) {
    ESP_LOGW("config", "lora %s changed (was %llu), restart required", row->name, (unsigned long long)was->u);
    return true;
}
#include "iotdata_node_config_device_batt.h"
static bool _iapp_on_batt_changed(const iotdata_node_config_row_t *row, __attribute__((unused)) const iotdata_node_config_value_t *was, __attribute__((unused)) const iotdata_node_config_info_t *info) {
    if (!iotdata_node_config_batt_apply())
        ESP_LOGW("config", "batt %s changed, but values not accepted", row->name);
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef POWER_DIAGNOSTICS
#define POWER_DIAGNOSTICS 1
#endif
#include "iotdata_node_diagnostics_device_batt.h"

// -----------------------------------------------------------------------------------------------------------------------------------------

static void _iapp_node_status(const uint16_t station, iotdata_node_status_t *const out);
static bool _iapp_node_transmit(const uint8_t *const packet, const size_t len);
static int _iapp_node_config_build(uint8_t *buf, size_t size, iotdata_node_partial_t *partial);
static bool _iapp_node_config_apply(const uint8_t *buf, size_t len, bool *reboot_out);

// -----------------------------------------------------------------------------------------------------------------------------------------
// APP STATE
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IAPP_STATE_MAGIC 0xB1E28003UL
#define IAPP_STATE_TAG   0xB1E28004UL

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    iotdata_node_version_caps_t caps;
    iotdata_node_t node;
    uint32_t restarts;
    uint32_t tx_cycles;   /* wake cycles since the last restart              */
    uint32_t tx_count;    /* packets transmitted                             */
    uint32_t tx_errors;   /* packets the radio would not take                */
    int16_t battery_mv;   /* last reading, for the charging trend bit        */
    bool battery_present; /* a divider answered the probe at restart         */
    bool lora_present;    /* E22 NVM configuration verified this power cycle */
    app_operating_t app;
} iapp_operating_t;

typedef struct {
    datastore_t datastore;
    iotdata_node_state_t node;
    iotdata_node_settings_t node_settings; /* the PROTOCOL's own: station id, reporting, receive */
    const iotdata_node_params_t node_config;
    uint8_t packet_buffer[IOTDATA_MAX_PACKET_SIZE];
    app_running_t app;
} iapp_running_t;

static _RTC_DATA_STRUCT iapp_operating_t _iapp_operating;
static iapp_running_t _iapp_running = { .node_config = {
                                            .caps = &_iapp_operating.caps,
                                            .status = _iapp_node_status,
                                            .tx = _iapp_node_transmit,
                                            .control = IOTDATA_NODE_DIAGNOSTICS_CONTROL,
                                            .control_actions = iotdata_node_diagnostics_control_actions,
                                            .control_actions_count = IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT,
                                            .diag = IOTDATA_NODE_DIAGNOSTICS_PULL,
                                            .receive_every_ms = IOTDATA_NODE_RECEIVE_EVERY_MS,
                                            .receive_window_ms = IOTDATA_NODE_RECEIVE_WINDOW_MS,
                                            .settings = &_iapp_running.node_settings,
                                            .state = &_iapp_running.node,
                                            .config_build = _iapp_node_config_build,
                                            .config_apply = _iapp_node_config_apply,
                                            .packet_max = LORA_PACKET_SIZE_MAX,
                                        } };

typedef struct {
    iapp_operating_t *const operating;
    iapp_running_t *const running;
} iapp_state_t;
typedef struct {
    app_operating_t *const operating;
    app_running_t *const running;
} app_state_t;

static iapp_state_t iapp_state = { .operating = &_iapp_operating, .running = &_iapp_running };
static app_state_t app_state = { .operating = &_iapp_operating.app, .running = &_iapp_running.app };

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    IAPP_EV_OK,
    IAPP_EV_SLEEP_DEEP,
    IAPP_EV_SLEEP_LIGHT,
    IAPP_EV_FAIL
} iapp_event_t;

typedef void (*app_func_init_log_t)(app_state_t *const as, iotdata_node_t *const node, char *const buf, const size_t len);
typedef iapp_event_t (*app_func_init_entry_t)(app_state_t *const as, iotdata_node_t *const node, const esp_reset_reason_t reset_reason);
typedef iapp_event_t (*app_func_init_rtc_t)(app_state_t *const as, iotdata_node_t *const node, iotdata_node_version_caps_t *const caps);
typedef iapp_event_t (*app_func_init_datastore_t)(app_state_t *const as, iotdata_node_t *const node, datastore_t *const ds);
typedef iapp_event_t (*app_func_init_final_t)(app_state_t *const as, iotdata_node_t *const node, const esp_reset_reason_t reset_reason);
typedef iapp_event_t (*app_func_sleep_t)(app_state_t *const as, iotdata_node_t *const node, uint32_t *const sleep_ms, const uint32_t start_ms, const uint32_t awake_ms, bool deepsleep);
typedef iapp_event_t (*app_func_exec_packet_t)(app_state_t *const as, iotdata_node_t *const node, const uint32_t cycle, const bool battery_present, const battery_reading_t *const battery, const int16_t battery_mv_was, uint8_t *const buf,
                                               const size_t len, const bool advertise, size_t *const transmit);
typedef iapp_event_t (*app_func_exec_final_t)(app_state_t *const as, iotdata_node_t *const node, const uint32_t start_ms);
typedef void (*app_func_fail_t)(app_state_t *const as, iotdata_node_t *const node);

typedef struct {
    const char *name;
    uint8_t variant;
    app_func_init_log_t func_init_log;
    app_func_init_entry_t func_init_entry;
    app_func_init_rtc_t func_init_rtc;
    app_func_init_datastore_t func_init_datastore;
    app_func_init_final_t func_init_final;
    app_func_sleep_t func_sleep;
    app_func_exec_packet_t func_exec_packet;
    app_func_exec_final_t func_exec_final;
    app_func_fail_t func_fail;
} app_descr_t;

extern const app_descr_t *app_spec;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef CONSOLE_DRAIN_MS
#define CONSOLE_DRAIN_MS 100 /* let the console flush before deep sleep    */
#endif
#ifndef RECEIVE_POLL_MS
#define RECEIVE_POLL_MS 10 /* polling during the recieve window */
#endif
#ifndef RESTART_MS
#define RESTART_MS 30000 /* duration of restart on failure */
#endif

static const char *__tag_iapp = "iapp";

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static void _iapp_node_status(__attribute__((unused)) const uint16_t station, iotdata_node_status_t *const out) {
    const iapp_operating_t *const s = iapp_state.operating;
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->reason = iotdata_node_reason_reset();
    out->has_heap = true;
    out->heap_free = (uint32_t)esp_get_free_heap_size();
    out->heap_min = (uint32_t)esp_get_minimum_free_heap_size();
    out->has_restarts = true;
    out->restarts = (uint16_t)s->restarts;
    if (s->battery_present && s->battery_mv > 0) {
        out->has_supply = true;
        out->supply_mv = (uint16_t)s->battery_mv;
    }
}

static bool _iapp_node_transmit(const uint8_t *const packet, const size_t len) {
    return lora_write_complete(packet, len, /*wait_complete=*/true) == ESP_OK;
}

static int _iapp_node_config_build(uint8_t *const buf, const size_t size, iotdata_node_partial_t *const partial) {
    return iotdata_node_config_pack(buf, size, partial);
}
static bool _iapp_node_config_apply(const uint8_t *const buf, const size_t len, bool *const reboot_out) {
    return iotdata_node_config_apply(buf, len, &iapp_state.running->datastore, reboot_out);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static void iapp_cycle_elapsed(iapp_state_t *const as, const uint32_t cycle_ms) {
    (void)iotdata_node_window_advance(&as->operating->node, cycle_ms);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static void iapp_node_receive(iapp_state_t *const as) {
    iotdata_node_t *const n = &as->operating->node;

    ESP_LOGI(__tag_iapp, "receive: window opened for %us (station=%03" PRIX16 ")", (unsigned)(IOTDATA_NODE_RECEIVE_WINDOW_MS / 1000), iotdata_node_station(n));
    iotdata_node_window_begin(n, (uint32_t)hw_time_ms());
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_WAKE, 0);
    bool reboot = false;
    unsigned frames = 0, acted = 0;
    while (iotdata_node_window_active(n, (uint32_t)hw_time_ms())) {
        int len = 0, rssi_dbm = 0;
        if (lora_read(as->running->packet_buffer, sizeof(as->running->packet_buffer), &len, &rssi_dbm, 0) == ESP_OK && len > 0) {
            frames++;
            acted += iotdata_node_on_frame(n, as->running->packet_buffer, (size_t)len, &reboot) ? 1 : 0;
        }
        iotdata_node_diagnostics_pump();        /* a DUMP that arrived in this window drains inside it */
        hw_delay_ms_yieldable(RECEIVE_POLL_MS); /* yield and pat the watchdog: the window is long by MCU standards */
    }
    iotdata_node_window_end(n);
    ESP_LOGI(__tag_iapp, "receive: window closed (%u frame(s) heard, %u for us)", frames, acted);

    if (reboot) {
        ESP_LOGW(__tag_iapp, "node: REBOOT commanded -- restarting");
        iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_STOP, 0);
        iotdata_node_diagnostics_flush();
        hw_delay_ms_yieldable(CONSOLE_DRAIN_MS);
        esp_restart();
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static void iapp_fail(iapp_state_t *const as) {

    const app_descr_t *const ad = app_spec;

    if (ad->func_fail)
        ad->func_fail(&app_state, &as->operating->node);

    // don't close the datastore or do anything: we don't know what kind of bad situation the app is in
    ESP_LOGE(__tag_iapp, "failure, restarting in %ds", RESTART_MS);
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_STOP, 0);
    hw_delay_ms_yieldable(RESTART_MS);
    esp_restart();
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iapp_init(iapp_state_t *const as) {

    const app_descr_t *const ad = app_spec;
    iotdata_node_t *const node = &as->operating->node;

    // capture reset reason
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool restarted = (reset_reason != ESP_RST_DEEPSLEEP);
    if (restarted)
        as->operating->restarts++;

    const bool cold_boot = reset_reason == ESP_RST_USB || !_RTC_DATA_VALID(as->operating, IAPP_STATE_MAGIC);
    const int16_t brownout_mv = cold_boot ? 0 : as->operating->battery_mv;
    if (cold_boot) {
        _RTC_DATA_INIT(as->operating, IAPP_STATE_MAGIC);
        iotdata_node_init(node, iotdata_node_station_from_mac(__tag_iapp), NULL, 0u); // station_id may be changed in persistence load
    }
    iotdata_node_attach(node, &as->running->node_config);

    // initial banner, before anything else
    char buffer[80] = { 0 };
    if (ad->func_init_log)
        ad->func_init_log(&app_state, node, buffer, sizeof(buffer));
    ESP_LOGI(__tag_iapp, "app :: %s :: variant: %s%s%s", ad->name, iotdata_vsuite_name(ad->variant), buffer[0] != '\0' ? ", details: " : "", buffer);
    ESP_LOGI(__tag_iapp, "boot: reset_reason=%d %s", (int)reset_reason, reset_reason_str(reset_reason));

    // bring up diagnostics and flush any pending records if in RTC
    iotdata_node_diagnostics_emit_set(_iapp_diagnostics_emit);
    if (!iotdata_node_diagnostics_begin((uint8_t)reset_reason, restarted))
        ESP_LOGW(__tag_iapp, "diag: the recorder did not start");
    iotdata_node_diagnostics_flush();

    // deal with power reset issues
    if (reset_reason == ESP_RST_BROWNOUT && brownout_mv > 0) {
        power_startup(brownout_mv);
        ESP_LOGE(__tag_iapp, "boot: BROWNOUT -- the last reading before it was %dmV", (int)brownout_mv);
    }

    // any app specific entry activities, failable
    if (ad->func_init_entry && ad->func_init_entry(&app_state, node, reset_reason) != IAPP_EV_OK)
        return false;

#if BATTERY_SELFTEST_MS > 0
    (void)battery_test(BATTERY_SELFTEST_MS);
#endif

    // capabilities and the fitted-hardware probe, cold boot only
    if (cold_boot) {
        iotdata_node_version_caps_init(&as->operating->caps);
        as->operating->battery_present = BATTERY_PROBE ? battery_probe() : false;
        if (ad->func_init_rtc)
            ad->func_init_rtc(&app_state, node, &as->operating->caps);
    }

    // dump out version information
    char vbuf[IOTDATA_NODE_VERSION_STR_MAX + 1];
    ESP_LOGI(__tag_iapp, "version: %s", iotdata_node_version_str(vbuf, sizeof(vbuf), &as->operating->caps));
    if (!iotdata_node_version_stamp_is_real())
        ESP_LOGW(__tag_iapp, "version: build stamp is unset -- this binary cannot say when it was built");

    // open the datastore and hydrate storage
    if (datastore_open(&as->running->datastore, "iotdata")) {
        iotdata_node_state_init(&as->running->node, &as->running->datastore, "state");
        iotdata_node_settings_defaults(&as->running->node_settings, iotdata_node_station(&as->operating->node));
        if (!iotdata_node_settings_attach(&as->running->node_settings, &as->running->node))
            ESP_LOGW(__tag_iapp, "state: persistence not available, protocol settings will not persist");
        if (!iotdata_node_bind(&as->operating->node, &as->running->node, IAPP_STATE_TAG))
            ESP_LOGW(__tag_iapp, "state: persistence binding failed");
        const bool restored = iotdata_node_state_load(&as->running->node);
        ESP_LOGI(__tag_iapp, "state: persistence %s %u block(s)", restored ? "restored" : "defaulted", (unsigned)as->running->node.count);
        if (iotdata_node_restore_station(&as->operating->node, &as->running->node_settings))
            ESP_LOGI(__tag_iapp, "node: station=%" PRIu16 " restored from persisted settings", iotdata_node_station(&as->operating->node));
        if (!iotdata_node_config_load(&as->running->datastore))
            ESP_LOGI(__tag_iapp, "config: persistence empty, using defaults");
        if (!iotdata_node_config_batt_apply())
            ESP_LOGW(__tag_iapp, "battery: persisted profile refused, using the built-in one");
        if (ad->func_init_datastore && ad->func_init_datastore(&app_state, node, &as->running->datastore) != IAPP_EV_OK) {
            datastore_close(&as->running->datastore);
            return false;
        }
    } else
        ESP_LOGW(__tag_iapp, "state: persistence disabled (no datastore)");

    // record power state
    if (cold_boot)
        power_describe(as->operating->battery_present);

    // any app specific final activities, failable
    if (ad->func_init_final && ad->func_init_final(&app_state, node, reset_reason) != IAPP_EV_OK)
        return false;

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iapp_sleep(iapp_state_t *const as, const uint32_t start_ms, bool deep) {

    const app_descr_t *const ad = app_spec;
    iotdata_node_t *const node = &as->operating->node;

    const uint32_t awake_ms = hw_time_ms() - start_ms;
    uint32_t sleep_ms = 0;
    if (!deep) {
        if (ad->func_sleep && ad->func_sleep(&app_state, node, &sleep_ms, start_ms, awake_ms, false) != IAPP_EV_OK)
            return false;
        iapp_cycle_elapsed(as, awake_ms + sleep_ms);
        iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_SLEEP, iotdata_node_diagnostics_reason(sleep_ms / 1000));
        iotdata_node_diagnostics_flush();
        ESP_LOGI(__tag_iapp, "sleep light %" PRIu32 "ms (awake %" PRIu32 "ms) -- cycle %" PRIu32 ", tx=%" PRIu32 " errors=%" PRIu32, sleep_ms, awake_ms, as->operating->tx_cycles, as->operating->tx_count, as->operating->tx_errors);
        hw_delay_ms_yieldable(CONSOLE_DRAIN_MS);
        hw_delay_ms_yieldable(sleep_ms);
    } else {
        if (ad->func_sleep && ad->func_sleep(&app_state, node, &sleep_ms, start_ms, awake_ms, true) != IAPP_EV_OK)
            return false;
        iapp_cycle_elapsed(as, awake_ms + sleep_ms);
        iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_SLEEP, iotdata_node_diagnostics_reason(sleep_ms / 1000));
        iotdata_node_diagnostics_flush();
        lora_hold();
        datastore_close(&as->running->datastore);
        ESP_LOGI(__tag_iapp, "sleep deep %" PRIu32 "ms (awake %" PRIu32 "ms) -- cycle %" PRIu32 ", tx=%" PRIu32 " errors=%" PRIu32, sleep_ms, awake_ms, as->operating->tx_cycles, as->operating->tx_count, as->operating->tx_errors);
        hw_delay_ms_yieldable(CONSOLE_DRAIN_MS);
        esp_deep_sleep((uint64_t)sleep_ms * 1000); /* does not return */
    }
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iapp_exec(iapp_state_t *const as) {

    const app_descr_t *const ad = app_spec;
    iotdata_node_t *const node = &as->operating->node;

    do {
        const uint32_t start_ms = hw_time_ms();

        const bool receive = iotdata_node_window_pending(&as->operating->node);

        const int16_t battery_mv_was = as->operating->battery_mv;
        battery_reading_t battery_reading = { 0 };
        const battery_reading_t *battery = NULL;
        if (as->operating->battery_present && battery_begin()) {
            if (battery_read(&battery_reading, &as->operating->battery_mv))
                battery = &battery_reading;
            else
                ESP_LOGE(__tag_iapp, "battery: unable to read");
            battery_end();
        }

        size_t transmit = 0;
        iapp_event_t ev = ad->func_exec_packet != NULL
                              ? ad->func_exec_packet(&app_state, node, as->operating->tx_cycles, as->operating->battery_present, battery, battery_mv_was, as->running->packet_buffer, sizeof(as->running->packet_buffer), receive, &transmit)
                              : IAPP_EV_OK;
        if (ev == IAPP_EV_FAIL)
            return false;
        as->operating->tx_cycles++;

        if (transmit || receive) {
            if (!as->operating->lora_present) {
                lora_config_t lora_cfg;
                iotdata_node_config_lora_apply(&lora_cfg);
                esp_log_level_set(__tag_device_e22900t22, lora_cfg.debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);
                as->operating->lora_present = lora_setup(&lora_cfg) == ESP_OK;
            }
            const bool lora_started = as->operating->lora_present && lora_start() == ESP_OK;
            if (!lora_started)
                ESP_LOGE(__tag_iapp, "lora: start failed, no transmissions this cycle");
            if (transmit) {
                if (lora_started && lora_write_complete(as->running->packet_buffer, transmit, /*wait_complete=*/true) == ESP_OK) {
                    ESP_LOGI(__tag_iapp, "packet: transmit success (%zu bytes)", transmit);
                    iotdata_node_sequence_used(&as->operating->node);
                    as->operating->tx_count++;
                } else {
                    ESP_LOGE(__tag_iapp, "packet: transmit failed (%zu bytes)", transmit);
                    as->operating->tx_errors++;
                    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_ERROR, iotdata_node_diagnostics_reason(as->operating->tx_errors));
                }
            }
            if (lora_started && receive)
                iapp_node_receive(as);
            if (lora_started)
                lora_stop();
        }

        ev = ad->func_exec_final != NULL ? ad->func_exec_final(&app_state, node, start_ms) : IAPP_EV_OK;
        if (ev == IAPP_EV_FAIL)
            return false;
        iotdata_node_state_flush(&as->running->node);
        if (ev == IAPP_EV_SLEEP_LIGHT || ev == IAPP_EV_SLEEP_DEEP) {
            if (!iapp_sleep(as, start_ms, ev == IAPP_EV_SLEEP_DEEP))
                return false;
        } else
            iapp_cycle_elapsed(as, hw_time_ms() - start_ms); /* a cycle that did not sleep still took time */

        esp_task_wdt_reset();

    } while (true);

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

void app_main(void) {

    setbuf(stdout, NULL);

    const esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK)
        ESP_LOGE(__tag_iapp, "task watchdog: subscribe failed: %s", esp_err_to_name(wdt_err));
    else
        ESP_LOGI(__tag_iapp, "task watchdog: subscribed (timeout=%ds)", CONFIG_ESP_TASK_WDT_TIMEOUT_S);

    iapp_state_t *const as = &iapp_state;
    if (!iapp_init(as) || !iapp_exec(as))
        iapp_fail(as);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
