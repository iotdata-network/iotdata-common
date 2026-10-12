#ifndef D_MODULE_OTASLOT_LINUX_H
#define D_MODULE_OTASLOT_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_otaslot_linux.h - the slot and hash seams on linux: declared, and not yet implemented.
//
// WHY A HEADER THAT DOES NOTHING IS WORTH HAVING. Supporting an update is part of the contract of
// using this framework, and iotdata_node_otaupdate.h is included by iotdata_node_platform.h -- which
// the linux gateway includes too. So the neutral names have to EXIST on linux or the framework
// header does not compile there, and the gateway would be carrying a contract it cannot name.
//
// WHAT IT DOES INSTEAD OF PRETENDING. ota_slot_impl_init() returns NULL, which
// iotdata_node_content_ota_apply_begin() reads as "no slot" and refuses -- so a gateway reports
// honestly that it cannot apply an update, rather than registering a consumer that would claim a
// firmware bundle and then fail to write it. A stub that lied would be worse than no stub.
//
// A real one needs: a slot (two executables and a selector, or the A/B disk-metadata failover that
// machine-setup.js already builds for the ROCK images), a digest (openssl or mbedtls), and a
// definition of "the running image" to patch deltas against. The seams are unchanged -- that is the
// point of them -- so this file is where that work lands, and nothing above it needs to move.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "d_module_otaslot.h"

// ------------------------------------------------------------------------------------------------------------------------

/* Nothing to keep yet. Not an empty struct: C forbids those, and a byte costs nothing. */
typedef struct {
    uint8_t _unused;
} ota_slot_impl_t;

typedef struct {
    uint8_t _unused;
} ota_hash_impl_t;

// ------------------------------------------------------------------------------------------------------------------------

/* NULL, deliberately -- see WHAT IT DOES INSTEAD OF PRETENDING. */
static inline const ota_slot_t *ota_slot_impl_init(__attribute__((unused)) ota_slot_impl_t *const s) {
    return NULL;
}

static inline const ota_hash_t *ota_hash_impl_init(__attribute__((unused)) ota_hash_impl_t *const h) {
    return NULL;
}

/* 0 is "I cannot say", which is what the format check wants to hear from a platform with no slot:
   a bundle is never refused for being too big for a slot whose size is unknown. */
static inline uint32_t ota_slot_impl_size(void) {
    return 0u;
}

/* false leaves iotdata_node_ota_device_t.base_hash NULL, which the check reads as "I cannot say what
   I am running" -- so a delta is refused for want of a base rather than accepted against a guess. */
static inline bool ota_slot_impl_running_digest(__attribute__((unused)) uint8_t *const out, __attribute__((unused)) const size_t outlen) {
    return false;
}

/* 0 is "I cannot say", and a BOOTLOADER_MIN check treats that as satisfying nothing -- which is the
   safe direction for a platform that does not have a bootloader in this sense at all. */
static inline uint32_t ota_slot_impl_bootloader(void) {
    return 0u;
}

#define OTA_SLOT_IMPL_PRESENT 0

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_OTASLOT_LINUX_H */
