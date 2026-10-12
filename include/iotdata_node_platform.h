
// common utils
#include "iotdata_node_utils.h"

// state support
#include "iotdata_node_state.h"

// over the air functionality (mapping to TLVs)
#include "iotdata_node_partial.h"
#define IOTDATA_NODE_VERSION_HAS_BLACKBOX
#include "iotdata_node_version.h"
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
#include "iotdata_node_firmware.h"
#include "iotdata_node_otaupdate.h"

// platform runtime
#include "iotdata_node_runtime.h"
