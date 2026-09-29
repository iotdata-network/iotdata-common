// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_timer.h"
#include "driver/gpio.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_led = "led";

// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    LED_PATTERN_OFF = 0,
    LED_PATTERN_SOLID,
    LED_PATTERN_BLINK,
    LED_PATTERN_PULSE,
} led_pattern_t;

const char *led_pattern_to_str(const led_pattern_t pattern) {
    switch (pattern) {
    case LED_PATTERN_OFF:
        return "off";
    case LED_PATTERN_SOLID:
        return "solid";
    case LED_PATTERN_BLINK:
        return "blink";
    case LED_PATTERN_PULSE:
        return "pulse";
    default:
        return "unknown";
    }
}

#define LED_BLINK_MS_DEFAULT      150
// Arg for BLINK: number of blinks (1, 2, 3, ...)
// Arg for PULSE: half-period in ms (on-time = off-time = arg)
#define LED_PULSE_SLOW_MS_DEFAULT 500 // 1Hz, active/working
#define LED_PULSE_FAST_MS_DEFAULT 100 // 5Hz, error condition

typedef struct {
    gpio_num_t gpio_led;
} led_pins_t;

typedef struct {
    led_pins_t pins;
    uint16_t blink_ms; // Half-period for blink on/off
    uint16_t pulse_slow_ms;
    uint16_t pulse_fast_ms;
} led_config_t;

typedef struct {
    const led_config_t *config;
    esp_timer_handle_t timer;
    volatile uint32_t timer_count;
    volatile bool busy;
    volatile led_pattern_t pattern;
    volatile int blink_count;
    volatile int blink_state;
} led_handle_t;

#define LED_LEVEL_ON                                     0
#define LED_LEVEL_OFF                                    1

#define __LED_SETTLE_US                                  200
#define __LED_FACTORY_TEST_DELAY_BETWEEN_BLINK_CYCLES_MS 1000
#define __LED_FACTORY_TEST_DELAY_COMPLETE_MS             10
#define __LED_FACTORY_TEST_DELAY_CYCLE_MS                100

// ------------------------------------------------------------------------------------------------------------------------

void led_timer_callback(void *const arg) {

    led_handle_t *handle = (led_handle_t *)arg;

    handle->timer_count++;

    switch (handle->pattern) {
    case LED_PATTERN_BLINK:
        ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, (uint32_t)(handle->blink_state++ % 2)));
        if (handle->blink_state >= handle->blink_count * 2) {
            ESP_ERROR_CHECK(esp_timer_stop(handle->timer));
            handle->busy = false;
        }
        break;

    case LED_PATTERN_PULSE:
        ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, (uint32_t)(handle->blink_state++ % 2)));
        break;

    case LED_PATTERN_OFF:
    case LED_PATTERN_SOLID:
    default:
        ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));
        ESP_ERROR_CHECK(esp_timer_stop(handle->timer));
        handle->busy = false;
        break;
    }
}

void led_timer_init(led_handle_t *const handle) {

    if (!handle->timer)
        ESP_ERROR_CHECK(esp_timer_create(
            &(const esp_timer_create_args_t){
                .callback = led_timer_callback,
                .arg = handle,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "led_timer_callback",
            },
            &handle->timer));
}

void led_timer_term(led_handle_t *const handle) {

    if (handle->timer) {
        ESP_ERROR_CHECK(esp_timer_delete(handle->timer));
        handle->timer = NULL;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

void led_stop(led_handle_t *const handle) {

    if (handle->busy) {
        if (handle->pattern == LED_PATTERN_BLINK || handle->pattern == LED_PATTERN_PULSE)
            ESP_ERROR_CHECK(esp_timer_stop(handle->timer));
        ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));
        handle->busy = false;
    }
}

void led_start(led_handle_t *const handle, const led_pattern_t pattern, const uint16_t arg) {

    led_stop(handle);

    handle->pattern = pattern;
    handle->blink_state = 0;

    switch (pattern) {
    case LED_PATTERN_SOLID:
        ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_ON));
        handle->busy = true;
        return;

    case LED_PATTERN_BLINK:
        handle->blink_count = arg > 0 ? (int)arg : 1;
        led_timer_init(handle);
        handle->busy = true;
        ESP_ERROR_CHECK(esp_timer_start_periodic(handle->timer, handle->config->blink_ms * US_PER_MS));
        return;

    case LED_PATTERN_PULSE:
        led_timer_init(handle);
        handle->busy = true;
        ESP_ERROR_CHECK(esp_timer_start_periodic(handle->timer, (arg > 0 ? arg : handle->config->pulse_slow_ms) * US_PER_MS));
        return;

    case LED_PATTERN_OFF:
    default:
        return;
    }
}

bool led_busy(led_handle_t *const handle) {
    return handle->busy;
}

int led_state(led_handle_t *const handle) {
    return (int)gpio_get_level(handle->config->pins.gpio_led);
}

typedef enum {
    LED_STATUS_OK = 0,
    LED_STATUS_OPEN_CIRCUIT,
    LED_STATUS_SHORT_CIRCUIT,
} led_status_t;

const char *led_status_to_str(const led_status_t status) {
    switch (status) {
    case LED_STATUS_OK:
        return "ok";
    case LED_STATUS_OPEN_CIRCUIT:
        return "open-circuit";
    case LED_STATUS_SHORT_CIRCUIT:
        return "short-circuit";
    default:
        return "unknown";
    }
}

led_status_t led_status(led_handle_t *const handle) {

    // Circuit: VCC -> R1(470Ω) -> LED(A->K) -> GPIO <- R2(10k) <- VCC
    //
    // With external pullup R2 permanently connected, input-mode pull probing
    // is limited. Detectable faults:
    //   - GPIO pin dead or stuck (can't drive HIGH/LOW)
    //   - External pullup R2 missing or trace shorted to GND
    //   - Dead short on LED output (cathode shorted to GND externally)
    // Not reliably detectable from software alone:
    //   - LED open vs LED present (R2 pulls HIGH in both cases)

    // Test 1: Input mode, no internal pulls - R2 (10k) should pull HIGH
    ESP_ERROR_CHECK(gpio_set_direction(handle->config->pins.gpio_led, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(handle->config->pins.gpio_led, GPIO_FLOATING));
    hw_delay_us_precise(__LED_SETTLE_US);
    const int level_float = gpio_get_level(handle->config->pins.gpio_led);

    // Test 2: Output LOW - GPIO should sink current (LED on if present)
    ESP_ERROR_CHECK(gpio_set_direction(handle->config->pins.gpio_led, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_ON));
    hw_delay_us_precise(__LED_SETTLE_US);
    const int level_on = gpio_get_level(handle->config->pins.gpio_led);

    // Test 3: Output HIGH - LED off, no current
    ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));
    hw_delay_us_precise(__LED_SETTLE_US);
    const int level_off = gpio_get_level(handle->config->pins.gpio_led);

    // Restore output mode with LED off
    ESP_ERROR_CHECK(gpio_set_direction(handle->config->pins.gpio_led, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));

    ESP_LOGD(__tag_device_led, "status: float=%d, on=%d, off=%d", level_float, level_on, level_off);

    // level_float=1: R2 pulling to VCC (expected)
    // level_on=0:    GPIO driving LOW successfully (expected, active-low ON)
    // level_off=1:   GPIO driving HIGH successfully (expected, active-low OFF)

    if (level_float == 0)
        return LED_STATUS_SHORT_CIRCUIT; // pin shorted to GND or R2 broken

    if (level_on != 0 || level_off != 1)
        return LED_STATUS_OPEN_CIRCUIT; // GPIO can't drive expected levels

    return LED_STATUS_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

bool led_conf(void *const _handle, const void *const _config) {

    led_handle_t *handle = (led_handle_t *)_handle;
    const led_config_t *config = (const led_config_t *)_config;

    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(config->pins.gpio_led));

    handle->config = config;

    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

void led_hardware_init(void *const _handle) {

    led_handle_t *handle = (led_handle_t *)_handle;

    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = BIT(handle->config->pins.gpio_led),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, // external 10k pullup R2 handles boot-safe state
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
    ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));

    ESP_LOGI(__tag_device_led, "enabled, gpio-led=%d, active=low", handle->config->pins.gpio_led);
}

void led_hardware_term(void *const _handle) {

    led_handle_t *handle = (led_handle_t *)_handle;

    ESP_ERROR_CHECK(gpio_set_level(handle->config->pins.gpio_led, LED_LEVEL_OFF));
    ESP_ERROR_CHECK(gpio_set_direction(handle->config->pins.gpio_led, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(handle->config->pins.gpio_led, GPIO_FLOATING)); // external R2 pullup holds HIGH

    ESP_LOGI(__tag_device_led, "disabled");
}

// ------------------------------------------------------------------------------------------------------------------------

void led_init(__attribute__((unused)) void *const _handle) {
}

void led_term(void *const _handle) {

    led_handle_t *handle = (led_handle_t *)_handle;

    led_stop(handle);
    led_timer_term(handle);
}

// ------------------------------------------------------------------------------------------------------------------------
//
// NOT PORTED: led_test_poweron() and led_test_factory(). Both speak the SDS
// application's own startup vocabulary -- poweron_result_t, APP_FLAGS_CIRCUIT_BOARD -- which is an
// application's way of reporting its own health, not anything an LED driver needs in order to be
// one. The original, framework and all, is parked at iotdata-device/src.deprecated/led.h.
//
// What they were checking is still here: led_status() drives the pin both ways and reads it back,
// which is what detects a GPIO shorted to ground or a missing pull-up.
//
// ------------------------------------------------------------------------------------------------------------------------
