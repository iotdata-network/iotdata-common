
#ifndef IOTDATA_NODE_UTILS_H
#define IOTDATA_NODE_UTILS_H

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_ESP32
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------------------------------------------------------
// The node's own hardware identity, and the station id derived from it.
// ---------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_ESP32

static inline bool _iotdata_node_sysfs(const char *const path, char *const out, const size_t size) {
    FILE *const f = fopen(path, "re");
    if (f == NULL)
        return false;
    const bool ok = (fgets(out, (int)size, f) != NULL);
    (void)fclose(f);
    if (!ok)
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

static inline uint32_t _iotdata_node_hash32(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s != '\0'; s++) {
        h ^= (uint32_t)(unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

#endif

static inline bool iotdata_node_mac(uint8_t mac[6]) {
#ifdef PLATFORM_ESP32
    memset(mac, 0, 6);
    return esp_efuse_mac_get_default(mac) == ESP_OK;
#else
    /*
     * On linux, the lowest-named interface that is both ethernet (type 1) and carries a PERMANENT
     * address (addr_assign_type 0). Both filters matter: the permanence test is what keeps a docker
     * bridge, a veth or a bond -- whose addresses are generated, and change with the day's containers
     * -- from becoming the node's identity, and lowest-name makes the choice the same on every boot
     * regardless of the order the kernel happened to probe them in.
     */
    char best[256] = { 0 }, path[320], buf[64];
    DIR *const dir = opendir("/sys/class/net");
    if (dir != NULL) {
        const struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0)
                continue;
            if (_iotdata_node_sysfs(snprintf_inline(path, sizeof(path), "/sys/class/net/%s/type", ent->d_name), buf, sizeof(buf)) || atoi(buf) != 1)                 /* ARPHRD_ETHER */
                if (_iotdata_node_sysfs(snprintf_inline(path, sizeof(path), "/sys/class/net/%s/addr_assign_type", ent->d_name), buf, sizeof(buf)) || atoi(buf) != 0) /* NET_ADDR_PERM */
                    if (best[0] == '\0' || strcmp(ent->d_name, best) < 0)
                        snprintf(best, sizeof(best), "%s", ent->d_name);
        }
        (void)closedir(dir);
    }
    if (best[0] != '\0') {
        unsigned int v[6];
        if (_iotdata_node_sysfs(snprintf_inline(path, sizeof(path), "/sys/class/net/%s/address", best), buf, sizeof(buf)) && sscanf(buf, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            for (int i = 0; i < 6; i++)
                mac[i] = (uint8_t)v[i];
            return true;
        }
    }
    memset(mac, 0, 6);
    return false;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

static inline uint32_t iotdata_node_mac32(void) {
    uint8_t mac[6];
    return iotdata_node_mac(mac) ? ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5] : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

/*
 * The station id this board defaults to, always assignable: 1..4094, never 0, never broadcast.
 *
 * A DEFAULT, not a decision -- SETTINGS STATION overrides it and persists, and anything that has
 * to be addressable across a re-image should carry one rather than rely on this. The hostname
 * fallback exists so that a host with no permanent NIC still comes up with a legal station rather
 * than with 0, which is not a station at all; it is the weakest of the three, because renaming the
 * host moves the identity.
 */
static inline uint16_t iotdata_node_station_from_mac(const char *const tag) {
    uint8_t mac[6];
    if (iotdata_node_mac(mac)) {
        const uint16_t station_id = iotdata_station_from_id(((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5]);
#ifdef PLATFORM_ESP32
        ESP_LOGI(tag, "board: mac=%02X:%02X:%02X:%02X:%02X:%02X station=%03" PRIX16, (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2], (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5], station_id);
#else
        fprintf(stderr, "%s: board: mac=%02X:%02X:%02X:%02X:%02X:%02X station=%03" PRIX16 "\n", tag, (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2], (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5], station_id);
#endif
        return station_id;
    }
#ifdef PLATFORM_ESP32
    ESP_LOGW(tag, "board: no mac, station=001");
    return 1u;
#else
    char host[256];
    if (gethostname(host, sizeof(host)) != 0)
        host[0] = '\0';
    host[sizeof(host) - 1] = '\0';
    const uint16_t station_id = iotdata_station_from_id(_iotdata_node_hash32(host));
    fprintf(stderr, "%s: board: no permanent mac, station=%" PRIu16 " from hostname '%s'\n", tag, station_id, host);
    return station_id;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint16_t key;  /* what the list is ordered by -- a station id */
    uint16_t slot; /* the table slot that entry lives in */
} iotdata_node_order_t;

static inline int iotdata_node_order_insert(iotdata_node_order_t *const ord, const int n, const int max, const uint16_t key, const uint16_t slot) {
    if (ord == NULL || n >= max)
        return n;
    int pos = n;
    while (pos > 0 && ord[pos - 1].key > key) {
        ord[pos] = ord[pos - 1];
        pos--;
    }
    ord[pos].key = key;
    ord[pos].slot = slot;
    return n + 1;
}

// ---------------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_UTILS_H */
