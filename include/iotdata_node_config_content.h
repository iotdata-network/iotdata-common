#ifndef IOTDATA_NODE_CONFIG_CONTENT_H
#define IOTDATA_NODE_CONFIG_CONTENT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_content.h - CONFIG rows for the content store.
//
// TWO GROUPS, SPLIT BY BACKEND, which is the cut iotdata_node_config_bbox.h makes and for the same
// reason:
//
//     IOTDATA_NODE_CONFIG_ENTRIES_CONTENT(X)        what EVERY content store has
//     IOTDATA_NODE_CONFIG_ENTRIES_CONTENT_OTA(X)    and a node that can APPLY a firmware bundle
//     IOTDATA_NODE_CONFIG_ENTRIES_CONTENT_FILE(X)   what only a DIRECTORY-backed one has
//
// The general group is one row, and one row is the honest size of it: whether this node keeps
// content at all. Everything else about a device's store is decided by the partition table -- where
// it is, and how big it is allowed to get -- and a row that restated either would be a row that can
// disagree with the flash layout.
//
// The file group is the part that only means something on a filesystem: which directory, and a
// BUDGET. A device's store is bounded by its partition and the worst a runaway insert can do is
// fill the megabyte set aside for it; a directory on a gateway's root filesystem has no such edge,
// and content is the one thing here that arrives from elsewhere in arbitrary quantity. See
// d_module_filestore_linux.h, which does the arithmetic.
//
//     #include "iotdata_node_config.h"                 // the types
//     #include "iotdata_node_config_content.h"         // the row blocks
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_CONTENT(X)
//         IOTDATA_NODE_CONFIG_ENTRIES_CONTENT_FILE(X)
//     #include "iotdata_node_config.h"                 // expand
//
// 0x0F0 general, 0x110 file, and never reused.
//
// 0x100 IS SKIPPED, which is not tidiness but a collision. The shared blocks have been marching up
// in sixteens since 0x020, and an application's OWN rows live in the same flat space -- the gateway
// took 0x101 for LORA_PORT long before the shared blocks reached there. A shared id is in config
// files on actual machines, so it cannot be moved later and must not be handed out twice; the
// sixteen that is already occupied is the one that gets left alone.
//
// WHY ENABLED IS A ROW AND NOT A BUILD FLAG. A node that is misbehaving is a node you cannot
// reflash, and content is the mechanism by which you would otherwise fix it -- so the ability to
// stop it accepting any more, on the node that is actually filling its flash, is worth a row. It is
// also the row that makes a FLEET decision possible: a relay at a remote site can be told to stop
// taking bundles without being told anything else, and without a visit.
//
// IT IS A GATE ON ACCEPTING, NOT ON HOLDING. Turning it off does not delete what is already there
// and does not stop a consumer finishing with what it owns: a store that threw away a firmware
// bundle because somebody narrowed a setting would be a store nobody could trust with one. What it
// stops is the next insert.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_CONTENT_ENABLED        0x0F0
#define IOTDATA_NODE_CFGID_CONTENT_OTA_ENABLED    0x0F1

#define IOTDATA_NODE_CFGID_CONTENT_FILE_DIRECTORY 0x110
#define IOTDATA_NODE_CFGID_CONTENT_FILE_MAX_BYTES 0x111

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_CONTENT_ENABLED
#define IOTDATA_NODE_CONFIG_CONTENT_ENABLED true /* a node built with a store is a node meant to use it */
#endif
#ifndef IOTDATA_NODE_CONFIG_CONTENT_OTA_ENABLED
#define IOTDATA_NODE_CONFIG_CONTENT_OTA_ENABLED true /* a node built with the consumer is meant to take one */
#endif

#ifndef IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY
#define IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY "."
#endif

#ifndef IOTDATA_NODE_CONFIG_CONTENT_FILE_MAX_BYTES
#define IOTDATA_NODE_CONFIG_CONTENT_FILE_MAX_BYTES 0u /* 0 = whatever the filesystem has, which is right only on its own volume */
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY_MAX
#define IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY_MAX 127
#endif

#ifndef IOTDATA_NODE_CONFIG_CONTENT_NOTIFY
static inline bool iotdata_node_config_content_changed(__attribute__((unused)) const iotdata_node_config_row_t *const row, __attribute__((unused)) const iotdata_node_config_value_t *const was,
                                                       __attribute__((unused)) const iotdata_node_config_info_t *const info) {
    return false;
}
#define IOTDATA_NODE_CONFIG_CONTENT_NOTIFY iotdata_node_config_content_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* 0x0F0-0x0FF -- every content store, whatever backend it writes to */
#define IOTDATA_NODE_CONFIG_ENTRIES_CONTENT(X) \
    X(CONTENT_ENABLED, IOTDATA_NODE_CFGID_CONTENT_ENABLED, BOOL, 0, 1, IOTDATA_NODE_CONFIG_CONTENT_ENABLED, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CONTENT_NOTIFY, "accept new content (what is held is kept either way)")

/* TWO SWITCHES AND NOT ONE, because holding a bundle and running it are different risks. A relay is
   a useful place to park a firmware image for the nodes behind it, and whether it HOLDS one is a
   question about flash; whether it RUNS one is a question about whether you are prepared for that
   node to reboot into something new tonight. A cautious rollout turns this off across a fleet,
   loads the bundle everywhere, and turns it back on a few nodes at a time.
 *
 * It is also the brake. A bundle already sitting on a hundred nodes that turns out to be bad is
 * stopped by this row, over the air, without having to reach any of their flash. */
#define IOTDATA_NODE_CONFIG_ENTRIES_CONTENT_OTA(X) \
    X(CONTENT_OTA_ENABLED, IOTDATA_NODE_CFGID_CONTENT_OTA_ENABLED, BOOL, 0, 1, IOTDATA_NODE_CONFIG_CONTENT_OTA_ENABLED, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_CONTENT_NOTIFY, \
      "apply a firmware bundle that is held (holding one is CONTENT_ENABLED)")

/* 0x110-0x11F -- only when the backend is a directory; see why not 0x100 above */
#define IOTDATA_NODE_CONFIG_ENTRIES_CONTENT_FILE(X) \
    X(CONTENT_FILE_DIRECTORY, IOTDATA_NODE_CFGID_CONTENT_FILE_DIRECTORY, STRING, 1, IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY_MAX, IOTDATA_NODE_CONFIG_CONTENT_FILE_DIRECTORY, \
      IOTDATA_NODE_CONFIG_FLAG_LOCAL | IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, IOTDATA_NODE_CONFIG_CONTENT_NOTIFY, "content file directory") \
    X(CONTENT_FILE_MAX_BYTES, IOTDATA_NODE_CFGID_CONTENT_FILE_MAX_BYTES, U32, 0, 0xFFFFFFFF, IOTDATA_NODE_CONFIG_CONTENT_FILE_MAX_BYTES, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, IOTDATA_NODE_CONFIG_CONTENT_NOTIFY, \
      "content bytes the directory may hold (0 = the filesystem's own free space)")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_CONTENT_H */
