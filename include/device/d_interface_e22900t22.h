
// ------------------------------------------------------------------------------------------------------------------------
// LoRa - E22-900T22 (UART, transparent mode)
// https://github.com/matthewgream/e22900t22/blob/main/specs/E22-900T22D_UserManual_EN_v1.3.pdf
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_e22900t22 = "device-e22900t22";

// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    LORA_MODULE_DIP = 0,
    LORA_MODULE_USB = 1,
} lora_module_t;

typedef enum { LORA_MODE_NORMAL, LORA_MODE_WAKE_ON_RECEIVE, LORA_MODE_CONFIG, LORA_MODE_DEEP_SLEEP } lora_mode_t;

#define LORA_E22_ADDRESS_DEFAULT            0x0008
#define LORA_E22_NETWORK_DEFAULT            0x00
#define LORA_CHANNEL_DEFAULT                0x0A
#define LORA_TRANSMIT_POWER_DEFAULT         22
#define LORA_AIR_DATA_RATE_DEFAULT          2400
#define LORA_PACKET_SIZE_DEFAULT            240
#define LORA_LISTEN_BEFORE_TRANSMIT_DEFAULT true
#define LORA_CRYPT_DEFAULT                  0x0000

typedef struct {
    lora_module_t module;
    uint16_t e22_address;
    uint8_t e22_network;
    uint8_t channel;
    uint8_t transmit_power;
    uint16_t air_data_rate;
    uint8_t packet_size;
    bool listen_before_transmit;
    uint16_t crypt;
    bool rssi_packet;
    bool rssi_channel;
} lora_config_t;

const lora_config_t lora_config_default = {
    .module = LORA_MODULE_DIP,
    .e22_address = LORA_E22_ADDRESS_DEFAULT,
    .e22_network = LORA_E22_NETWORK_DEFAULT,
    .channel = LORA_CHANNEL_DEFAULT,
    .transmit_power = LORA_TRANSMIT_POWER_DEFAULT,
    .air_data_rate = LORA_AIR_DATA_RATE_DEFAULT,
    .packet_size = LORA_PACKET_SIZE_DEFAULT,
    .listen_before_transmit = LORA_LISTEN_BEFORE_TRANSMIT_DEFAULT,
    .crypt = LORA_CRYPT_DEFAULT,
    .rssi_packet = true, /* both default ON: what the hardcoded registers did */
    .rssi_channel = true,
};

static inline uint8_t _lora_e22_air_data_rate(uint16_t rate) {
    if (rate == 300)
        return 0x00;
    else if (rate == 1200)
        return 0x01;
    else if (rate == 2400)
        return 0x02;
    else if (rate == 4800)
        return 0x03;
    else if (rate == 9600)
        return 0x04;
    else if (rate == 19200)
        return 0x05;
    else if (rate == 38400)
        return 0x06;
    else if (rate == 62500)
        return 0x07;
    else
        return 0x02;
}
#define _LORA_E22_CONFIG_AIR_DATA_RATE(n)  ((uint8_t)(_lora_e22_air_data_rate(n) & 0x07))
#define _LORA_E22_CONFIG_TRANSMIT_POWER(n) ((uint8_t)(((n) == 10) ? 0x03 : ((n) == 13 ? 0x02 : ((n) == 17 ? 0x01 : 0x00))))
#define _LORA_E22_CONFIG_PACKET_SIZE(n)    ((uint8_t)((((n) == 32) ? 0x03 : ((n) == 64 ? 0x02 : ((n) == 128 ? 0x01 : 0x00))) << 6))
#define _LORA_E22_CONFIG_LBT(n)            ((uint8_t)(((n) ? 0x01 : 0x00) << 4))

#define LORA_PACKET_SIZE_MAX               240

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_READY_DELAY_MS               10
/*
 * USB flow control, such as it is.
 *
 * On a DIP module AUX tells us when the module will accept a command, and every command waits on
 * it. A USB module exposes no AUX, so there is nothing to wait FOR -- and issuing a command before
 * the dongle is ready gets no response at all, not an error. The only available substitute is
 * time, which is what the 50ms `usleep` marked "yuck" in the reference serial_linux.h was actually
 * buying: it sat before every read and every write.
 *
 * Observed without it: the mode switch and the first config read succeed, then product-read and
 * config-write return 0 bytes -- every command that follows a *successful* one too closely is
 * simply not heard, while one that happens to follow a timeout works fine.
 */
#define _LORA_USB_SETTLE_MS                50
#define _LORA_SETUP_DELAY_MS               100
#define _LORA_SETTLE_DELAY_MS              500
/*
 * Response budget for a command. Generous on purpose: a config WRITE commits to the module's own
 * NVM before it answers, and the reference implementation effectively allowed far more than its
 * nominal 1000ms -- its serial_read waited that long for the FIRST byte and then permitted 100ms
 * per byte after, so a 12-byte response had ~2.1s. hw_uart_read is a total budget (matching
 * uart_read_bytes on the chip), so the number has to cover the whole exchange.
 */
#define _LORA_CMD_TIMEOUT_MS               3000
/* A config WRITE commits the register block to the module's own NVM before it answers, and only
   then sends the echo. Observed at ~3s on a USB dongle, i.e. right on the general command budget,
   so it gets its own. */
#define _LORA_CMD_TIMEOUT_SAVE_MS          8000
#define _LORA_TRANSMIT_WAIT_MS             1000
#define _LORA_SLEEP_DELAY_MS               5
#define _LORA_TRANSMIT_DELAY_MS            30000
#define _LORA_SAVE_DELAY_MS                100

// ------------------------------------------------------------------------------------------------------------------------

// E22 register layout (read from offset 0x00, 9 bytes)
// [0] ADDH  [1] ADDL  [2] NETID  [3] REG0  [4] REG1  [5] CH  [6] REG3
// [7] CRYPT_H  [8] CRYPT_L
#define _LORA_E22_REG_SIZE_CONFIG_READ     9
#define _LORA_E22_REG_SIZE_CONFIG_WRITE    9
#define _LORA_E22_REG_SIZE_PRODUCT_READ    7
#define _LORA_E22_CMD_SIZE_HEADER          3

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_E22_RSSI_DBM(b)              (-(256 - (int)(b)))

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    lora_config_t config;
    uint8_t product[_LORA_E22_REG_SIZE_PRODUCT_READ];
} _lora_rtc_t;

#define _LORA_RTC_MAGIC 0xDEE210AA
_RTC_DATA_STRUCT _lora_rtc_t _lora_rtc;
#define _LORA_RTC_INIT()                _RTC_DATA_INIT(&_lora_rtc, _LORA_RTC_MAGIC)
#define _LORA_RTC_VALID()               _RTC_DATA_VALID(&_lora_rtc, _LORA_RTC_MAGIC)

#define _LORA_CONFIG(entry)             ((_lora_rtc.config).entry)

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_PRODUCT_ID_VALID(product) (true)

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_IS_USB()                  (_LORA_CONFIG(module) == LORA_MODULE_USB)

void _lora_pins_enable(void) {
    if (!_LORA_IS_USB()) {
        /* Release any hold applied before deep sleep (see lora_hold); a no-op on a cold boot. Must come
        before the pins are reconfigured, or the hold fights the new direction. */
        gpio_deep_sleep_hold_dis();
        (void)gpio_hold_dis(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            (void)gpio_hold_dis(PIN_DEVICE_LORA_M0);
        hw_gpio_revoke_two(PIN_DEVICE_LORA_M0, PIN_DEVICE_LORA_M1);
        hw_gpio_cfg_enable_input(PIN_DEVICE_LORA_AUX, false);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M0);
        hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M1);
    }
}

void _lora_pins_disable(void) {
    if (!_LORA_IS_USB()) {
        hw_gpio_revoke_two(PIN_DEVICE_LORA_M0, PIN_DEVICE_LORA_M1);
        hw_gpio_cfg_disable(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            hw_gpio_cfg_disable(PIN_DEVICE_LORA_M0);
        hw_gpio_cfg_disable(PIN_DEVICE_LORA_AUX);
    }
}

// ------------------------------------------------------------------------------------------------------------------------

bool lora_is_ready(void) {
    return _LORA_IS_USB() ? true : hw_gpio_get(PIN_DEVICE_LORA_AUX);
}

static inline void _lora_settle(void) {
    if (_LORA_IS_USB())
        hw_delay_ms_yieldable(_LORA_USB_SETTLE_MS);
}

D_WAIT_READY_FUNC(lora_wait_ready, lora_is_ready, _LORA_READY_DELAY_MS, true, __tag_device_e22900t22)

// ------------------------------------------------------------------------------------------------------------------------

static int _lora_cmd_xfer_to(const uint8_t *const cmd, const size_t cmd_len, uint8_t *const res, const size_t res_len, const int timeout_ms) {
    _lora_settle();
    if (!lora_wait_ready(_LORA_TRANSMIT_WAIT_MS))
        return -1;
    if (hw_uart_flush() != ESP_OK)
        return -1;
    if (hw_uart_write(cmd, cmd_len) != (int)cmd_len)
        return -1;
    _lora_settle();
    if (!lora_wait_ready(_LORA_TRANSMIT_WAIT_MS))
        return -1;
    return hw_uart_read(res, res_len, timeout_ms);
}

static int _lora_cmd_xfer(const uint8_t *const cmd, const size_t cmd_len, uint8_t *const res, const size_t res_len) {
    return _lora_cmd_xfer_to(cmd, cmd_len, res, res_len, _LORA_CMD_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_mode_set_dip(const lora_mode_t mode) {
    bool m0 = false, m1 = false;
    if (mode == LORA_MODE_WAKE_ON_RECEIVE)
        m0 = true, m1 = false;
    else if (mode == LORA_MODE_CONFIG)
        m0 = false, m1 = true;
    else if (mode == LORA_MODE_DEEP_SLEEP)
        m0 = true, m1 = true;
    // else if (mode == LORA_MODE_NORMAL)          m0 = false, m1 = false;
    hw_gpio_set(PIN_DEVICE_LORA_M1, m1);
    if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
        hw_gpio_set(PIN_DEVICE_LORA_M0, m0);
    return mode == LORA_MODE_DEEP_SLEEP || lora_wait_ready(_LORA_SETTLE_DELAY_MS) ? ESP_OK : DEV_ERR_TIMEOUT;
}

esp_err_t _lora_mode_set_usb(const lora_mode_t mode) {
    uint8_t mode_byte;
    if (mode == LORA_MODE_NORMAL)
        mode_byte = 0x00;
    else if (mode == LORA_MODE_CONFIG)
        mode_byte = 0x01;
    else if (mode == LORA_MODE_WAKE_ON_RECEIVE)
        mode_byte = 0x02;
    else
        mode_byte = 0x03; // LORA_MODE_DEEP_SLEEP
    const uint8_t cmd[] = { 0xC0, 0xC1, 0xC2, 0xC3, 0x02, mode_byte };
    uint8_t res[sizeof(cmd)];
    const int want = (int)sizeof(cmd) - 1;
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, (size_t)want);
    if (n == 3 && res[0] == 0xFF && res[1] == 0xFF && res[2] == 0xFF) {
        /*
         * WARN, not DEBUG, and read the message carefully before believing it.
         *
         * FF FF FF is documented nowhere. The reference implementation calls it "already appears to
         * be in required mode, will accept" and returns success -- and that reading cost real
         * debugging time, because it is at least equally consistent with a plain NACK. Observed:
         * with REG1 bit 2 (switch-config-serial) clear, EVERY mode command answers FF FF FF while
         * the module stays put, config reads keep working, and data writes come back as FF FF FF
         * too. Nothing looks broken; nothing works.
         *
         * So it is accepted as success (the module may genuinely be in the mode we asked for) but
         * it is now audible. If you see it more than occasionally, suspect bit 2.
         */
        ESP_LOGW(__tag_device_e22900t22, "mode set (usb): mode %d not confirmed (FF FF FF) -- accepted, but check REG1 bit 2 if this repeats", (int)mode);
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(n >= want, ESP_FAIL, __tag_device_e22900t22, "mode set (usb): got %d bytes, expected %d", n, want);
    return ESP_OK;
}

esp_err_t _lora_mode_set(const lora_mode_t mode) {
    return _LORA_IS_USB() ? _lora_mode_set_usb(mode) : _lora_mode_set_dip(mode);
}

/*
 * Keep the module in the mode it was left in across the host's deep sleep.
 *
 * Deep sleep leaves the pads floating, which drops a DIP module straight out of the sleep mode
 * lora_stop() just selected -- so it wakes up drawing radio current for the whole interval instead
 * of ~2uA. Holding M0/M1 pins it there for as long as we are down. The hold is released in
 * _lora_pins_enable() on the next wake.
 *
 * Call immediately before esp_deep_sleep(), after lora_stop(). A no-op on USB, which has no pins
 * (and no host that deep-sleeps).
 */
void lora_hold(void) {
    if (!_LORA_IS_USB()) {
        (void)gpio_hold_en(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            (void)gpio_hold_en(PIN_DEVICE_LORA_M0);
        gpio_deep_sleep_hold_en();
    }
}

void _hack_lora_mode_deep_sleep_start(void) {
    _lora_pins_enable();
    _lora_mode_set(LORA_MODE_DEEP_SLEEP);
}
void _hack_lora_mode_deep_sleep_end(void) {
    _lora_pins_disable();
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_read_config(uint8_t *const config_data) {

    const uint8_t cmd[] = { 0xC1, 0x00, _LORA_E22_REG_SIZE_CONFIG_READ };
    uint8_t resp[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_READ];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), resp, sizeof(resp));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(resp), ESP_FAIL, __tag_device_e22900t22, "config read: got %d bytes, expected %d", n, (int)sizeof(resp));
    ESP_RETURN_ON_FALSE(resp[0] == 0xC1 && resp[1] == 0x00 && resp[2] == _LORA_E22_REG_SIZE_CONFIG_READ, ESP_FAIL, __tag_device_e22900t22, "config read: bad header %02" PRIX8 " %02" PRIX8 " %02" PRIX8, resp[0], resp[1], resp[2]);
    memcpy(config_data, resp + _LORA_E22_CMD_SIZE_HEADER, _LORA_E22_REG_SIZE_CONFIG_READ);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_write_config(const uint8_t *const config_data) {

    uint8_t cmd[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_WRITE] = { 0xC0, 0x00, _LORA_E22_REG_SIZE_CONFIG_WRITE };
    memcpy(cmd + _LORA_E22_CMD_SIZE_HEADER, config_data, _LORA_E22_REG_SIZE_CONFIG_WRITE);
    ESP_LOGD(__tag_device_e22900t22, "config_write: len=%d", (int)sizeof(cmd));
    uint8_t res[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_WRITE];
    const int n = _lora_cmd_xfer_to(cmd, sizeof(cmd), res, sizeof(res), _LORA_CMD_TIMEOUT_SAVE_MS);
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "config write: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1, ESP_FAIL, __tag_device_e22900t22, "config write: bad response header 0x%02" PRIX8, res[0]);
    for (int i = 0; i < _LORA_E22_REG_SIZE_CONFIG_WRITE; i++)
        ESP_RETURN_ON_FALSE(res[_LORA_E22_CMD_SIZE_HEADER + i] == config_data[i], ESP_FAIL, __tag_device_e22900t22, "config write: verify fail at [%d]: wrote 0x%02" PRIX8 ", read 0x%02" PRIX8, i, config_data[i],
                            res[_LORA_E22_CMD_SIZE_HEADER + i]);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_read_product(uint8_t *const info_data) {

    const uint8_t cmd[] = { 0xC1, 0x80, _LORA_E22_REG_SIZE_PRODUCT_READ };
    uint8_t res[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_PRODUCT_READ];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, sizeof(res));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "product read: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1, ESP_FAIL, __tag_device_e22900t22, "product read: bad response header 0x%02" PRIX8, res[0]);
    memcpy(info_data, res + _LORA_E22_CMD_SIZE_HEADER, _LORA_E22_REG_SIZE_PRODUCT_READ);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_read_product(void) {

    ESP_RETURN_ON_ERROR(_lora_cmd_read_product(_lora_rtc.product), __tag_device_e22900t22, "read_product: cmd_read_product");
    ESP_LOGD(__tag_device_e22900t22, "product: name=%d, ver=%d, power=%ddBm, freq=%d, type=%d", (int)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3], _lora_rtc.product[4],
             _lora_rtc.product[5]);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_read_and_update_config(void) {

    const uint8_t req_config[_LORA_E22_REG_SIZE_CONFIG_WRITE] = {
        (uint8_t)((_LORA_CONFIG(e22_address) >> 8)),                        // [ADDH = 0x00]
        (uint8_t)((_LORA_CONFIG(e22_address) & 0xFF)),                      // [ADDL = 0x08]
        _LORA_CONFIG(e22_network),                                          // [NETID = 0x00]
        0x60 | _LORA_E22_CONFIG_AIR_DATA_RATE(_LORA_CONFIG(air_data_rate)), // REG0: 0x60 = UART 9600/8N1, [air rate]
        /*
         * REG1: packet_size (7:6), rssi_channel (5), reserved (4:3), switch_config_serial (2),
         *       transmit_power (1:0)
         *
         * BIT 2 IS LOAD-BEARING ON USB. "Switch config by serial" is what makes the software mode
         * command (C0 C1 C2 C3 02 xx) work; the DIP module has M0/M1 instead and no such bit.
         * Clear it on a USB dongle and you lock the module in whatever mode it is in -- after
         * which every mode command answers FF FF FF, config reads still work (so it looks alive),
         * and any data write is rejected as a malformed command. Recoverable only because config
         * writes keep working, which is the one mercy.
         *
         * More generally: building this block from scratch means every bit we do not model gets
         * zeroed. The reference implementation read-modify-writes the config the device reports,
         * so it preserves such bits by accident of design. Worth remembering before adding a
         * field here.
         */
        (uint8_t)((_LORA_IS_USB() ? 0x04 : 0) | (_LORA_CONFIG(rssi_channel) ? 0x20 : 0) | _LORA_E22_CONFIG_PACKET_SIZE(_LORA_CONFIG(packet_size)) | _LORA_E22_CONFIG_TRANSMIT_POWER(_LORA_CONFIG(transmit_power))),
        _LORA_CONFIG(channel),                                                                                                 // [CH = 10 / 860.125 MHz]
        (uint8_t)((_LORA_CONFIG(rssi_packet) ? 0x80 : 0) | 0x03 | _LORA_E22_CONFIG_LBT(_LORA_CONFIG(listen_before_transmit))), // REG3: [RSSI packet], transparent, [LBT], WOR 2000ms
        (uint8_t)((_LORA_CONFIG(crypt) >> 8)),                                                                                 // [CRYPT_H = 0x00]
        (uint8_t)((_LORA_CONFIG(crypt) & 0xFF)),                                                                               // [CRYPT_L = 0x00]
    };

    uint8_t dev_config[_LORA_E22_REG_SIZE_CONFIG_READ];
    ESP_RETURN_ON_ERROR(_lora_cmd_read_config(dev_config), __tag_device_e22900t22, "setup: config read");
    char str[sizeof(dev_config) * 3]; // 'XX:...XX\0'
    ESP_LOGD(__tag_device_e22900t22, "CONFIG: %s", d_bytes_hex_str(str, sizeof(str), dev_config, sizeof(dev_config), ":"));

    const bool e22_config_differs = memcmp(dev_config, req_config, _LORA_E22_REG_SIZE_CONFIG_WRITE) != 0;
    if (e22_config_differs) {
        ESP_RETURN_ON_ERROR(_lora_cmd_write_config(req_config), __tag_device_e22900t22, "setup: config write");
        hw_delay_ms_yieldable(_LORA_SAVE_DELAY_MS);
    }

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_sleep(void) {

    ESP_RETURN_ON_ERROR(_lora_mode_set(LORA_MODE_DEEP_SLEEP), __tag_device_e22900t22, "sleep: mode deep sleep");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wjump-misses-init"
esp_err_t lora_setup(const lora_config_t *const config) {

    /* Assert the wiring before anything else -- except on USB, where there is none to assert.
       The config has to land in RTC first, since _LORA_IS_USB() reads it. */
    _LORA_RTC_INIT();
    memcpy(&_lora_rtc.config, config, sizeof(_lora_rtc.config));

    if (!_LORA_IS_USB()) {
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_UART_TX));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_UART_RX));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_LORA_AUX));
        ESP_ERROR_CHECK_BOOLEAN(PIN_DEVICE_LORA_M0 == GPIO_NUM_NC || GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_LORA_M0));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_LORA_M1));
    }

    ESP_LOGD(__tag_device_e22900t22, "setup: address=0x%04" PRIX16 ", network=0x%02" PRIX8 ", channel=%d, transmit_power=%d, air_data_rate=%d, packet_size=%d, LBT=%s, crypt=0x%04" PRIX16, _LORA_CONFIG(e22_address),
             _LORA_CONFIG(e22_network), _LORA_CONFIG(channel), _LORA_CONFIG(transmit_power), _LORA_CONFIG(air_data_rate), _LORA_CONFIG(packet_size), _LORA_CONFIG(listen_before_transmit) ? "true" : "false", _LORA_CONFIG(crypt));

    esp_err_t ret;

    _lora_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(PIN_DEVICE_UART_TX, PIN_DEVICE_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_MIN, UART_TX_BUF_SIZE_DEFAULT), __tag_device_e22900t22, "setup: uart start");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_CONFIG), lora_setup_exit, __tag_device_e22900t22, "setup: config mode");

    (void)_lora_read_product();
    ESP_GOTO_ON_FALSE(_LORA_PRODUCT_ID_VALID(_lora_rtc.product), DEV_ERR_PRODUCT_ID, lora_setup_exit, __tag_device_e22900t22, "setup: product invalid");

    ESP_GOTO_ON_ERROR(_lora_read_and_update_config(), lora_setup_exit, __tag_device_e22900t22, "setup: config");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_DEEP_SLEEP), lora_setup_exit, __tag_device_e22900t22, "setup: mode deep sleep");
    hw_uart_stop();
    _lora_pins_disable();

    ESP_LOGI(__tag_device_e22900t22, "setup: name=0x%04" PRIX16 ", version=%d, power=%ddBm", (uint16_t)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3]);

    return ESP_OK;

lora_setup_exit:
    (void)lora_sleep();
    hw_uart_stop();
    _lora_pins_disable();
    return ret;
}
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_start(void) {

    ESP_RETURN_ON_FALSE(_LORA_RTC_VALID(), DEV_ERR_RTC, __tag_device_e22900t22, "start: rtc invalid");
    ESP_RETURN_ON_FALSE(_LORA_PRODUCT_ID_VALID(lora_rtc.product), DEV_ERR_PRODUCT_ID, __tag_device_e22900t22, "start: product invalid");

    esp_err_t ret;

    _lora_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(PIN_DEVICE_UART_TX, PIN_DEVICE_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_MIN, UART_TX_BUF_SIZE_DEFAULT), __tag_device_e22900t22, "start: uart start");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_NORMAL), lora_start_failed, __tag_device_e22900t22, "start: mode normal");

    ESP_LOGI(__tag_device_e22900t22, "started");

    return ESP_OK;

lora_start_failed:
    (void)_lora_mode_set(LORA_MODE_NORMAL);
    hw_uart_stop();
    _lora_pins_disable();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_read_channel_rssi(int *const rssi_dbm) {

    const uint8_t cmd[] = { 0xC0, 0xC1, 0xC2, 0xC3, 0x00, 0x01 };
    uint8_t res[4];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, sizeof(res));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "channel rssi: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1 && res[1] == 0x00 && res[2] == 0x01, ESP_FAIL, __tag_device_e22900t22, "channel rssi: bad header %02" PRIX8 " %02" PRIX8 " %02" PRIX8, res[0], res[1], res[2]);

    *rssi_dbm = _LORA_E22_RSSI_DBM(res[3]);
    ESP_LOGI(__tag_device_e22900t22, "channel rssi: %d dBm", *rssi_dbm);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_RX_GAP_MS 20 /* inter-byte idle that delimits a received frame (>> one byte-time at 9600 baud) */

esp_err_t lora_read(uint8_t *const buf, const size_t max, int *const out_len, int *const out_rssi_dbm, const int first_byte_timeout_ms) {

    ESP_RETURN_ON_FALSE(buf != NULL && out_len != NULL && max > 0, ESP_ERR_INVALID_ARG, __tag_device_e22900t22, "read: bad args");

    *out_len = 0;
    if (out_rssi_dbm != NULL)
        *out_rssi_dbm = 0;

    if (hw_uart_read(buf, 1, first_byte_timeout_ms) <= 0)
        return ESP_OK;
    size_t total = 1;
    while (total < max && hw_uart_read(buf + total, 1, _LORA_RX_GAP_MS) > 0)
        total++;
    if (total >= 1) {
        if (out_rssi_dbm != NULL)
            *out_rssi_dbm = _LORA_E22_RSSI_DBM(buf[total - 1]);
        total -= 1;
    }
    *out_len = (int)total;

    if (*out_len > 0) {
        ESP_LOGD(__tag_device_e22900t22, "read: %d bytes, rssi=%d dBm", *out_len, out_rssi_dbm != NULL ? *out_rssi_dbm : 0);
        d_bytes_hex_log(__tag_device_e22900t22, buf, *out_len);
    }

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_write(const uint8_t *const data, const size_t len) {

    ESP_RETURN_ON_FALSE(len > 0 && len <= LORA_PACKET_SIZE_MAX, ESP_ERR_INVALID_SIZE, __tag_device_e22900t22, "write: invalid length %d (< 0 || > %d)", (int)len, (int)LORA_PACKET_SIZE_MAX);

    // Wait for AUX ready before transmit
    ESP_RETURN_ON_FALSE(lora_wait_ready(_LORA_TRANSMIT_WAIT_MS), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "write: wait ready (%d)", _LORA_TRANSMIT_WAIT_MS);
    ESP_RETURN_ON_FALSE(hw_uart_write(data, len) == (int)len, ESP_FAIL, __tag_device_e22900t22, "write: uart write (len=%d)", (int)len);
    // Block until the UART has physically clocked every byte out to the module
    ESP_RETURN_ON_ERROR(hw_uart_wait_tx_done(_LORA_CMD_TIMEOUT_MS), __tag_device_e22900t22, "write: tx drain");

    ESP_LOGD(__tag_device_e22900t22, "write: sent %d bytes", (int)len);
    d_bytes_hex_log(__tag_device_e22900t22, data, (int)len);

    return ESP_OK;
}

esp_err_t lora_write_complete(const uint8_t *const data, const size_t len, const bool wait_complete) {
    ESP_RETURN_ON_ERROR(lora_write(data, len), __tag_device_e22900t22, "write_complete: write");
    if (wait_complete)
        ESP_RETURN_ON_FALSE(lora_wait_ready(_LORA_TRANSMIT_DELAY_MS), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "write_complete: wait ready (%d)", _LORA_TRANSMIT_DELAY_MS);
    return ESP_OK;
}

esp_err_t lora_wait_complete(void) {
    ESP_RETURN_ON_FALSE(lora_wait_ready(_LORA_TRANSMIT_DELAY_MS), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "wait_complete: wait ready (%d)", _LORA_TRANSMIT_DELAY_MS);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_stop(void) {

    if (lora_sleep() == ESP_OK)
        if (_LORA_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_LORA_SLEEP_DELAY_MS);
    hw_uart_stop();
    _lora_pins_disable();

    ESP_LOGI(__tag_device_e22900t22, "stopped");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_test(device_test_result_t *const result, const uint32_t duration_ms) {

    result->passed = false;
    const uint32_t sleeping_ms = 1 * 1000;
    const __ticks_t start_ms = __ticks_ms();
    const uint32_t s = esp_random();

    esp_err_t rc;

    if ((rc = lora_setup(&lora_config_default)) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "setup failed: %s", esp_err_to_name(rc));
        return rc;
    }

    if ((rc = lora_start()) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "start failed: %s", esp_err_to_name(rc));
        return rc;
    }

#define _TEST_IOTDATA_VARIANT    0x04 // 4 bits
#define _TEST_IOTDATA_STATION_ID 3210 // 12 bits
#define _TEST_IOTDATA_PRESENCE   0x00 // 8 bits

    int failures = 5, iterations = 0;
    uint16_t sequence = (uint16_t)(s & 0xFFFF);
    while (rc == ESP_OK && (__ticks_ms() - start_ms) < duration_ms) {
        if (iterations++ & 1) {
            // rssi
            int rssi_dbm;
            if ((rc = lora_read_channel_rssi(&rssi_dbm)) != ESP_OK && --failures > 0)
                rc = ESP_OK;
        } else {
            // data
            if (rc == ESP_OK && (__ticks_ms() - start_ms) < duration_ms) {
                const uint8_t packet[5] = {
                    (uint8_t)(((_TEST_IOTDATA_VARIANT << 4) & 0xF0) | ((_TEST_IOTDATA_STATION_ID >> 8) & 0x0F)),
                    (uint8_t)((_TEST_IOTDATA_STATION_ID >> 0) & 0xFF),
                    (uint8_t)((sequence >> 8) & 0xFF),
                    (uint8_t)((sequence >> 0) & 0xFF),
                    _TEST_IOTDATA_PRESENCE,
                };
                if ((rc = lora_write(packet, sizeof(packet))) != ESP_OK && --failures > 0)
                    rc = ESP_OK;
                else
                    ESP_LOGD(__tag_device_e22900t22, "transmit (%d bytes) [%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 "]", (int)sizeof(packet), packet[0], packet[1], packet[2], packet[3], packet[4]);
                sequence++;
            }
        }
        if (rc == ESP_OK && (__ticks_ms() - start_ms) < duration_ms)
            hw_delay_ms_yieldable((uint32_t)sleeping_ms);
    }

    esp_err_t rc2;
    if ((rc2 = lora_stop()) != ESP_OK && rc == ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "stop failed: %s", esp_err_to_name(rc));
        return rc2;
    }

    if (rc != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "rssi read failed: %s", esp_err_to_name(rc));
        return rc;
    }

    result->passed = true;
    snprintf(result->detail, sizeof(result->detail), "E22 name=0x%04" PRIX16 ", ver=%d, pwr=%ddBm", (uint16_t)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3]);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
