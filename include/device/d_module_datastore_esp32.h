#ifndef D_MODULE_DATASTORE_ESP32_H
#define D_MODULE_DATASTORE_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_datastore_esp32.h - a keyed blob store in NVS. The esp32 half of the seam
// d_module_datastore_linux.h fills with a directory of files.
//
// One namespace, one blob per key, values opaque. It is the BOTTOM layer: it knows nothing about
// what it holds, only how to get bytes onto something that survives a restart and back again.
//
// WRITES ARE ATOMIC BY CONSTRUCTION here, which is the one place this half is simpler than the
// other: nvs_set_blob followed by nvs_commit either lands whole or not at all, so there is no
// temporary file and no directory to flush.
//
// WEAR IS THE CONSTRAINT INSTEAD. A flash sector takes on the order of 10^5 erases, so a caller
// that persists on every change of a per-packet counter will wear a partition out in the field.
// That is why iotdata_node_state.h reserves ahead for sequence numbers rather than writing each
// one: one commit per N packets, and N is free to be large because skipping forward is harmless.
//
// datastore_persistent() is unconditionally true. NVS is flash; the tmpfs trap the linux half
// guards against does not exist here.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "nvs.h"
#include "nvs_flash.h"
#pragma GCC diagnostic pop

const char *__tag_device_datastore = "device-datastore";

#ifndef DATASTORE_KEY_MAX
#define DATASTORE_KEY_MAX NVS_KEY_NAME_MAX_SIZE /* 16, NUL included: NVS's own limit */
#endif
#ifndef DATASTORE_BLOB_MAX
#define DATASTORE_BLOB_MAX 8192
#endif

typedef struct {
    nvs_handle_t handle;
    bool open;
    uint32_t stat_read, stat_write, stat_read_fail, stat_write_fail;
} datastore_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline bool datastore_persistent(const datastore_t *const ds) {
    return ds != NULL && ds->open; /* flash: if it opened, it persists */
}

/* `location` is the NVS namespace. Brings the partition up if the application has not: nvs_flash_init
   is idempotent, and a store that refused to work unless someone else had initialised NVS first
   would be a trap for the one application that has no other reason to. */
static inline bool datastore_open(datastore_t *const ds, const char *const location) {
    if (ds == NULL || location == NULL)
        return false;
    *ds = (datastore_t){ 0 };
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_datastore, "open: nvs_flash_init: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = nvs_open(location, NVS_READWRITE, &ds->handle)) != ESP_OK) {
        ESP_LOGE(__tag_device_datastore, "open: nvs_open(%s): %s", location, esp_err_to_name(err));
        return false;
    }
    ds->open = true;
    return true;
}

static inline void datastore_close(datastore_t *const ds) {
    if (ds != NULL && ds->open) {
        nvs_close(ds->handle);
        ds->open = false;
    }
}

static inline bool datastore_read(datastore_t *const ds, const char *const key, void *const buf, const size_t size, size_t *const out_len) {
    if (ds == NULL || !ds->open || buf == NULL || key == NULL)
        return false;
    size_t len = size;
    const esp_err_t err = nvs_get_blob(ds->handle, key, buf, &len);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) /* absent is an answer, not a failure */
            ds->stat_read_fail++;
        return false;
    }
    ds->stat_read++;
    if (out_len != NULL)
        *out_len = len;
    return true;
}

static inline bool datastore_write(datastore_t *const ds, const char *const key, const void *const data, const size_t len) {
    if (ds == NULL || !ds->open || data == NULL || key == NULL || len > DATASTORE_BLOB_MAX)
        return false;
    esp_err_t err = nvs_set_blob(ds->handle, key, data, len);
    if (err == ESP_OK)
        err = nvs_commit(ds->handle); /* the commit is what makes it atomic */
    if (err != ESP_OK) {
        ESP_LOGW(__tag_device_datastore, "write(%s): %s", key, esp_err_to_name(err));
        ds->stat_write_fail++;
        return false;
    }
    ds->stat_write++;
    return true;
}

static inline bool datastore_erase(datastore_t *const ds, const char *const key) {
    if (ds == NULL || !ds->open || key == NULL)
        return false;
    const esp_err_t err = nvs_erase_key(ds->handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return false;
    return nvs_commit(ds->handle) == ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_DATASTORE_ESP32_H */
