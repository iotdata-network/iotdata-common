// ------------------------------------------------------------------------------------------------------------------------

// common utils
#include "iotdata_node_utils.h"

// persistent state support
#include "iotdata_node_state.h"

// over the air functionality (mapping to TLVs)
#include "iotdata_node_partial.h"
#define IOTDATA_NODE_VERSION_HAS_BLACKBOX
#include "iotdata_node_version.h"
// Beside version, and BEFORE the console block: both answer "what is this node running", and the
// console's `vers` reports them together the way the boot banner does. It depends on nothing
// between here and where it used to sit (esp_ota_ops + esp_partition, and snprintf_inline from
// utils above), so the only thing that moved is when its declarations become available.
#include "iotdata_node_firmware.h"
#include "iotdata_node_variant.h"
#include "iotdata_node_status.h"
#include "iotdata_node_settings.h"
#include "iotdata_node_config.h"
#include "iotdata_node_control.h"
#define IOTDATA_NODE_DIAGNOSTICS 2
#define IOTDATA_NODE_BLACKBOX_IMPLEMENTATION
#include "iotdata_node_diagnostics.h"
#include "iotdata_node_content.h"
#include "iotdata_node_content_ota.h"

// console specific
#include "iotdata_node_console.h"
// #include "iotdata_node_console_config.h"
#include "iotdata_node_console_settings.h"
#include "iotdata_node_console_diagnostics.h"
#include "iotdata_node_console_content.h"

// content specific
#include "iotdata_node_content_control.h"

// modules: firmware
#include "iotdata_node_otaupdate.h"

// platform runtime
#include "iotdata_node_runtime.h"

// ------------------------------------------------------------------------------------------------------------------------
// THE BRING-UP BELOW IS ESP32-ONLY, which the guard is here to say out loud: it reads the reset
// reason, keeps its state in RTC memory so it survives a deep sleep, and logs through ESP_LOG.
// Everything ABOVE the line is shared -- a host build (the gateway) includes this header for the
// headers it gathers, and would otherwise try to compile this too.
#if defined(PLATFORM_ESP32)

// ------------------------------------------------------------------------------------------------------------------------

typedef void (*iotdata_node_platform_caps_fn)(iotdata_node_version_caps_t *const caps);
typedef struct {
    iotdata_node_platform_caps_fn caps_fn;
    iotdata_node_diagnostics_emit_fn diag_emit_fn;
} iotdata_node_platform_conf_t;

#define NODE_PLATFORM_STATE_MAGIC           0xB1E28003UL
typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    iotdata_node_version_caps_t caps;
} iotdata_node_platform_state_t;
static _RTC_DATA_STRUCT iotdata_node_platform_state_t s_node_platform_state;

// ------------------------------------------------------------------------------------------------------------------------

static const iotdata_node_version_caps_t* iotdata_node_platform_caps(void) {
    return &s_node_platform_state.caps;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_platform_init (const char* const tag, const iotdata_node_platform_conf_t* const conf) {
    
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool restarted = (reset_reason != ESP_RST_DEEPSLEEP);
    const bool cold_boot = reset_reason == ESP_RST_USB || !_RTC_DATA_VALID(&s_node_platform_state, NODE_PLATFORM_STATE_MAGIC);

    // diagnostics
    if (conf->diag_emit_fn) iotdata_node_diagnostics_emit_set(conf->diag_emit_fn);
    if (!iotdata_node_diagnostics_begin((uint8_t)reset_reason, restarted))
        ESP_LOGW(tag, "diag: the recorder did not start");
    iotdata_node_diagnostics_flush();

    // initialise
    if (cold_boot) {
        _RTC_DATA_INIT(&s_node_platform_state, NODE_PLATFORM_STATE_MAGIC);
        iotdata_node_version_caps_init(&s_node_platform_state.caps);
        if (conf->caps_fn) conf->caps_fn (&s_node_platform_state.caps);
    }
    char vbuf[IOTDATA_NODE_VERSION_STR_MAX + 1];
    ESP_LOGI(tag, "version: %s", iotdata_node_version_str(vbuf, sizeof(vbuf), &s_node_platform_state.caps));
    if (!iotdata_node_version_stamp_is_real())
        ESP_LOGW(tag, "version: build stamp is unset -- this binary cannot say when it was built");
    char ibuf[IOTDATA_NODE_FIRMWARE_STR_MAX + 1];
    ESP_LOGI(tag, "firmware: %s", iotdata_node_firmware_str(ibuf, sizeof(ibuf)));

    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

#endif /* PLATFORM_ESP32 */
