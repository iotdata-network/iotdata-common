
// ------------------------------------------------------------------------------------------------------------------------
// GPIO
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h" // esp_gpio_revoke
#pragma GCC diagnostic pop
#endif

#ifndef GPIO_IS_VALID_INPUT_GPIO
#define GPIO_IS_VALID_INPUT_GPIO(n) GPIO_IS_VALID_GPIO(n)
#endif

// ------------------------------------------------------------------------------------------------------------------------

void hw_gpio_cfg_enable_input(const gpio_num_t pin, const bool pullup) {
    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_gpio_cfg_enable_output(const gpio_num_t pin) {
    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_gpio_cfg_disable(const gpio_num_t pin) {
    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
}

void hw_gpio_cfg_disable_two(const gpio_num_t pin_a, const gpio_num_t pin_b) {
    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = (1ULL << pin_a) | (1ULL << pin_b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
}

void hw_gpio_cfg_disable_four(const gpio_num_t pin_a, const gpio_num_t pin_b, const gpio_num_t pin_c, const gpio_num_t pin_d) {
    ESP_ERROR_CHECK(gpio_config(&(const gpio_config_t){
        .pin_bit_mask = (1ULL << pin_a) | (1ULL << pin_b) | (1ULL << pin_c) | (1ULL << pin_d),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }));
}

// ------------------------------------------------------------------------------------------------------------------------

bool hw_gpio_get(const gpio_num_t pin) {
    return gpio_get_level(pin) != 0;
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_gpio_set(const gpio_num_t pin, const bool level) {
    ESP_ERROR_CHECK(gpio_set_level(pin, level ? 1 : 0));
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_gpio_revoke_two(const gpio_num_t pin_a, const gpio_num_t pin_b) {
    (void)esp_gpio_revoke((pin_a == GPIO_NUM_NC ? 0 : (1ULL << pin_a)) | (pin_b == GPIO_NUM_NC ? 0 : (1ULL << pin_b)));
}

// ------------------------------------------------------------------------------------------------------------------------
