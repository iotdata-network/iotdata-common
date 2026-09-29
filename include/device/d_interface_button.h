// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

/* esp_deep_sleep_enable_gpio_wakeup() was renamed in IDF 6.1 to
   esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(); the arguments and the wake-mode enum are
   unchanged. Nothing here deep sleeps today, but the module is written to survive one. */
const char *__tag_device_button = "button";

// ------------------------------------------------------------------------------------------------------------------------

#define BUTTON_DEBOUNCE_MS_DEFAULT         50
#define BUTTON_DOUBLE_CLICK_MS_DEFAULT     500
#define BUTTON_LONG_PRESS_MS_DEFAULT       10000
#define BUTTON_EXTRA_LONG_PRESS_MS_DEFAULT 20000
#define BUTTON_STUCK_MS_DEFAULT            60000

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SINGLE_PRESS,
    BUTTON_EVENT_DOUBLE_PRESS,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_EXTRA_LONG_PRESS,
    BUTTON_EVENT_STUCK,
} button_event_type_t;

const char *button_event_type_to_str(const button_event_type_t type) {
    switch (type) {
    case BUTTON_EVENT_SINGLE_PRESS:
        return "single-press";
        break;
    case BUTTON_EVENT_DOUBLE_PRESS:
        return "double-press";
        break;
    case BUTTON_EVENT_LONG_PRESS:
        return "long-press";
        break;
    case BUTTON_EVENT_EXTRA_LONG_PRESS:
        return "extra-long-press";
        break;
    case BUTTON_EVENT_STUCK:
        return "stuck";
        break;
    case BUTTON_EVENT_NONE:
        return "none";
        break;
    default:
        return "unknown";
        break;
    }
}

typedef struct {
    button_event_type_t type;
    int64_t timestamp_us;
    uint32_t event_id;
} button_event_t;

#define BUTTON_EVENT_BUFFER_SIZE 8

typedef struct {
    gpio_num_t gpio_button;
} button_pins_t;

typedef struct {
    button_pins_t pins;
    uint16_t debounce_ms;
    uint16_t double_click_ms;
    uint32_t long_press_ms;
    uint32_t extra_long_press_ms;
    uint32_t stuck_timeout_ms;
} button_config_t;

typedef struct {
    const button_config_t *config;
} button_handle_t;

// ------------------------------------------------------------------------------------------------------------------------

#define _BUTTON_FACTORY_TEST_EVENTS_CYCLES_MS 500
#define _BUTTON_FACTORY_TEST_LISTEN_CYCLES_MS 10
#define _BUTTON_FACTORY_TEST_STUCK_DELAY_MS   2000
#undef _BUTTON_FACTORY_TEST_GPIO_RAW

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    button_config_t config;
    // ring buffer
    button_event_t events[BUTTON_EVENT_BUFFER_SIZE];
    int write_index, read_index;
    uint32_t event_count;
    int64_t press_time; // start of validated press, only set on first falling when not pressed, 0 when released
    int64_t click_time; // timestamp of last single click (valid falling to rising duration)
    // debug
    uint32_t isr_total;
    uint32_t isr_debounced;
} _button_rtc_t;

#define _BUTTON_STORE_RTC_MAGIC 0xB7704570
_RTC_DATA_STRUCT_ISR _button_rtc_t _button_rtc;
#define _BUTTON_STORE_RTC_INIT()  _RTC_DATA_INIT_ISR(&_button_rtc, _BUTTON_STORE_RTC_MAGIC)
#define _BUTTON_STORE_RTC_VALID() _RTC_DATA_VALID(&_button_rtc, _BUTTON_STORE_RTC_MAGIC)

// ------------------------------------------------------------------------------------------------------------------------

void IRAM_ATTR _button_store_event(const int64_t now, const button_event_type_t type) {
    _button_rtc.events[_button_rtc.write_index].type = type;
    _button_rtc.events[_button_rtc.write_index].timestamp_us = now;
    _button_rtc.events[_button_rtc.write_index].event_id = ++_button_rtc.event_count;
    _button_rtc.write_index = (_button_rtc.write_index + 1) % BUTTON_EVENT_BUFFER_SIZE;
    if (_button_rtc.write_index == _button_rtc.read_index)
        _button_rtc.read_index = (_button_rtc.read_index + 1) % BUTTON_EVENT_BUFFER_SIZE;
}

void IRAM_ATTR _button_store_resolve_pending_single(const int64_t now) {
    if ((now - _button_rtc.click_time) < 0) // Stale data from a previous boot — reset state
        _button_rtc.click_time = 0;
    else if (_button_rtc.press_time == 0 && _button_rtc.click_time > 0 && (now - _button_rtc.click_time) > (int64_t)_button_rtc.config.double_click_ms * US_PER_MS) {
        _button_store_event(_button_rtc.click_time, BUTTON_EVENT_SINGLE_PRESS);
        _button_rtc.click_time = 0;
    }
}

void IRAM_ATTR button_isr_handler(__attribute__((unused)) void *const arg) {

    _button_rtc.isr_total++;

    const int64_t now = esp_timer_get_time();

    if (gpio_get_level(_button_rtc.config.pins.gpio_button) == 0) {
        // Pin is LOW (pressed) — record start only if not already in a press
        if (_button_rtc.press_time == 0)
            _button_rtc.press_time = now;
    } else {
        // Pin is HIGH (released)
        if (_button_rtc.press_time == 0) {
            // No press recorded — spurious rising edge, ignore
        } else {
            const int64_t press_duration = now - _button_rtc.press_time;
            if (press_duration < 0) {
                // Junk left over from previous boot
                _button_rtc.click_time = 0;
                _button_rtc.press_time = 0;
            } else if (press_duration < _button_rtc.config.debounce_ms * US_PER_MS) {
                // Too short AND pin is HIGH — this is release-bounce creating
                // a phantom press. Clear it. We KNOW the button is released because we read
                // the level, so there's no real press to protect.
                _button_rtc.press_time = 0;
                _button_rtc.isr_debounced++;
            } else {
                // Valid press — classify and clear
                if (press_duration > _button_rtc.config.stuck_timeout_ms * US_PER_MS) {
                    _button_store_event(now, BUTTON_EVENT_STUCK);
                    _button_rtc.click_time = 0;
                } else if (press_duration > _button_rtc.config.extra_long_press_ms * US_PER_MS) {
                    _button_store_event(now, BUTTON_EVENT_EXTRA_LONG_PRESS);
                    _button_rtc.click_time = 0;
                } else if (press_duration > _button_rtc.config.long_press_ms * US_PER_MS) {
                    _button_store_event(now, BUTTON_EVENT_LONG_PRESS);
                    _button_rtc.click_time = 0;
                } else {
                    _button_store_resolve_pending_single(now);
                    if (_button_rtc.click_time > 0 && (now - _button_rtc.click_time) < _button_rtc.config.double_click_ms * US_PER_MS) {
                        _button_store_event(now, BUTTON_EVENT_DOUBLE_PRESS);
                        _button_rtc.click_time = 0;
                    } else {
                        _button_rtc.click_time = now;
                    }
                }
                _button_rtc.press_time = 0;
            }
        }
    }
}
// ------------------------------------------------------------------------------------------------------------------------

void button_apply_wakeup_deep(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return;
    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(_button_rtc.config.pins.gpio_button), _button_rtc.press_time ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
}

void button_apply_wakeup_light(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return;
    gpio_wakeup_enable(_button_rtc.config.pins.gpio_button, _button_rtc.press_time ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

// ------------------------------------------------------------------------------------------------------------------------

bool button_event_in_progress(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return false;
    _button_store_resolve_pending_single(esp_timer_get_time());
    return _button_rtc.click_time > 0;
}

bool button_event_timed_out(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return true;
    const int64_t now = esp_timer_get_time();
    _button_store_resolve_pending_single(now);
    return (now - _button_rtc.press_time > (_button_rtc.config.extra_long_press_ms * US_PER_MS) + (50 * US_PER_MS)); // nominal
}

int button_event_count(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return 0;
    _button_store_resolve_pending_single(esp_timer_get_time());
    return (_button_rtc.write_index >= _button_rtc.read_index) ? _button_rtc.write_index - _button_rtc.read_index : BUTTON_EVENT_BUFFER_SIZE - _button_rtc.read_index + _button_rtc.write_index;
}

bool button_event_fetch(button_event_t *const event) {
    if (!_BUTTON_STORE_RTC_VALID())
        return false;
    if (button_event_count() == 0)
        return false;
    if (event)
        *event = _button_rtc.events[_button_rtc.read_index];
    _button_rtc.read_index = (_button_rtc.read_index + 1) % BUTTON_EVENT_BUFFER_SIZE;
    return true;
}

void button_events_clear(void) {
    if (!_BUTTON_STORE_RTC_VALID())
        return;
    _button_rtc.read_index = _button_rtc.write_index;
}

// ------------------------------------------------------------------------------------------------------------------------

bool button_is_stuck(void *const _handle) {
    button_handle_t *handle = (button_handle_t *)_handle;
    return gpio_get_level(handle->config->pins.gpio_button) == 0;
}

typedef enum {
    BUTTON_STATUS_OK = 0,
    BUTTON_STATUS_STUCK, // Button held LOW (ice, debris, physical damage, or wiring fault)
} button_status_t;

const char *button_status_to_str(const button_status_t status) {
    switch (status) {
    case BUTTON_STATUS_OK:
        return "ok";
    case BUTTON_STATUS_STUCK:
        return "stuck";
    default:
        return "unknown";
    }
}

button_status_t button_status(button_handle_t *const handle) {

    // Circuit: VCC -> R(10k) -> GPIO <- Button <- GND
    //
    // With external pullup, button released = HIGH, button pressed = LOW.
    // A stuck-LOW reading (with nobody pressing) indicates ice, debris,
    // physical damage, or wiring fault.

    const int level = gpio_get_level(handle->config->pins.gpio_button);

    ESP_LOGD(__tag_device_button, "status: level=%d", level);

    if (level == 0)
        return BUTTON_STATUS_STUCK;

    return BUTTON_STATUS_OK;
}

void button_debug(button_handle_t *const handle) {
    if (!_BUTTON_STORE_RTC_VALID())
        ESP_LOGI(__tag_device_button, "button_events: bad rtc");
    else {
        const int64_t now_us = esp_timer_get_time();
#define _BUTTON_AGE_MS(ts) ((ts) > 0 && (ts) <= now_us ? (int64_t)((now_us - (ts)) / US_PER_MS) : (int64_t)-1)
        ESP_LOGD(__tag_device_button, "gpio=%d, wr/rd_idx=%d/%d, cnt=%" PRIu32 ", press_time=%" PRId64 "ms, click_time=%" PRId64 "ms, isr_total/debounced=%" PRIu32 "/%" PRIu32, (int)gpio_get_level(handle->config->pins.gpio_button),
                 _button_rtc.write_index, _button_rtc.read_index, _button_rtc.event_count, _BUTTON_AGE_MS(_button_rtc.press_time), _BUTTON_AGE_MS(_button_rtc.click_time), _button_rtc.isr_total, _button_rtc.isr_debounced);
#undef _BUTTON_AGE_MS
    }
}

// ------------------------------------------------------------------------------------------------------------------------

bool button_conf(void *const _handle, const void *const _config) {

    button_handle_t *handle = (button_handle_t *)_handle;
    const button_config_t *config = (const button_config_t *)_config;

    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(config->pins.gpio_button));
    ESP_ERROR_CHECK_BOOLEAN(esp_sleep_is_valid_wakeup_gpio(config->pins.gpio_button));

    if (!_BUTTON_STORE_RTC_VALID())
        _BUTTON_STORE_RTC_INIT();
    _button_rtc.config = *config;

    handle->config = config;

    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

void button_hardware_init(void *const _handle) {

    button_handle_t *handle = (button_handle_t *)_handle;

    esp_err_t ret;

    if ((ret = gpio_install_isr_service(0)) == ESP_OK) {

        const gpio_config_t gpio_conf = {
            .pin_bit_mask = BIT(handle->config->pins.gpio_button),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, // external 10k pullup handles this
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_conf));

        // ISR service wasn't installed - reconfigure
        ESP_ERROR_CHECK(gpio_isr_handler_add(handle->config->pins.gpio_button, button_isr_handler, NULL));
        ESP_ERROR_CHECK(gpio_intr_enable(handle->config->pins.gpio_button));

        if (gpio_get_level(handle->config->pins.gpio_button) == 0) {
            _button_rtc.press_time = esp_timer_get_time();
            ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(handle->config->pins.gpio_button), ESP_GPIO_WAKEUP_GPIO_HIGH));
        } else {
            _button_rtc.press_time = 0;
            ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(handle->config->pins.gpio_button), ESP_GPIO_WAKEUP_GPIO_LOW));
        }

        ESP_LOGI(__tag_device_button, "enabled, interrupt/gpio configured, gpio-button=%d, wake-on=%s", handle->config->pins.gpio_button, _button_rtc.press_time ? "high" : "low");

    } else if (ret == ESP_ERR_INVALID_STATE) {

        // ISR service already installed - we woke from deep sleep, so don't touch
        ESP_LOGD(__tag_device_button, "enabled, interrupt/gpio already installed, gpio-button=%d", handle->config->pins.gpio_button);

    } else {

        ESP_ERROR_CHECK(ret);
    }
}

void button_hardware_term(void *const _handle) {

    button_handle_t *handle = (button_handle_t *)_handle;

    ESP_ERROR_CHECK(gpio_intr_disable(handle->config->pins.gpio_button));
    ESP_ERROR_CHECK(gpio_isr_handler_remove(handle->config->pins.gpio_button));

    // esp_deep_sleep_disable_gpio_wakeup();

    ESP_ERROR_CHECK(gpio_reset_pin(handle->config->pins.gpio_button));

    // Note: We don't uninstall the ISR service itself since other modules might be using it
    // gpio_uninstall_isr_service() would affect all GPIO ISRs system-wide

    ESP_LOGI(__tag_device_button, "disabled");
}

// ------------------------------------------------------------------------------------------------------------------------

void button_init(void *const _handle) {
    (void)_handle;
}

void button_term(void *const _handle) {
    (void)_handle;
}
// ------------------------------------------------------------------------------------------------------------------------
//
// NOT PORTED: button_test_poweron() and button_test_factory(). Both are written
// against the SDS application's own test framework -- poweron_result_t, POWERON_OK/WARN/FAIL,
// APP_FLAGS_BUTTON_STUCK -- which is an application's vocabulary for reporting its own startup,
// not anything a button driver needs in order to be one. Bringing them here would have dragged
// that framework into every project that wants a button.
//
// The original, framework and all, is parked at iotdata-device/src.deprecated/button.h.
// What a caller needs to answer the same question is already here: button_status() reports
// STUCK for a button reading pressed while idle, which is the check the poweron test was making.
//
// ------------------------------------------------------------------------------------------------------------------------
