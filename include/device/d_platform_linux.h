
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_platform_linux.h - enough of ESP-IDF, on Linux, to run the d_interface_* drivers against real
// hardware attached to a host.
//
// WHY. The device drivers in this directory are written against ESP-IDF: esp_err_t, ESP_LOG*, the
// ESP_RETURN_ON_* macros, and the hw_uart_* / hw_gpio_* abstraction. That is the right shape for
// the boards, but the Linux gateway needs the same E22 driver, and duplicating it was how the
// iotdata-depend/e22900t22 fork came to exist in the first place. So rather than a second driver,
// this supplies the platform underneath the one driver.
//
// RELATIONSHIP TO THE EMULATOR. iotdata-device/emu/app_emu_espidf_esp32c3.c also implements this
// surface under the same EMU_LINUX define, but backs the peripherals with SIMULATORS -- it exists
// to host-test a whole device app. This header instead backs the UART with a real serial port.
// They are alternatives; include exactly one.
//
// USAGE. Define EMU_LINUX (which makes the d_* headers skip their ESP-IDF includes), then:
//
//     #define EMU_LINUX
//     #include "d_platform_linux.h"       // must be FIRST: d_common.h uses what it defines
//     #include "d_common.h"
//     #include "d_interface_e22900t22.h"
//
// Do NOT include d_hardware_uart.h or d_hardware_gpio.h on Linux -- their bodies are written in
// terms of the IDF's uart_*/gpio_* calls. This header provides hw_uart_* / hw_gpio_* directly,
// which is the layer the drivers actually use.
//
// SCOPE. Deliberately only what the E22 driver needs. It is not an ESP-IDF port and should not
// grow into one: if a driver needs more, the honest question is usually whether that driver
// belongs on a host at all.
//
// GPIO IS NOT IMPLEMENTED. hw_gpio_* are accepted and ignored, and a read returns "ready". That is
// sufficient for, and only for, the E22-900T22U (USB) module, which exposes no M0/M1/AUX and
// switches mode over the serial link -- see lora_module_t. A DIP module on a host would need real
// GPIO (libgpiod) and is not supported here.
//
// ------------------------------------------------------------------------------------------------------------------------

#ifndef D_PLATFORM_LINUX_H
#define D_PLATFORM_LINUX_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_LINUX
#define PLATFORM_LINUX
#endif

// ------------------------------------------------------------------------------------------------------------------------
// errors
// ------------------------------------------------------------------------------------------------------------------------

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_TIMEOUT       0x107

static inline const char *esp_err_to_name(const esp_err_t e) {
    switch (e) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        break;
    }
    /* DEV_ERR_* from d_common.h land here; the number is more use than a wrong name. */
    static char buf[24];
    snprintf(buf, sizeof(buf), "err:0x%X", (unsigned)e);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------
// logging
// ------------------------------------------------------------------------------------------------------------------------
//
// Routed through the host app's PRINTF_* if it has them (the gateway does, with timestamps and
// level tags), so driver output is not a second log format in the same file. Otherwise stderr.

#ifndef PRINTF_INFO
#define PRINTF_INFO(...) fprintf(stderr, __VA_ARGS__)
#endif
#ifndef PRINTF_WARN
#define PRINTF_WARN(...) fprintf(stderr, __VA_ARGS__)
#endif
#ifndef PRINTF_ERROR
#define PRINTF_ERROR(...) fprintf(stderr, __VA_ARGS__)
#endif
#ifndef PRINTF_DEBUG
#define PRINTF_DEBUG(...) \
    do { \
    } while (0)
#endif

#define ESP_LOGE(tag, fmt, ...) PRINTF_ERROR("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) PRINTF_WARN("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) PRINTF_INFO("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) PRINTF_DEBUG("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) PRINTF_DEBUG("%s: " fmt "\n", tag, ##__VA_ARGS__)

#define ESP_ERROR_CHECK(x) \
    do { \
        const esp_err_t __e = (x); \
        if (__e != ESP_OK) { \
            ESP_LOGE("CHECK", "%s(%d): 0x%X", __FILE__, __LINE__, (unsigned)__e); \
            abort(); \
        } \
    } while (0)

/* The IDF's esp_check.h macros. Same contract: log with context and return/goto on failure. */
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) \
    do { \
        const esp_err_t __e = (x); \
        if (__e != ESP_OK) { \
            ESP_LOGE(tag, fmt " [0x%X]", ##__VA_ARGS__, (unsigned)__e); \
            return __e; \
        } \
    } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) \
    do { \
        if (!(a)) { \
            ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
            return (err); \
        } \
    } while (0)
#define ESP_GOTO_ON_ERROR(x, label, tag, fmt, ...) \
    do { \
        const esp_err_t __e = (x); \
        if (__e != ESP_OK) { \
            ESP_LOGE(tag, fmt " [0x%X]", ##__VA_ARGS__, (unsigned)__e); \
            ret = __e; \
            goto label; \
        } \
    } while (0)
#define ESP_GOTO_ON_FALSE(a, err, label, tag, fmt, ...) \
    do { \
        if (!(a)) { \
            ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
            ret = (err); \
            goto label; \
        } \
    } while (0)

// ------------------------------------------------------------------------------------------------------------------------
// system
// ------------------------------------------------------------------------------------------------------------------------

/* Nothing survives a restart on a host the way RTC memory does on the chip, and nothing needs to:
   the gateway is a process, and a restarted process re-reads its config. Plain storage, and the
   magic stamp in _RTC_DATA_INIT then simply always initialises. */
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR

static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline uint32_t esp_random(void) {
    return (uint32_t)random();
}

/* A stand-in for the CPU cycle counter. __JITTER() samples it around a 1us delay to harvest
   entropy from clock wander; nanosecond wall time serves the same purpose on a host, where there
   is far more jitter available than on a microcontroller. */
static inline uint32_t esp_cpu_get_cycle_count(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_nsec;
}

static inline void ets_delay_us(const uint32_t us) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)us * 1000L };
    (void)nanosleep(&ts, NULL);
}

/* No watchdog to feed: on a host that is systemd's job, not ours. */
static inline int esp_task_wdt_reset(void) {
    return ESP_OK;
}

static inline void esp_rom_delay_us(const uint32_t us) {
    struct timespec ts = { .tv_sec = (time_t)(us / 1000000u), .tv_nsec = (long)(us % 1000000u) * 1000L };
    (void)nanosleep(&ts, NULL);
}

/* FreeRTOS delay, in the same units the callers use (pdMS_TO_TICKS is the identity here, so a
   "tick" is a millisecond). */
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(const uint32_t ms) {
    struct timespec ts = { .tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L };
    (void)nanosleep(&ts, NULL);
}

// ------------------------------------------------------------------------------------------------------------------------
// GPIO -- accepted and ignored; see the note at the top
// ------------------------------------------------------------------------------------------------------------------------

typedef int gpio_num_t;

#define GPIO_NUM_NC                  (-1)
#define GPIO_IS_VALID_GPIO(n)        ((n) >= 0)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) ((n) >= 0)
#define GPIO_IS_VALID_INPUT_GPIO(n)  ((n) >= 0)

static inline esp_err_t gpio_hold_en(const gpio_num_t pin) {
    (void)pin;
    return ESP_OK;
}
static inline esp_err_t gpio_hold_dis(const gpio_num_t pin) {
    (void)pin;
    return ESP_OK;
}
static inline void gpio_deep_sleep_hold_en(void) {
}
static inline void gpio_deep_sleep_hold_dis(void) {
}

static inline void hw_gpio_cfg_enable_input(const gpio_num_t pin, const bool pullup) {
    (void)pin;
    (void)pullup;
}
static inline void hw_gpio_cfg_enable_output(const gpio_num_t pin) {
    (void)pin;
}
static inline void hw_gpio_cfg_disable(const gpio_num_t pin) {
    (void)pin;
}
static inline void hw_gpio_cfg_disable_two(const gpio_num_t a, const gpio_num_t b) {
    (void)a;
    (void)b;
}
static inline void hw_gpio_cfg_disable_four(const gpio_num_t a, const gpio_num_t b, const gpio_num_t c, const gpio_num_t d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}
static inline void hw_gpio_set(const gpio_num_t pin, const bool level) {
    (void)pin;
    (void)level;
}
static inline bool hw_gpio_get(const gpio_num_t pin) {
    (void)pin;
    return true;
}
static inline void hw_gpio_revoke_two(const gpio_num_t a, const gpio_num_t b) {
    (void)a;
    (void)b;
}

// ------------------------------------------------------------------------------------------------------------------------
// UART -- a real serial port
// ------------------------------------------------------------------------------------------------------------------------
//
// The device path is not a pin number, so set it before the driver's setup call: hw_uart_set_device("/dev/e22900t22u");
//
// ------------------------------------------------------------------------------------------------------------------------

#ifndef HW_UART_DEVICE_DEFAULT
#define HW_UART_DEVICE_DEFAULT "/dev/e22900t22u"
#endif

#define UART_BAUD_DEFAULT        9600
#define UART_RX_BUF_SIZE_MIN     256
#define UART_RX_BUF_SIZE_DEFAULT 512
#define UART_TX_BUF_SIZE_MIN     256
#define UART_TX_BUF_SIZE_DEFAULT 256

static int s_hw_uart_fd = -1;
static const char *s_hw_uart_device = HW_UART_DEVICE_DEFAULT;

static inline void hw_uart_set_device(const char *const path) {
    if (path != NULL && *path != '\0')
        s_hw_uart_device = path;
}

static inline const char *hw_uart_get_device(void) {
    return s_hw_uart_device;
}

static inline speed_t _hw_uart_speed(const int baud) {
    switch (baud) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    default:
        return B9600; /* the E22's factory rate, and the only one this driver asks for */
    }
}

static inline esp_err_t hw_uart_start(const gpio_num_t tx, const gpio_num_t rx, const int baud, const int rx_buf_size, const int tx_buf_size) {
    (void)tx;
    (void)rx;
    (void)rx_buf_size;
    (void)tx_buf_size;

    if (s_hw_uart_fd >= 0)
        return ESP_ERR_INVALID_STATE;

    const int fd = open(s_hw_uart_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        ESP_LOGE("hw_uart", "open %s: %s", s_hw_uart_device, strerror(errno));
        return ESP_FAIL;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        ESP_LOGE("hw_uart", "tcgetattr: %s", strerror(errno));
        close(fd);
        return ESP_FAIL;
    }
    cfmakeraw(&tty); /* 8N1, no echo, no translation -- this is a binary protocol */
    tty.c_cflag &= (tcflag_t) ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CS8 | CREAD | CLOCAL);
    tty.c_cc[VMIN] = 0; /* reads never block; poll() does the waiting */
    tty.c_cc[VTIME] = 0;
    (void)cfsetispeed(&tty, _hw_uart_speed(baud));
    (void)cfsetospeed(&tty, _hw_uart_speed(baud));
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        ESP_LOGE("hw_uart", "tcsetattr: %s", strerror(errno));
        close(fd);
        return ESP_FAIL;
    }
    (void)tcflush(fd, TCIOFLUSH);

    s_hw_uart_fd = fd;
    ESP_LOGD("hw_uart", "started: %s @ %d", s_hw_uart_device, baud);
    return ESP_OK;
}

static inline int hw_uart_write(const uint8_t *const data, const size_t len) {
    if (s_hw_uart_fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) { /* short writes are normal on a tty */
        const ssize_t n = write(s_hw_uart_fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd p = { .fd = s_hw_uart_fd, .events = POLLOUT };
                if (poll(&p, 1, 1000) <= 0)
                    break;
                continue;
            }
            break;
        }
        off += (size_t)n;
    }
    return (int)off;
}

static inline esp_err_t hw_uart_wait_tx_done(const int timeout_ms) {
    (void)timeout_ms; /* tcdrain has no timeout; the kernel bounds it by the baud rate */
    if (s_hw_uart_fd < 0)
        return ESP_ERR_INVALID_STATE;
    return tcdrain(s_hw_uart_fd) == 0 ? ESP_OK : ESP_FAIL;
}

static inline int hw_uart_read(uint8_t *const buf, const size_t len, const int timeout_ms) {
    if (s_hw_uart_fd < 0)
        return -1;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    while (got < len) {
        const int64_t remain_us = deadline - esp_timer_get_time();
        if (remain_us <= 0)
            break;
        struct pollfd p = { .fd = s_hw_uart_fd, .events = POLLIN };
        const int pr = poll(&p, 1, (int)(remain_us / 1000) + 1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            break; /* timed out with fewer than len bytes: normal, and how frames are delimited */
        const ssize_t n = read(s_hw_uart_fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    return (int)got;
}

static inline esp_err_t hw_uart_flush(void) {
    if (s_hw_uart_fd < 0)
        return ESP_ERR_INVALID_STATE;
    return tcflush(s_hw_uart_fd, TCIFLUSH) == 0 ? ESP_OK : ESP_FAIL;
}

static inline void hw_uart_stop(void) {
    if (s_hw_uart_fd < 0)
        return;
    (void)tcdrain(s_hw_uart_fd);
    close(s_hw_uart_fd);
    s_hw_uart_fd = -1;
    ESP_LOGD("hw_uart", "stopped");
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_PLATFORM_LINUX_H */
