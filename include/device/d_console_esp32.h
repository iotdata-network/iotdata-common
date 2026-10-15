#ifndef D_CONSOLE_ESP32_H
#define D_CONSOLE_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_console_esp32.h -- the console line on an esp32: USB-serial-JTAG, the same one the app logs on.
//
// See d_console.h for what this answers. The receive ring is the interesting part and the comment
// on console_line_begin() is the measurement behind its size.
//
// ESP-IDF COMPONENTS THIS NEEDS, in the project's main CMakeLists.txt:
//
//     PRIV_REQUIRES ... esp_driver_usb_serial_jtag vfs esp_app_format
//
//   esp_driver_usb_serial_jtag  the driver and its vfs binding -> the line itself
//   vfs                         so stdio and the driver agree about the same device
//   esp_app_format              esp_app_desc.h -> the app's own name and version in `vers`
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

#include "d_console.h"

#define D_CONSOLE_PROVIDED 1

// ------------------------------------------------------------------------------------------------------------------------

/*
 * THE RECEIVE RING IS NOT FLOW CONTROLLED, AND THIS WAS MEASURED NOT ASSUMED. USB-serial-JTAG looks
 * as though it should NAK a host that is ahead of the device, and it does not: what will not fit is
 * dropped. At 256 bytes a single 261-byte line arrived short every time; at 1024 a burst of ten
 * such lines landed as four -- with logging off, so this is the receive path and not a device
 * blocked in printf. The symptom is a base64 line whose length is no longer a multiple of four,
 * which the decoder refuses: the transport lost the bytes and the layers above merely noticed.
 *
 * So the size is a real contract, not a buffer guess, which is why it arrives as an argument from
 * the layer that depends on it rather than being chosen here.
 */
static inline bool console_line_begin(const unsigned rx_bytes, const unsigned tx_bytes) {
    if (usb_serial_jtag_driver_install(&(usb_serial_jtag_driver_config_t){ .tx_buffer_size = (size_t)tx_bytes, .rx_buffer_size = (size_t)rx_bytes }) != ESP_OK)
        return false;
    usb_serial_jtag_vfs_use_driver(); /* route stdio through the driver so reads + printf agree */
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline size_t console_line_read(uint8_t *const buf, const size_t max) {
    const int n = usb_serial_jtag_read_bytes(buf, max, 0); /* timeout 0 -> drain the FIFO */
    return (n > 0) ? (size_t)n : 0u;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline uint32_t console_stamp_ms(void) {
    return esp_log_timestamp();
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool console_heap(uint32_t *const free_bytes, uint32_t *const min_bytes, uint32_t *const largest_bytes) {
    if (free_bytes != NULL)
        *free_bytes = (uint32_t)esp_get_free_heap_size();
    if (min_bytes != NULL)
        *min_bytes = (uint32_t)esp_get_minimum_free_heap_size();
    if (largest_bytes != NULL)
        *largest_bytes = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const char *console_platform_str(char *const buf, const size_t size) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    (void)snprintf(buf, size, "%s chip-rev=%d cores=%d idf=%s", CONFIG_IDF_TARGET, (int)chip.revision, (int)chip.cores, esp_get_idf_version());
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void console_restart(void) {
    fflush(stdout);
    esp_rom_delay_us(50000);
    esp_restart();
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_CONSOLE_ESP32_H */
