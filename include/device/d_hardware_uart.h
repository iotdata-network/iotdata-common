
// ------------------------------------------------------------------------------------------------------------------------
// UART
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "driver/uart.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

#define UART_PORT_NUM            UART_NUM_1
#define UART_BAUD_DEFAULT        9600
#define UART_RX_BUF_SIZE_MIN     (UART_HW_FIFO_LEN(UART_PORT_NUM) + 1)
#define UART_RX_BUF_SIZE_DEFAULT 512
#define UART_TX_BUF_SIZE_MIN     (UART_HW_FIFO_LEN(UART_PORT_NUM) + 1)
#define UART_TX_BUF_SIZE_DEFAULT 256

const uart_port_t s_uart_port = UART_PORT_NUM;
gpio_num_t s_uart_tx, s_uart_rx;

bool s_uart_installed = false;

// ------------------------------------------------------------------------------------------------------------------------

void _hw_uart_pins_enable(void) {
    // tx/rx enabled by uart_set_pin()
}

void _hw_uart_pins_disable(void) {
    // tx/rx released by uart_driver_delete() anyway
    if (s_uart_tx != GPIO_NUM_NC && s_uart_rx != GPIO_NUM_NC)
        hw_gpio_cfg_disable_two(s_uart_tx, s_uart_rx);
    else if (s_uart_tx != GPIO_NUM_NC)
        hw_gpio_cfg_disable(s_uart_tx);
    else if (s_uart_rx != GPIO_NUM_NC)
        hw_gpio_cfg_disable(s_uart_rx);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_start(const gpio_num_t tx, const gpio_num_t rx, const int baud, const int rx_buf_size, const int tx_buf_size) {
    assert(!s_uart_installed);

    s_uart_tx = tx;
    s_uart_rx = rx;

    esp_err_t ret = ESP_OK;

    _hw_uart_pins_enable();

    ESP_ERROR_CHECK(uart_driver_install(s_uart_port, rx_buf_size, tx_buf_size, 0, NULL, 0));
    s_uart_installed = true;

    ESP_GOTO_ON_ERROR(uart_param_config(s_uart_port, &(const uart_config_t){
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#ifdef CONFIG_PM_ENABLE
        // Under power management the APB clock scales with the CPU (DFS), which corrupts the baud
        // divider and the peer reads back 0 bytes. XTAL is a fixed clock independent of DFS, so the
        // UART stays reliable. Only PM builds (e.g. the always-on relay) take this path; every other
        // consumer keeps UART_SCLK_DEFAULT byte-for-byte.
        .source_clk = UART_SCLK_XTAL,
#else
        .source_clk = UART_SCLK_DEFAULT,
#endif
    }), hw_uart_start_failed, __func__, "uart_param_config");
    ESP_GOTO_ON_ERROR(uart_set_pin(s_uart_port, tx, rx, GPIO_NUM_NC, GPIO_NUM_NC), hw_uart_start_failed, __func__, "uart_set_pin");
    ESP_GOTO_ON_ERROR(uart_flush_input(s_uart_port), hw_uart_start_failed, __func__, "uart_flush_input");

    ESP_LOGD("hw_uart", "started: tx=%d, rx=%d, baud=%d, rx_buf=%d, tx_buf=%d", tx, rx, baud, rx_buf_size, tx_buf_size);

    return ESP_OK;

hw_uart_start_failed:
    ESP_ERROR_CHECK(uart_driver_delete(s_uart_port));
    s_uart_installed = false;
    _hw_uart_pins_disable();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

int hw_uart_write(const uint8_t *const data, const size_t len) {
    return uart_write_bytes(s_uart_port, data, len);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_wait_tx_done(const int timeout_ms) {
    return uart_wait_tx_done(s_uart_port, pdMS_TO_TICKS(timeout_ms));
}

// ------------------------------------------------------------------------------------------------------------------------

int hw_uart_read(uint8_t *const buf, const size_t len, const int timeout_ms) {
    return uart_read_bytes(s_uart_port, buf, len, pdMS_TO_TICKS(timeout_ms));
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_flush(void) {
    return uart_flush_input(s_uart_port);
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_uart_stop(void) {
    assert(s_uart_installed);

    ESP_ERROR_CHECK(uart_driver_delete(s_uart_port));
    s_uart_installed = false;
    _hw_uart_pins_disable();

    ESP_LOGD("hw_uart", "stopped: pins released");
}

// ------------------------------------------------------------------------------------------------------------------------
