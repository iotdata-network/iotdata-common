
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

/* STATE PER PORT, and a port argument on every call.
 *
 * One controller re-pointed between devices is enough while they take turns -- a sense-and-send
 * cycle reads its sensors and only then powers the radio. It stops being enough the moment two
 * devices must be listening at the same time: a node that receives continuously cannot hand its
 * uart to a GNSS for thirty seconds and call what comes back a measurement of anything.
 *
 * So the port is a parameter, and each driver names the one it was given. Nothing here assumes a
 * single device any more, and a board that still multiplexes simply passes UART_PORT_NUM twice. */
#define HW_UART_PORTS            UART_NUM_MAX

// ------------------------------------------------------------------------------------------------------------------------

static gpio_num_t s_uart_tx[HW_UART_PORTS], s_uart_rx[HW_UART_PORTS];
static bool s_uart_installed[HW_UART_PORTS] = { false };

// ------------------------------------------------------------------------------------------------------------------------

void _hw_uart_pins_enable(void) {
    // tx/rx enabled by uart_set_pin()
}

// ------------------------------------------------------------------------------------------------------------------------

void _hw_uart_pins_disable(const uart_port_t port) {
    // tx/rx released by uart_driver_delete() anyway
    const gpio_num_t tx = s_uart_tx[port], rx = s_uart_rx[port];
    if (tx != GPIO_NUM_NC && rx != GPIO_NUM_NC)
        hw_gpio_cfg_disable_two(tx, rx);
    else if (tx != GPIO_NUM_NC)
        hw_gpio_cfg_disable(tx);
    else if (rx != GPIO_NUM_NC)
        hw_gpio_cfg_disable(rx);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_start(const uart_port_t port, const gpio_num_t tx, const gpio_num_t rx, const int baud, const int rx_buf_size, const int tx_buf_size) {
    assert(port >= 0 && port < HW_UART_PORTS);
    assert(!s_uart_installed[port]);

    s_uart_tx[port] = tx;
    s_uart_rx[port] = rx;

    esp_err_t ret = ESP_OK;

    _hw_uart_pins_enable();

    ESP_ERROR_CHECK(uart_driver_install(port, rx_buf_size, tx_buf_size, 0, NULL, 0));
    s_uart_installed[port] = true;

    ESP_GOTO_ON_ERROR(uart_param_config(port,
                                        &(const uart_config_t){
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
                                        }),
                      hw_uart_start_failed, __func__, "uart_param_config");
    ESP_GOTO_ON_ERROR(uart_set_pin(port, tx, rx, GPIO_NUM_NC, GPIO_NUM_NC), hw_uart_start_failed, __func__, "uart_set_pin");
    ESP_GOTO_ON_ERROR(uart_flush_input(port), hw_uart_start_failed, __func__, "uart_flush_input");

    ESP_LOGD("hw_uart", "started: port=%d, tx=%d, rx=%d, baud=%d, rx_buf=%d, tx_buf=%d", (int)port, tx, rx, baud, rx_buf_size, tx_buf_size);

    return ESP_OK;

hw_uart_start_failed:
    ESP_ERROR_CHECK(uart_driver_delete(port));
    s_uart_installed[port] = false;
    _hw_uart_pins_disable(port);
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

int hw_uart_write(const uart_port_t port, const uint8_t *const data, const size_t len) {
    return uart_write_bytes(port, data, len);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_wait_tx_done(const uart_port_t port, const int timeout_ms) {
    return uart_wait_tx_done(port, pdMS_TO_TICKS(timeout_ms));
}

// ------------------------------------------------------------------------------------------------------------------------

int hw_uart_read(const uart_port_t port, uint8_t *const buf, const size_t len, const int timeout_ms) {
    return uart_read_bytes(port, buf, len, pdMS_TO_TICKS(timeout_ms));
}

// ------------------------------------------------------------------------------------------------------------------------

size_t hw_uart_available(const uart_port_t port) {
    size_t n = 0;
    return (uart_get_buffered_data_len(port, &n) == ESP_OK) ? n : 0;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_uart_flush(const uart_port_t port) {
    return uart_flush_input(port);
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_uart_stop(const uart_port_t port) {
    assert(port >= 0 && port < HW_UART_PORTS);
    assert(s_uart_installed[port]);

    ESP_ERROR_CHECK(uart_driver_delete(port));
    s_uart_installed[port] = false;
    _hw_uart_pins_disable(port);

    ESP_LOGD("hw_uart", "stopped: port=%d, pins released", (int)port);
}

// ------------------------------------------------------------------------------------------------------------------------
