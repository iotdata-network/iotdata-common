
// ------------------------------------------------------------------------------------------------------------------------
// SPI
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "driver/spi_master.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

#define SPI_PORT_NUM       SPI2_HOST
#define SPI_CLK_FREQ_HZ    4000000 // 4 MHz (safe for SSD1680, up to 20)
#define _SPI_BUSY_DELAY_MS 25

const spi_host_device_t s_spi_port = SPI_PORT_NUM;
gpio_num_t s_spi_mosi, s_spi_clk, s_spi_dc, s_spi_busy, s_spi_cs;

spi_device_handle_t s_spi_dev = NULL;

// ------------------------------------------------------------------------------------------------------------------------

void _hw_spi_pins_enable(void) {
    // mosi/clk enabled by spi_bus_initialize()
    // DC pin: output, default low (command mode)
    hw_gpio_cfg_enable_output(s_spi_dc);
    // hw_gpio_set(s_spi_dc, false);
    // BUSY pin: input, no pull
    hw_gpio_cfg_enable_input(s_spi_busy, false);
}

void _hw_spi_pins_disable(void) {
    hw_gpio_cfg_disable_four(s_spi_mosi, s_spi_clk, s_spi_dc, s_spi_busy);
    if (s_spi_cs != GPIO_NUM_NC)
        hw_gpio_cfg_disable(s_spi_cs);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_start(const gpio_num_t mosi, const gpio_num_t clk, const gpio_num_t dc, const gpio_num_t busy, const gpio_num_t cs, const int max_transfer_size) {
    assert(!s_spi_dev);

    s_spi_mosi = mosi;
    s_spi_clk = clk;
    s_spi_dc = dc;
    s_spi_busy = busy;
    s_spi_cs = cs;

    esp_err_t ret = ESP_OK;

    _hw_spi_pins_enable();

    ESP_ERROR_CHECK(spi_bus_initialize(s_spi_port,
                                       &(const spi_bus_config_t){
                                           .mosi_io_num = mosi,
                                           .miso_io_num = GPIO_NUM_NC, // no MISO in this driver
                                           .sclk_io_num = clk,
                                           .quadwp_io_num = GPIO_NUM_NC,
                                           .quadhd_io_num = GPIO_NUM_NC,
                                           .max_transfer_sz = max_transfer_size,
                                       },
                                       SPI_DMA_CH_AUTO));

    ESP_GOTO_ON_ERROR(spi_bus_add_device(s_spi_port,
                                         &(const spi_device_interface_config_t){
                                             .clock_speed_hz = SPI_CLK_FREQ_HZ,
                                             .mode = 0,          // CPOL=0, CPHA=0
                                             .spics_io_num = cs, // CS may not be managed by driver
                                             .queue_size = 1,
                                         },
                                         &s_spi_dev),
                      hw_spi_start_failed, __func__, "spi_bus_add_device");

    ESP_LOGD("hw_spi", "started: mosi=%d, clk=%d, dc=%d, busy=%d, cs=%d, freq=%d", mosi, clk, dc, busy, cs, SPI_CLK_FREQ_HZ);

    return ESP_OK;

hw_spi_start_failed:
    ESP_ERROR_CHECK(spi_bus_free(s_spi_port));
    _hw_spi_pins_disable();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_cmd(const uint8_t cmd) {
    hw_gpio_set(s_spi_dc, false);                                                                   // command mode
    return spi_device_transmit(s_spi_dev, &(spi_transaction_t){ .length = 8, .tx_buffer = &cmd })); // note stack local variables are OK with spi_device_transmit but not if going to DMA
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_data(const uint8_t val) {
    hw_gpio_set(s_spi_dc, true);                                                                    // data mode
    return spi_device_transmit(s_spi_dev, &(spi_transaction_t){ .length = 8, .tx_buffer = &val })); // note stack local variables are OK with spi_device_transmit but not if going to DMA
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_data_bulk(const uint8_t *const data, const size_t len) {
    hw_gpio_set(s_spi_dc, true);                                                                          // data mode
    return spi_device_transmit(s_spi_dev, &(spi_transaction_t){ .length = len * 8, .tx_buffer = data })); // note stack local variables are OK with spi_device_transmit but not if going to DMA
}

// ------------------------------------------------------------------------------------------------------------------------

bool hw_spi_busy(void) {
    return hw_gpio_get(s_spi_busy);
}

D_WAIT_READY_FUNC(hw_spi_busy_wait, !hw_spi_busy, _SPI_BUSY_DELAY_MS, false, "hw_spi_busy_wait")

// ------------------------------------------------------------------------------------------------------------------------

void hw_spi_stop(void) {
    assert(s_spi_dev);

    ESP_ERROR_CHECK(spi_bus_remove_device(s_spi_dev));
    s_spi_dev = NULL;
    ESP_ERROR_CHECK(spi_bus_free(s_spi_port));
    _hw_spi_pins_disable();

    ESP_LOGD("hw_spi", "stopped: pins released");
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_cmd_data(const uint8_t cmd, const uint8_t data) {
    esp_err_t rc;
    if ((rc = hw_spi_cmd(cmd)) == ESP_OK)
        rc = hw_spi_data(data);
    return rc;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_cmd_data_bulk(const uint8_t cmd, const uint8_t *const data, const size_t len) {
    esp_err_t rc;
    if ((rc = hw_spi_cmd(cmd)) == ESP_OK)
        rc = hw_spi_data_bulk(data, len);
    return rc;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_spi_cmd_busy_wait(const uint8_t cmd, const uint32_t delay_ms, const uint32_t timeout_ms) {
    esp_err_t rc;
    if ((rc = hw_spi_cmd(cmd)) == ESP_OK) {
        if (delay_ms > 0)
            hw_delay_ms_precise(delay_ms);
        if (timeout_ms > 0)
            rc = hw_spi_busy_wait(timeout_ms) ? ESP_OK : DEV_ERR_TIMEOUT;
    }
    return rc;
}

// ------------------------------------------------------------------------------------------------------------------------
