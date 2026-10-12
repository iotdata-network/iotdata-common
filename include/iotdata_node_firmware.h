#ifndef IOTDATA_NODE_FIRMWARE_H
#define IOTDATA_NODE_FIRMWARE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_firmware.h - the RUNNING firmware image: which slot it came out of, what the bootloader
// thinks of it, and the probation that turns a freshly-flashed image into a trusted one.
//
// Not content, and not the OTA blob format. iotdata_node_content_ota.h describes the bytes an update
// ARRIVES as; this describes the slot an image RUNS from. They meet only at the moment a blob is
// written, and keeping them apart is what lets this file exist before any of the content work does.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT ROLLBACK ACTUALLY IS, AND THE OBLIGATION IT CREATES
//
// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the bootloader boots a newly-selected image in state
// PENDING_VERIFY and watches it. If that image reboots without having confirmed itself, the
// bootloader marks it ABORTED and boots the previous one instead. That is the whole safety net, and
// it is also a loaded gun: AN APP THAT NEVER CONFIRMS ITSELF ROLLS BACK ON EVERY OTA, for ever,
// and the symptom is an update that appears to work and is gone after the next restart.
//
// So the config option and iotdata_node_firmware_tick() are a matched pair. Enabling the first without
// calling the second is strictly worse than having neither.
//
// A CABLE FLASH IS NOT ON PROBATION, and that is correct rather than a gap. esptool writes
// ota_data_initial.bin, which is 8 KB of 0xFF, so on the next boot the BOOTLOADER finds otadata in
// its initial state and writes the selection itself -- set_actual_ota_seq() in
// bootloader_utility.c, which commits ota_seq = slot + 1 and ota_state = ESP_OTA_IMG_VALID. So the
// running image reports `valid` from its first boot and is never pending. Probation exists to judge
// an image that arrived by itself; one that arrived on the end of a cable has already been judged by
// the person holding it. (Observed, not assumed: a freshly flashed C3 reads back
// otadata[0] seq=1 state=2.)
//
// WHICH IS ALSO WHY A PROBATION TEST NEEDS NO OTA. Writing an app into the other slot and committing
// an otadata entry with ota_state = ESP_OTA_IMG_NEW is precisely what esp_ota_set_boot_partition()
// does; the bootloader turns NEW into PENDING_VERIFY and boots it (bootloader_utility.c, under
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). Both halves -- confirm, and roll back -- are then testable
// with one build and a cable.
//
// ANTI-ROLLBACK IS DELIBERATELY NOT ENABLED. CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK burns a secure
// version into eFuse, which is irreversible per device and would make a downgrade physically
// impossible -- including the deliberate downgrade to a known-good build that is the recovery path
// when a release turns out to be bad. The protection it offers is against an attacker replaying an
// old signed image, which is a threat this system does not yet have the signing to care about.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHY CONFIRMATION IS A TICK AND NOT THE END OF INITIALISATION
//
// "The hardware came up" is necessary and not sufficient. An image that initialises its radio and
// then dies in its first minute -- a wedged main loop, a watchdog reset, a null dereference on the
// first frame that arrives -- is EXACTLY the image rollback exists to undo, and confirming at the
// end of init would confirm it before it had a chance to fail.
//
// So the evidence is time spent in the loop: begin() is called once the application considers itself
// up, and tick() confirms only after IOTDATA_NODE_FIRMWARE_VALIDATE_S of the loop actually running.
// Anything that kills the node inside that window -- panic, task watchdog, brownout -- takes the
// rollback with it, because none of them reach the confirmation.
//
// A FAILED CONFIRMATION IS RETRIED rather than given up on, because the consequence of not landing
// it is losing a good image at the next restart. The retry is simply another full validate period,
// which keeps a persistently failing NVS from filling the log.
//
// THE VALIDATE PERIOD IS A BUILD OPTION, not a CONFIG row. It is needed before CONFIG is loaded (and
// before CONFIG exists at all on a node that has none), it is an engineering judgement about how long
// a fault takes to show rather than a site preference, and a row an operator could set to zero would
// quietly disarm the only protection an unattended node has.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(ESP_PLATFORM)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#pragma GCC diagnostic pop
#endif

#ifndef IOTDATA_NODE_FIRMWARE_VALIDATE_S
#define IOTDATA_NODE_FIRMWARE_VALIDATE_S 60u /* of the main loop running, not of being powered */
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    IOTDATA_NODE_FIRMWARE_EVENT_NONE = 0,       /* nothing to do, or not yet due                      */
    IOTDATA_NODE_FIRMWARE_EVENT_CONFIRMED,      /* probation passed and the image is now VALID         */
    IOTDATA_NODE_FIRMWARE_EVENT_CONFIRM_FAILED, /* the mark did not land -- will be retried            */
} iotdata_node_firmware_event_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(ESP_PLATFORM)

static uint32_t _iotdata_node_firmware_began_ms = 0u;
static bool _iotdata_node_firmware_probation = false; /* PENDING_VERIFY, and not yet confirmed by us */

static inline const char *iotdata_node_firmware_state_name(const esp_ota_img_states_t s) {
    switch (s) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending-verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
        return "undefined";
    default:
        break; /* -Wswitch-default: covered above, but the enum is the bootloader's, not ours */
    }
    return "?";
}

/* Whether this boot is being judged. False after a cable flash, and false once confirmed. */
static inline bool iotdata_node_firmware_on_probation(void) {
    return _iotdata_node_firmware_probation;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Call once the application considers itself up -- after the hardware it cannot work without has
   started. Reads the state rather than assuming it: a cable flash has none, an OTA has
   PENDING_VERIFY, and a slot that has already been confirmed has VALID. */
static inline void iotdata_node_firmware_begin(const uint32_t now_ms) {
    _iotdata_node_firmware_began_ms = now_ms;
    _iotdata_node_firmware_probation = false;
    const esp_partition_t *const running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
        _iotdata_node_firmware_probation = true;
}

/* Every loop pass. Does nothing at all unless this boot is on probation. */
static inline iotdata_node_firmware_event_t iotdata_node_firmware_tick(const uint32_t now_ms) {
    if (!_iotdata_node_firmware_probation)
        return IOTDATA_NODE_FIRMWARE_EVENT_NONE;
    if ((now_ms - _iotdata_node_firmware_began_ms) < (uint32_t)IOTDATA_NODE_FIRMWARE_VALIDATE_S * 1000u)
        return IOTDATA_NODE_FIRMWARE_EVENT_NONE;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        _iotdata_node_firmware_probation = false;
        return IOTDATA_NODE_FIRMWARE_EVENT_CONFIRMED;
    }
    /* Stay on probation and come back in another full period: losing a good image at the next
       restart is worse than trying again, and a fresh period keeps a failing store out of the log. */
    _iotdata_node_firmware_began_ms = now_ms;
    return IOTDATA_NODE_FIRMWARE_EVENT_CONFIRM_FAILED;
}

/* Give up on the running image and return to the previous one, now. Refuses on its own when there is
   nothing valid to go back to -- which is the case on a board that has only ever been cable-flashed
   -- so it cannot be used to reboot into a hole. Does not return when it succeeds. */
/* IS THERE ANYWHERE TO GO, asked separately so a caller can do its own tidying up BEFORE the point
   of no return. esp_ota_mark_app_invalid_rollback_and_reboot() does not return when it works, so
   anything that must be written down first has to be written down before it is called -- and
   writing it down before knowing the rollback is even possible would leave a refused command with
   side effects. */
static inline bool iotdata_node_firmware_rollback_possible(void) {
    return esp_ota_check_rollback_is_possible();
}

static inline bool iotdata_node_firmware_rollback(void) {
    return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* One line for the boot banner and for an operator: where we are running from, what the bootloader
   thinks of it, and WHAT WE WOULD FALL BACK TO -- which is the part that cannot be inferred, and the
   part that says whether the safety net is actually strung.

   PROBATION IS READ FROM THE STATE, not from our own flag, so this is printable BEFORE begin() has
   run -- which is where a boot banner wants it. It also reports what the bootloader decided rather
   than what we noticed, and it clears by itself once the mark lands and the state turns VALID. */
static inline const char *iotdata_node_firmware_str(char *const buf, const size_t size) {
    if (buf == NULL || size == 0u)
        return "";
    const esp_partition_t *const running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        const bool stated = (esp_ota_get_state_partition(running, &state) == ESP_OK);
        /* The other slot. "next update" is the one an OTA would be written to, which is by definition
        the one a rollback would come back from. Empty is the honest answer on a fresh board and the
        reason `reset firmware` refuses there. */
        char other[64] = "none";
        const esp_partition_t *const next = esp_ota_get_next_update_partition(NULL);
        if (next != NULL) {
            esp_app_desc_t desc;
            if (esp_ota_get_partition_description(next, &desc) == ESP_OK)
                (void)snprintf(other, sizeof(other), "%s (%.16s %.14s)", next->label, desc.project_name, desc.version); /* 64 bytes: the widest is 51 */
            else
                (void)snprintf(other, sizeof(other), "%s (empty)", next->label);
        }
        return snprintf_inline(buf, size, "slot=%s at 0x%06" PRIx32 " %" PRIu32 "K state=%s%s, fallback=%s", running->label, (uint32_t)running->address, (uint32_t)(running->size / 1024u),
                               stated ? iotdata_node_firmware_state_name(state) : "none", (stated && state == ESP_OTA_IMG_PENDING_VERIFY) ? " ON PROBATION" : "", other);
    } else
        return snprintf_inline(buf, size, "slot=unknown");
}

#ifndef IOTDATA_NODE_FIRMWARE_STR_MAX
#define IOTDATA_NODE_FIRMWARE_STR_MAX 160
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#else /* !ESP_PLATFORM -- a host build has one image and no slots, so every call is inert */

#ifndef IOTDATA_NODE_FIRMWARE_STR_MAX
#define IOTDATA_NODE_FIRMWARE_STR_MAX 160
#endif

static inline bool iotdata_node_firmware_on_probation(void) {
    return false;
}
static inline void iotdata_node_firmware_begin(__attribute__((unused)) const uint32_t now_ms) {
}
static inline iotdata_node_firmware_event_t iotdata_node_firmware_tick(__attribute__((unused)) const uint32_t now_ms) {
    return IOTDATA_NODE_FIRMWARE_EVENT_NONE;
}
static inline bool iotdata_node_firmware_rollback_possible(void) {
    return false;
}
static inline bool iotdata_node_firmware_rollback(void) {
    return false;
}
static inline const char *iotdata_node_firmware_str(char *const buf, const size_t size) {
    if (buf == NULL || size == 0u)
        return "";
    return snprintf_inline(buf, size, "slot=n/a (not a slotted platform)");
}

#endif /* ESP_PLATFORM */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_FIRMWARE_H */
