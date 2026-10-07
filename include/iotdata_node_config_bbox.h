#ifndef IOTDATA_NODE_CONFIG_BBOX_H
#define IOTDATA_NODE_CONFIG_BBOX_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_bbox.h - CONFIG rows for the blackbox recorder.
//
// TWO GROUPS, SPLIT BY BACKEND rather than by role -- which is the same idea the mesh header uses
// and a different cut of it:
//
//     IOTDATA_NODE_CONFIG_ENTRIES_BBOX(X)        what EVERY recorder has, whatever it writes to
//     IOTDATA_NODE_CONFIG_ENTRIES_BBOX_FILE(X)   what only a FILE-backed one has
//
// The general group is the store itself: whether it runs, how much it keeps, and how long a record
// may sit in RAM before it is written down. None of that depends on where "written down" goes, so a
// device recording to a flash partition and a host recording to CSV answer the same three questions
// with the same ids. The file group is the part that only means something when the backend is a
// filesystem: which directory, and how many rotated generations to keep behind the active file.
//
// A node composes the groups its build actually has. One recording to ESP_FLASH takes the general
// group alone and reports no directory, which is the honest answer rather than a path nothing uses.
// (An ESP_FLASH group, with its partition label, is the obvious third -- 0x0C0 when it is wanted.)
//
//     #include "iotdata_node_config.h"            // the types
//     #include "iotdata_node_config_bbox.h"       // the row blocks
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_BBOX(X)
//         IOTDATA_NODE_CONFIG_ENTRIES_BBOX_FILE(X)
//     #include "iotdata_node_config.h"            // expand
//
// 0x0A0 general, 0x0B0 file, and never reused.
//
// WHY THESE ARE WORTH SETTLING AT ALL, when a build-time constant would do: a recorder is what you
// reach for when a node is misbehaving in a way you cannot reproduce, and by then you cannot reflash
// it. Being able to turn it on, or widen what it keeps, on the node that is actually failing is the
// entire point of it being configuration.
//
// ZERO MEANS UNBOUNDED, on every one of the three limits, and that is deliberate rather than an
// accident of using unsigned types: "no limit" is a real choice for a recorder attached to a host
// with a disk, and it needs to be expressible without a second flag beside each number.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_BLACKBOX_ENABLED          0x0A0
#define IOTDATA_NODE_CFGID_BLACKBOX_MAX_RECORDS      0x0A1
#define IOTDATA_NODE_CFGID_BLACKBOX_MAX_SECONDS      0x0A2
#define IOTDATA_NODE_CFGID_BLACKBOX_MAX_BYTES        0x0A3

#define IOTDATA_NODE_CFGID_BLACKBOX_FILE_DIRECTORY   0x0B0
#define IOTDATA_NODE_CFGID_BLACKBOX_FILE_GENERATIONS 0x0B1

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_BBOX_ENABLED
#define IOTDATA_NODE_CONFIG_BBOX_ENABLED false
#endif
#ifndef IOTDATA_NODE_CONFIG_BBOX_MAX_RECORDS
#define IOTDATA_NODE_CONFIG_BBOX_MAX_RECORDS 0u
#endif
#ifndef IOTDATA_NODE_CONFIG_BBOX_MAX_SECONDS
#define IOTDATA_NODE_CONFIG_BBOX_MAX_SECONDS 0u /* 0 = WRITE-THROUGH: every record is persisted as it is made. */
#endif
#ifndef IOTDATA_NODE_CONFIG_BBOX_MAX_BYTES
#define IOTDATA_NODE_CONFIG_BBOX_MAX_BYTES 0u
#endif
#ifndef IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY
#define IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY "."
#endif
#ifndef IOTDATA_NODE_CONFIG_BBOX_FILE_GENERATIONS
#define IOTDATA_NODE_CONFIG_BBOX_FILE_GENERATIONS 10u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY_MAX
#define IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY_MAX 127
#endif

#ifndef IOTDATA_NODE_CONFIG_BBOX_NOTIFY
static inline bool iotdata_node_config_bbox_changed(__attribute__((unused)) const iotdata_node_config_row_t *const row, __attribute__((unused)) const iotdata_node_config_value_t *const was,
                                                    __attribute__((unused)) const iotdata_node_config_info_t *const info) {
    return false;
}
#define IOTDATA_NODE_CONFIG_BBOX_NOTIFY iotdata_node_config_bbox_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* 0x0A0-0x0AF -- every recorder, whatever backend it writes to */
#define IOTDATA_NODE_CONFIG_ENTRIES_BBOX(X) \
    X(BLACKBOX_ENABLED, IOTDATA_NODE_CFGID_BLACKBOX_ENABLED, BOOL, 0, 1, IOTDATA_NODE_CONFIG_BBOX_ENABLED, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, "enabled") \
    X(BLACKBOX_MAX_RECORDS, IOTDATA_NODE_CFGID_BLACKBOX_MAX_RECORDS, U32, 0, 1000000, IOTDATA_NODE_CONFIG_BBOX_MAX_RECORDS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, \
      "records to keep before the oldest goes (0 = unbounded)") \
    X(BLACKBOX_MAX_SECONDS, IOTDATA_NODE_CFGID_BLACKBOX_MAX_SECONDS, U32, 0, 31536000, IOTDATA_NODE_CONFIG_BBOX_MAX_SECONDS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, \
      "records time to keep in RAM until flush/evict (0 = write through)") \
    X(BLACKBOX_MAX_BYTES, IOTDATA_NODE_CFGID_BLACKBOX_MAX_BYTES, U32, 0, 0xFFFFFFFF, IOTDATA_NODE_CONFIG_BBOX_MAX_BYTES, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, \
      "records bytes to keep in RAM before flush/evict (0 = unbounded)")

/* 0x0B0-0x0BF -- only when the backend is a filesystem */
#define IOTDATA_NODE_CONFIG_ENTRIES_BBOX_FILE(X) \
    X(BLACKBOX_FILE_DIRECTORY, IOTDATA_NODE_CFGID_BLACKBOX_FILE_DIRECTORY, STRING, 1, IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY_MAX, IOTDATA_NODE_CONFIG_BBOX_FILE_DIRECTORY, IOTDATA_NODE_CONFIG_FLAG_LOCAL | IOTDATA_NODE_CONFIG_FLAG_REBOOT, \
      NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, "records file directory") \
    X(BLACKBOX_FILE_GENERATIONS, IOTDATA_NODE_CFGID_BLACKBOX_FILE_GENERATIONS, U8, 0, 255, IOTDATA_NODE_CONFIG_BBOX_FILE_GENERATIONS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_BBOX_NOTIFY, \
      "records file generations (behind active) (0 = overwrite)")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_BBOX_H */
