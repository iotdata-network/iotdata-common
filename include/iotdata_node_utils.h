
#ifndef IOTDATA_NODE_UTILS_H
#define IOTDATA_NODE_UTILS_H

static inline uint32_t iotdata_node_mac32(void) {
#ifdef PLATFORM_ESP32
    uint8_t mac[6] = { 0 };
    (void)esp_efuse_mac_get_default(mac);
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
#else
    return 0;
#endif
}

static inline uint16_t iotdata_node_station_from_mac(const char *const tag) {
#ifdef PLATFORM_ESP32
    uint8_t mac[6] = { 0 };
    (void)esp_efuse_mac_get_default(mac);
    const uint16_t station_id = iotdata_station_from_id(iotdata_node_mac32()); /* 1..4094: never 0, never broadcast */
    ESP_LOGI(tag, "board: mac=%02X:%02X:%02X:%02X:%02X:%02X station=%" PRIu16, (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2], (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5], station_id);
    return station_id;
#else
    (void)tag;
    return 0; // XXX
#endif
}

static inline uint8_t iotdata_node_reason_reset(void) {
#ifdef PLATFORM_ESP32
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return IOTDATA_NODE_REASON_POWER_ON;
    case ESP_RST_SW:
        return IOTDATA_NODE_REASON_SOFTWARE;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return IOTDATA_NODE_REASON_WATCHDOG;
    case ESP_RST_BROWNOUT:
        return IOTDATA_NODE_REASON_BROWNOUT;
    case ESP_RST_PANIC:
        return IOTDATA_NODE_REASON_PANIC;
    case ESP_RST_DEEPSLEEP:
        return IOTDATA_NODE_REASON_DEEPSLEEP;
    case ESP_RST_EXT:
        return IOTDATA_NODE_REASON_EXTERNAL;
    default:
        return IOTDATA_NODE_REASON_UNKNOWN;
    }
#else
    return IOTDATA_NODE_REASON_UNKNOWN;
#endif
}

#endif /* IOTDATA_NODE_UTILS_H */
