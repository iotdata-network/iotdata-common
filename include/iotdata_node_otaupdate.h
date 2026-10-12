#ifndef IOTDATA_NODE_OTAUPDATE_H
#define IOTDATA_NODE_OTAUPDATE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_otaupdate.h -- everything an application needs to take a firmware update, in one
// object it declares once.
//
// NOT A REPLACEMENT FOR iotdata_node_content_ota.h. That header is the format and the consumer, and
// it stays exactly as it is -- this is the assembly above it. Nothing here decides anything; it
// holds the eight pieces the consumer wants and hands them over in the right order.
//
// WHAT IT IS FOR. The relay used to declare the slot, the hash, the inflater, the patcher, the
// record, the device descriptor, the attempt buffer and the consumer itself as eight separate
// file-scope objects, then wire them together in twenty lines of begin() -- reading the bootloader
// version, asking the partition table for the slot size, digesting the running image for a delta's
// base, and passing all of it to one call with ten arguments. Every other node that takes an update
// would have copied those twenty lines, and the fourth copy is where the orders start to drift.
// Here it is one struct and one call, and the twenty lines live in one place where they can be got
// right once.
//
// THE PLATFORM IS CHOSEN BY THE APPLICATION, not here. The instance types are spelled
// ota_slot_impl_t, ota_hash_impl_t and ota_codec_impl_t -- neutral names that every backend of those
// seams declares -- so this header never mentions esp32 or linux and contains no #ifdef about them.
// Which backend an application gets is decided by which d_ module it includes, which is the one
// place that choice is visible. See THE NEUTRAL NAMES in d_module_otaslot_esp32.h.
//
// WHAT MUST ALREADY BE INCLUDED, in this order, by the application or by iotdata_node_platform.h on
// its behalf (no iotdata_node_* header includes another):
//
//     the d_ modules       d_module_otaslot_{esp32,linux}.h, d_module_inflate_*.h, d_module_hpatch.h
//     the library          iotdata.h, for the kvr the bundle header is framed in
//     the framework        iotdata_node_content.h, iotdata_node_content_ota.h
//
// A NODE WITH NO SLOT STILL COMPILES. On a platform whose ota_slot_impl_init() returns NULL -- linux
// today -- begin() refuses and says so, and the application carries on without an update path. That
// is the honest outcome and it is why the stub exists rather than an #ifdef here.
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE ONE OBJECT. Sized by the consumer inside it, which carries the apply path's two buffers, so
   this is a few kilobytes and belongs in bss or a single allocation -- not on a stack. */
typedef struct {
    /* the four implementations, each owning its own vtable (see the note in d_module_hpatch.h) */
    ota_slot_impl_t slot;
    ota_hash_impl_t hash;
    ota_codec_impl_t codec;
    hpatch_t patch;

    /* what the consumer is told about this node, and the buffers behind the two string/hash fields
       it only borrows pointers to */
    iotdata_node_ota_device_t device;
    uint8_t base_hash[IOTDATA_NODE_OTA_HASH_LEN];
    char stamp[IOTDATA_NODE_OTA_STAMP_LEN + 1];

    /* and the durable record's seam, whose save() is the application's -- it is the layer that knows
       where this node keeps state */
    ota_record_t record;

    iotdata_node_ota_apply_t apply;
} iotdata_node_otaupdate_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE BLOCK AN APPLICATION REGISTERS WITH ITS STATE STORE, and it may ask for this before begin():
   the consumer's record survives begin, so a state layer that wants every block in before it
   restores anything can take this first. See THE DURABLE RECORD SURVIVES THIS CALL. */
static inline void *iotdata_node_otaupdate_record(iotdata_node_otaupdate_t *const o, size_t *const size) {
    return (o != NULL) ? iotdata_node_content_ota_apply_record(&o->apply, size) : NULL;
}

/* WIRE IT ALL UP AND REGISTER THE CONSUMER. Call BEFORE iotdata_node_content_begin(), which is what
   offers the store's files to whoever wants them -- registered after it, a node that rebooted with
   an update waiting would sit on it until the re-offer cadence came round.
 *
 * `save` is called synchronously when the attempt record must be on durable storage before the slot
 * is activated; `app` is this build's name as a bundle must claim it; `my_stamp` is this build's
 * stamp, or NULL when the build cannot say when it was made -- which costs the counter its ability
 * to clear itself and is handled, not rejected.
 *
 * WHAT IT GATHERS THAT AN APPLICATION OTHERWISE HAD TO. The bootloader floor and the slot size come
 * from the slot implementation, and so does the digest of the running image -- the base a delta is
 * patched onto. Left NULL when the platform cannot say, which the format check reads as "I cannot
 * say" and refuses a delta for, rather than guessing. */
static inline bool iotdata_node_otaupdate_begin(iotdata_node_otaupdate_t *const o, iotdata_node_content_t *const store, const char *const app, const char *const my_stamp, bool (*const save)(void *ctx), void *const save_ctx,
                                                const bool enabled) {
    if (o == NULL || store == NULL)
        return false;

    /* the device descriptor, and the buffers the pointers in it refer to */
    o->device = (iotdata_node_ota_device_t){ 0 };
    o->device.app = app;
    if (my_stamp != NULL) {
        (void)snprintf(o->stamp, sizeof(o->stamp), "%s", my_stamp);
        o->device.stamp = o->stamp;
    }
    o->device.bootloader = ota_slot_impl_bootloader();
    o->device.slot_size = ota_slot_impl_size();
    o->device.codec_deflate = true; /* every backend of ota_codec_impl_t is a raw-deflate inflater */
    o->device.codec_heatshrink = false;
    if (ota_slot_impl_running_digest(o->base_hash, sizeof(o->base_hash)))
        o->device.base_hash = o->base_hash;

    o->record = (ota_record_t){ .ctx = save_ctx, .save = save };

    return iotdata_node_content_ota_apply_begin(&o->apply, store, &o->record, &o->device, ota_slot_impl_init(&o->slot), ota_hash_impl_init(&o->hash), ota_codec_impl_init(&o->codec), ota_patch_init(&o->patch), o->device.stamp, enabled);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE PASS-THROUGHS
//
// Thin on purpose: an application that holds one of these should not have to reach past it into the
// consumer to say the four things it needs to say. Each is the consumer's own call with the object
// unwrapped, and each is named for what the application means rather than for the mechanism.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The device descriptor, for whoever else wants to describe a bundle -- the content control module
   takes one so that `content inspect` can judge a file this node will never apply. */
static inline const iotdata_node_ota_device_t *iotdata_node_otaupdate_device(iotdata_node_otaupdate_t *const o) {
    return (o != NULL) ? &o->device : NULL;
}

/* What the LAST boot was trying to do, once the state store has put the record back. */
static inline bool iotdata_node_otaupdate_pending(const iotdata_node_otaupdate_t *const o, char *const out, const size_t size) {
    return (o != NULL) && iotdata_node_content_ota_apply_pending(&o->apply, out, size);
}

/* On FIRMWARE_EVENT_CONFIRMED and nowhere else: the first moment the new image cannot be taken away. */
static inline void iotdata_node_otaupdate_confirmed(iotdata_node_otaupdate_t *const o) {
    if (o != NULL)
        iotdata_node_content_ota_apply_confirmed(&o->apply);
}

/* Immediately before a deliberate rollback, so the consumer does not read it as a crash and
   re-apply what was just rejected. See ABANDONED ON PURPOSE. */
static inline void iotdata_node_otaupdate_abandon(iotdata_node_otaupdate_t *const o) {
    if (o != NULL)
        iotdata_node_content_ota_apply_abandon(&o->apply);
}

/* The config row moved: tell the consumer rather than wait for the next boot. */
static inline void iotdata_node_otaupdate_enable(iotdata_node_otaupdate_t *const o, const bool on) {
    if (o != NULL)
        iotdata_node_content_ota_apply_enable(&o->apply, on);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_OTAUPDATE_H */
