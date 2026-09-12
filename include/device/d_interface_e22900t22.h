
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
    bool host_sleeps;
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
    .host_sleeps = true, /* the battery case is the default: an always-on host opts out */
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
/*
 * How long to wait for AUX to say the module has drained -- see _lora_transmit_wait_ms(). These
 * bound a value derived from the configured air rate rather than replacing it.
 */
#define _LORA_TRANSMIT_WAIT_MIN_MS         1000  /* the fast rates make the arithmetic tiny */
#define _LORA_TRANSMIT_WAIT_MAX_MS         20000 /* beyond this the module is not merely busy */
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

#define _LORA_E22_REG1_OFFSET              4 /* ADDH ADDL NETID REG0 [REG1] CH REG3 ... */

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

/*
 * The AUX wait has to be DERIVED, not fixed.
 *
 * AUX stays low while the module still holds anything, so the worst case is the air time of what
 * it is holding -- and air time scales with the configured rate. A full 240-byte packet is 800ms
 * at 2.4kbps and 6.4 SECONDS at 0.3kbps, so no single constant serves both: the 1000ms this
 * replaces was nearly right at 2.4kbps and wrong by more than 6x below it.
 *
 * Budget: the air time of a full packet, doubled. The doubling covers the two things that make a
 * write wait longer than its own frame -- one frame already queued ahead of it, and
 * listen-before-transmit deferring the start while the channel is busy. Both happen together on a
 * mesh under load, which is where the fixed value failed: a relay's forward retries filled the
 * module and the write gave up after a second, reporting an error for ordinary back-pressure.
 *
 * Only DIP modules reach this: lora_is_ready() is unconditionally true on USB, which has no AUX.
 */
static inline uint32_t _lora_transmit_wait_ms(void) {
    const uint16_t bps = _LORA_CONFIG(air_data_rate);
    if (bps == 0)
        return _LORA_TRANSMIT_WAIT_MIN_MS;
    const uint32_t air_ms = ((uint32_t)LORA_PACKET_SIZE_MAX * 8u * 1000u) / (uint32_t)bps;
    const uint32_t budget = air_ms * 2u;
    if (budget < _LORA_TRANSMIT_WAIT_MIN_MS)
        return _LORA_TRANSMIT_WAIT_MIN_MS;
    return budget > _LORA_TRANSMIT_WAIT_MAX_MS ? _LORA_TRANSMIT_WAIT_MAX_MS : budget;
}

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

/*
 * TRANSMIT BACK-PRESSURE, and the USB module has none of its own.
 *
 * On DIP, AUX is the module telling us its buffer has room, and lora_write() waits on it. USB
 * exposes no AUX at all, so this answered `true` unconditionally -- meaning lora_write()'s
 * "wait for ready" was a no-op and the host could hand the module a second frame while it was
 * still radiating the first. With listen-before-transmit on, how long that takes is not even
 * predictable: the module waits for a clear channel first.
 *
 * What that cost, observed live on a gateway: three phantom stations in two logs, each one the
 * gateway's OWN BEACON transmitted with its leading byte missing. `F0 01 11 2F 00 01 00 12 64`
 * went out as `01 11 2F 00 01 00 12 64`, which is a syntactically perfect header for a station
 * that does not exist, so every relay that heard it invented a neighbour and forwarded it. Each
 * one was a beacon whose mesh sequence was exactly ACK+1, sent ~220ms behind that ACK in the same
 * loop pass, where ack-to-ack spacing of ~390ms never failed. The module drops the leading byte
 * of a frame written while it is busy.
 *
 * Time is the only substitute for AUX here, so the guard is a minimum interval between writes.
 * It is deliberately a crude floor rather than a computed air time: with LBT the module's busy
 * period is not a function of frame length, so a formula would be a guess dressed up as physics.
 * 400ms is the spacing that was never observed to fail; tune it against a log, not against the
 * bit rate.
 */
#ifndef _LORA_TX_GUARD_USB_MS
#define _LORA_TX_GUARD_USB_MS 400
#endif

static uint32_t _lora_tx_guard_until_ms = 0;

bool lora_is_ready(void) {
    if (!_LORA_IS_USB())
        return hw_gpio_get(PIN_DEVICE_LORA_AUX);
    /* wrap-safe, so this still orders correctly across the millisecond clock's rollover */
    return (int32_t)((uint32_t)__ticks_ms() - _lora_tx_guard_until_ms) >= 0;
}

static inline void _lora_settle(void) {
    if (_LORA_IS_USB())
        hw_delay_ms_yieldable(_LORA_USB_SETTLE_MS);
}

D_WAIT_READY_FUNC(lora_wait_ready, lora_is_ready, _LORA_READY_DELAY_MS, true, __tag_device_e22900t22)

// ------------------------------------------------------------------------------------------------------------------------

static bool _lora_cmd_send(const uint8_t *const cmd, const size_t cmd_len) {
    _lora_settle();
    if (!lora_wait_ready(_lora_transmit_wait_ms()))
        return false;
    if (hw_uart_flush() != ESP_OK)
        return false;
    if (hw_uart_write(cmd, cmd_len) != (int)cmd_len)
        return false;
    _lora_settle();
    return true;
}

static int _lora_cmd_xfer_to(const uint8_t *const cmd, const size_t cmd_len, uint8_t *const res, const size_t res_len, const int timeout_ms) {
    if (!_lora_cmd_send(cmd, cmd_len))
        return -1;
    if (!lora_wait_ready(_lora_transmit_wait_ms()))
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
    if (mode == LORA_MODE_DEEP_SLEEP) {
        ESP_RETURN_ON_FALSE(_lora_cmd_send(cmd, sizeof(cmd)), ESP_FAIL, __tag_device_e22900t22, "mode set (usb): sleep command not written");
    } else {
        uint8_t res[sizeof(cmd)];
        const int want = (int)sizeof(cmd) - 1;
        const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, (size_t)want);
        if (n == 3 && res[0] == 0xFF && res[1] == 0xFF && res[2] == 0xFF) {
            /*
             * WARN, not DEBUG, and read the message carefully before believing it.
             *
             * FF FF FF is documented nowhere. The reference implementation calls it "already appears to
             * be in required mode, will accept" and returns success -- and that reading cost real
             * debugging time, because it is at least equally consistent with a plain NACK. Observed
             * with REG1 bit 2 (switch-config-serial) CLEAR: every mode command answers FF FF FF while
             * the module stays put, config reads keep working, and data writes come back as FF FF FF
             * too. Nothing looks broken; nothing works.
             *
             * It is NOT on its own evidence that bit 2 is clear -- a deep-sleep command answers this
             * way with bit 2 perfectly set, which is why that mode no longer reaches here at all.
             *
             * So it is accepted as success (the module may genuinely be in the mode we asked for) but
             * it is now audible. If you see it for NORMAL or CONFIG, read the REG1 value the setup
             * logs and check bit 2 rather than guessing.
             */
            ESP_LOGW(__tag_device_e22900t22, "mode set (usb): mode %d not confirmed (FF FF FF) -- accepted, but check the REG1 value if this repeats", (int)mode);
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(n >= want, ESP_FAIL, __tag_device_e22900t22, "mode set (usb): got %d bytes, expected %d", n, want);
    }
    return ESP_OK;
}

/* Only meaningful when the host does not sleep: then setup leaves the link up and start has
   nothing to do. A sleeping host loses statics anyway, which is what _lora_rtc is for. */
static bool s_lora_link_up = false;

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
        uint8_t now_config[_LORA_E22_REG_SIZE_CONFIG_READ];
        if (_lora_cmd_read_config(now_config) != ESP_OK)
            ESP_LOGW(__tag_device_e22900t22, "config verify: read back failed -- cannot confirm the write took");
        else if (memcmp(now_config, req_config, _LORA_E22_REG_SIZE_CONFIG_WRITE) != 0) {
            char want[sizeof(req_config) * 3], got[sizeof(now_config) * 3];
            ESP_LOGE(__tag_device_e22900t22, "config verify: read back failed -- wanted %s, module reports %s", d_bytes_hex_str(want, sizeof(want), req_config, (int)sizeof(req_config), ":"),
                     d_bytes_hex_str(got, sizeof(got), now_config, _LORA_E22_REG_SIZE_CONFIG_WRITE, ":"));
        }
    }

    if (_LORA_IS_USB()) {
        uint8_t reg_config[_LORA_E22_REG_SIZE_CONFIG_READ];
        if (_lora_cmd_read_config(reg_config) == ESP_OK && (reg_config[_LORA_E22_REG1_OFFSET] & 0x04) == 0)
            ESP_LOGE(__tag_device_e22900t22, "REG1 bit 2 (switch config by serial) is CLEAR (REG1=0x%02" PRIX8 ") -- mode commands will be PUT ON AIR instead of obeyed; use the module's mode button to reach config mode and rewrite it",
                     reg_config[_LORA_E22_REG1_OFFSET]);
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

    if (!_LORA_CONFIG(host_sleeps)) {
        ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_NORMAL), lora_setup_exit, __tag_device_e22900t22, "setup: mode normal");
        s_lora_link_up = true;
    } else {
        ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_DEEP_SLEEP), lora_setup_exit, __tag_device_e22900t22, "setup: mode deep sleep");
        hw_uart_stop();
        _lora_pins_disable();
    }

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

    if (s_lora_link_up) {
        ESP_LOGI(__tag_device_e22900t22, "started (link already up)");
        return ESP_OK; /* setup left it in NORMAL with the UART open: there is nothing to do */
    }

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

/*
 * The inter-byte idle that delimits a received frame -- and it has to be told about the link, not
 * about the baud rate.
 *
 * On a DIP module the UART is wired straight to the SoC: bytes arrive one byte-time apart (1.04ms
 * at 9600) and any real gap is a frame boundary, so 20ms is already generous.
 *
 * It was briefly raised to 50ms on the theory that a DIP module was splitting frames mid-packet,
 * after a relay invented stations out of beacons that arrived a byte short. That theory was
 * WRONG and the history is worth keeping, because the symptom looks identical either way: the
 * byte was never lost on this side at all. It was dropped by the USB module at the OTHER end,
 * which had been handed a frame while still radiating the previous one -- see
 * _LORA_TX_GUARD_USB_MS. The proof was _LORA_RX_FRAME_MIN below: a read-side split now reports a
 * runt and logs its cause, and across two hours of the 50ms build not one fired while the
 * phantoms kept coming. So 20ms stands, and there is no evidence a DIP module has ever split a
 * frame.
 *
 * If one ever does, the fragment warning will say so and 50ms is the answer -- measured rather
 * than guessed: across 1188 receives on that link the shortest gap between two frames was 180ms,
 * with nothing below 100ms, so there is room to raise this without merging two frames into one.
 * Keep DIP below USB rather than collapsing them, though: that 180ms floor is a property of one
 * network's beacon and ack cadence at 2.4kbps, not a guarantee.
 *
 * Behind a USB-serial dongle the host schedules URBs, and a frame longer than one bulk transfer
 * arrives as two, tens of milliseconds apart, with nothing wrong. At 20ms that reads as a frame
 * boundary and a long frame is torn in half: observed on a 56-byte node VERSION report, split into
 * a 31-byte piece that decoded as far as its header and a 28-byte tail that looked like a fresh
 * packet from a nonsense station. The reference implementation allowed 100ms per byte after the
 * first, which is why it never showed this; that is the number to match.
 *
 * The cost of a longer gap is twofold. Two frames genuinely arriving back-to-back within it are
 * read as one; that merged frame then fails to decode -- and since a frame that does not decode no
 * longer claims a dedup slot, it is dropped and the relayed copy still gets through. And every
 * read waits the gap out after its last byte, so a received frame costs the gap in loop time --
 * which is the other reason not to raise this without evidence: it is paid on every frame, by a
 * loop that has to keep reading.
 */
#define _LORA_RX_GAP_DIP_MS 20
#define _LORA_RX_GAP_USB_MS 100

/* The shortest thing that could be a frame: an iotdata/mesh header is 4 bytes (variant+station,
   sequence). A read shorter than that PLUS the appended RSSI byte did not catch a whole frame, it
   caught a fragment -- and a fragment's last byte is DATA, not RSSI. Treating it as RSSI is how a
   1-byte fragment becomes `len = 0`, which the caller cannot tell from "nothing arrived", while
   the rest of the packet is then read as a frame in its own right, one byte short at the front.
   Observed live: a gateway beacon `F0 01 0B DD 00 01 00 11 A2` split after its first byte was
   read back as `01 0B DD 00 01 00 11 A2` -- a syntactically perfect header for a station that
   does not exist, which nothing downstream can reject. */
#define _LORA_RX_FRAME_MIN  4
#define _LORA_RX_GAP_MS     (_LORA_IS_USB() ? _LORA_RX_GAP_USB_MS : _LORA_RX_GAP_DIP_MS)

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
    if (total >= (size_t)_LORA_RX_FRAME_MIN + 1) {
        if (out_rssi_dbm != NULL)
            *out_rssi_dbm = _LORA_E22_RSSI_DBM(buf[total - 1]);
        total -= 1;
    } else {
        /* A fragment. Hand back every byte read, with no RSSI claimed, so the caller counts it as
           the runt it is rather than being told nothing happened. The stream is already
           desynchronised at this point and these bytes cannot be un-read -- what this buys is
           that the NEXT frame's bogus header has a logged cause sitting immediately before it. */
        ESP_LOGW(__tag_device_e22900t22, "read: %u byte fragment, not a frame -- a packet split across the %dms gap; the next read will start mid-packet", (unsigned)total, _LORA_RX_GAP_MS);
        d_bytes_hex_log(__tag_device_e22900t22, buf, (int)total);
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
    ESP_RETURN_ON_FALSE(lora_wait_ready(_lora_transmit_wait_ms()), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "write: wait ready (%" PRIu32 "ms at %ubps -- module still busy)", _lora_transmit_wait_ms(), (unsigned)_LORA_CONFIG(air_data_rate));
    ESP_RETURN_ON_FALSE(hw_uart_write(data, len) == (int)len, ESP_FAIL, __tag_device_e22900t22, "write: uart write (len=%d)", (int)len);
    // Block until the UART has physically clocked every byte out to the module
    ESP_RETURN_ON_ERROR(hw_uart_wait_tx_done(_LORA_CMD_TIMEOUT_MS), __tag_device_e22900t22, "write: tx drain");

    /* The module is now busy. On DIP, AUX will say so; on USB nothing will, so hold the next
       write off for the guard interval -- this is the only thing standing between a back-to-back
       pair and a frame going out a byte short. */
    if (_LORA_IS_USB())
        _lora_tx_guard_until_ms = (uint32_t)__ticks_ms() + _LORA_TX_GUARD_USB_MS;

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

    if (_LORA_CONFIG(host_sleeps) && lora_sleep() == ESP_OK)
        if (_LORA_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_LORA_SLEEP_DELAY_MS);
    s_lora_link_up = false;
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
