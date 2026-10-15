#ifndef D_PLATFORM_H
#define D_PLATFORM_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_platform.h -- what every d_platform_<platform>.h answers, beyond the hardware shims.
//
// THE BOARD'S OWN IDENTITY AND WHY IT IS RUNNING. Both are questions only the platform can answer
// and neither has anything to do with the protocol, so they are asked here and mapped above:
//
//   platform_identity_mac(mac)            true and six bytes, or false -- NEVER an invented address
//   platform_identity_alt32()             a stable 32-bit id from a second source, 0 where there is none
//   platform_identity_alt_name(buf, size) what that second source was called, for the log line
//   platform_reset_reason()               why we came up, in the terms below
//
// A MAC IS THE PREFERRED IDENTITY BECAUSE IT IS THE BOARD'S. The alternative exists for hosts with
// no permanent NIC, where something stable is better than nothing -- and it is weaker on purpose:
// renaming the host moves the identity, which is why it is only ever the fallback.
//
// ------------------------------------------------------------------------------------------------------------------------
// WHAT A PLATFORM DECLARES RATHER THAN IMPLEMENTS. A capability is a macro the layer above tests
// for instead of testing for a platform, so that a question is asked in its own terms and a third
// port answers it without anyone editing an #if. Each is optional; absent is a real answer.
//
//   D_PERSIST_STORE_PARTITION    a durable record store exists, and it is a labelled flash partition
//   D_PERSIST_STORE_FILE         a durable record store exists, and it is a file
//   D_PERSIST_POOL_SZ_DEFAULT    how big a reset-surviving ring this platform can spare
//   D_PLATFORM_SUPPLY_FLOOR_LVL  a hardware supply floor is enforced, at this setting
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------------------------------------------------------------

/* A RESET REASON OF OUR OWN, because the protocol has its own codes (IOTDATA_NODE_REASON_*) and
   this layer is not allowed to know them. The platform answers in these terms and the node layer
   maps them once, instead of a vendor enum leaking all the way up into a report. */
typedef enum {
    PLATFORM_RESET_UNKNOWN = 0,
    PLATFORM_RESET_POWER_ON,
    PLATFORM_RESET_SOFTWARE,
    PLATFORM_RESET_WATCHDOG,
    PLATFORM_RESET_BROWNOUT,
    PLATFORM_RESET_PANIC,
    PLATFORM_RESET_DEEPSLEEP,
    PLATFORM_RESET_EXTERNAL,
} platform_reset_t;

// ------------------------------------------------------------------------------------------------------------------------

/* THE LOG LEVEL, as a ceiling the platform's own log macros honour. The numbering is ESP-IDF's,
   because one of the two platforms is bound to it and a different order would be a silent
   mistranslation; nothing above this layer cares what the numbers are.

   platform_log_level_get() / platform_log_level_set() are the platform's, so that a console can
   move the ceiling at run time without knowing whose logging it is moving. */
typedef enum {
    PLATFORM_LOG_NONE = 0,
    PLATFORM_LOG_ERROR,
    PLATFORM_LOG_WARN,
    PLATFORM_LOG_INFO,
    PLATFORM_LOG_DEBUG,
    PLATFORM_LOG_VERBOSE,
} platform_log_level_t;

// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_PLATFORM_H */
