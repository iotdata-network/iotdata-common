#ifndef IOTDATA_NODE_CONTENT_CONTROL_H
#define IOTDATA_NODE_CONTENT_CONTROL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_content_control.h - the store's CONTROL words: list, delete, inspect, validate.
//
// WHY THIS IS NOT IN iotdata_node_content.h, which is where iotdata_node_diagnostics.h keeps its
// equivalent. The store module is deliberately free of the protocol: it builds and is tested on a
// host against a RAM backend with nothing but a checksum underneath it, and dragging the action ids
// in would make the one piece of this that can be exercised without a device depend on the library.
// So the handler sits one layer out, where it can see both, and the module stays testable.
//
// WHAT IS HERE AND WHAT IS IN iotdata_node_console_content.h is the split the recorder already
// makes: these four have a WIRE equivalent and therefore cannot be written twice, so they live in
// iotdata_node_control.h's table and are reached by a console and by MQTT through the same handler.
// `insert` and `download` have no wire equivalent in this shape -- both are a byte stream, and the
// wire's answer to a byte stream is the CONTENT TLVs rather than a command with a vast argument --
// so they are console-only and live over there.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// INSPECT KNOWS ABOUT OTA, ON PURPOSE, AND IS NOT THE OTA CONSUMER
//
// `content inspect` parses an OTA header if the file starts with one and reports what the checklist
// in iotdata_node_content_ota.h makes of it. That is a READ: it claims nothing, owns nothing,
// writes nothing and reboots nothing. It exists so that a bundle can be put on a device and
// questioned before anything is built that would act on it -- which is exactly how the faux-header
// bring-up is done -- and it stays useful afterwards as the way to ask a device what it thinks of
// a blob without asking it to take it.
//
// The OTA CONSUMER is a different thing entirely: it claims the file, decides when to apply it,
// writes a slot and reboots. When it arrives it will describe() its own files, and this reports
// that description as well -- so the two answers sit side by side rather than one replacing the
// other.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// VALIDATE COMPARES AGAINST NOTHING, AND SAYS SO
//
// It reads the file back out of the store and reports §7's CRC-64 over what it found. It does not
// say "good" or "bad", because there is nothing yet to be good or bad against: the gross hash lives
// in §3's MANIFEST, and there is no manifest until the wire path brings one. Until then the number
// is for the operator to compare with what they sent, which they have and the node does not.
//
// That is worth having on its own. The insert's checksum proves the TRANSFER; this proves the
// STORE, some time later, which is a different claim and the one that catches a decayed page or a
// factory-flashed file that never had a transfer behind it at all.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <inttypes.h>

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX
#define IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX 160
#endif

/* How much of a file is read to look for a header in it. The header is bounded by the kvr keys the
   bundler writes -- app, semver, stamp, three hashes, the sizes -- and 256 covers every one of
   them with room over; a parse that wants more than this is looking at something that is not one. */
#ifndef IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES
#define IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES 256u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* (subject, action) PAIRS, the shape the CONTROL inventory emits and the shape a command takes on
   the wire, so advertising one and sending one cannot drift apart. */
/* As a macro so it can be SPLICED beside another module's -- see the long note by
   IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS, which is where the argument for this lives. The
   trailing comma is what lets two of these sit next to each other in one initialiser. */
#define IOTDATA_NODE_CONTENT_CONTROL_ACTION_PAIRS \
    IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, IOTDATA_NODE_TLV_CONTENT, \
        IOTDATA_NODE_ACTION_CONTENT_VALIDATE, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_STATUS, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_APPLY,

static const uint8_t iotdata_node_content_control_actions[] = { IOTDATA_NODE_CONTENT_CONTROL_ACTION_PAIRS };
#define IOTDATA_NODE_CONTENT_CONTROL_ACTIONS_COUNT ((uint8_t)(sizeof(iotdata_node_content_control_actions) / (2 * sizeof(iotdata_node_content_control_actions[0]))))

// -----------------------------------------------------------------------------------------------------------------------------------------

static iotdata_node_content_t *_iotdata_node_content_control_store = NULL;
static const iotdata_node_ota_device_t *_iotdata_node_content_control_device = NULL;
static char _iotdata_node_content_control_line[IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX];

/* `device` is what this build knows about itself, and NULL is a legitimate answer: inspect then
   parses the header and declines to judge it, rather than judging it against zeroes and calling a
   perfectly good bundle unacceptable. */
static inline void iotdata_node_content_control_init(iotdata_node_content_t *const c, const iotdata_node_ota_device_t *const device) {
    _iotdata_node_content_control_store = c;
    _iotdata_node_content_control_device = device;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#define _IOTDATA_NODE_CONTENT_CONTROL_SAY(...) \
    do { \
        _iotdata_node_content_say(snprintf_inline(_iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), __VA_ARGS__)); \
    } while (0)

/* A u24 id, big-endian, exactly as iotdata_node_control_build writes it. Three bytes and not four
   because §4's id is 24 bits: a four-byte field would have a quarter of itself permanently zero
   and would invite someone to put something in it. */
static inline bool _iotdata_node_content_control_id(const uint8_t *const args, const uint8_t arglen, uint32_t *const id) {
    if (args == NULL || arglen < 3u)
        return false;
    *id = ((uint32_t)args[0] << 16) | ((uint32_t)args[1] << 8) | (uint32_t)args[2];
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_list(iotdata_node_content_t *const c) {
    uint32_t cursor = 0u, id = 0u, total = 0u, held = 0u, files = 0u;
    const char *owner = NULL;
    while (iotdata_node_content_iterate(c, &cursor, &id, &total, &held, &owner)) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %8" PRIu32 " of %8" PRIu32 " %-10s%s", id, held, total, (owner != NULL) ? owner : "-", iotdata_node_content_id_is_scratch(id) ? " scratch" : "");
        files++;
    }
    /* The totals LAST, so a long listing ends with the summary rather than opening with it and
       scrolling it away -- and so the line that says how many there were is the line that proves
       the walk finished. */
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %" PRIu32 " file%s, %" PRIu32 " bytes free", files, (files == 1u) ? "" : "s", iotdata_node_content_space(c));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_delete(iotdata_node_content_t *const c, const uint32_t id) {
    const iotdata_node_content_status_t st = iotdata_node_content_delete(c, id);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, (st == IOTDATA_NODE_CONTENT_OK) ? "deleted" : iotdata_node_content_status_name(st));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_validate(iotdata_node_content_t *const c, const uint32_t id) {
    uint64_t crc = 0u;
    uint32_t len = 0u;
    const iotdata_node_content_status_t st = iotdata_node_content_checksum(c, id, &crc, &len);
    if (st == IOTDATA_NODE_CONTENT_OK)
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " crc64=0x%016" PRIX64 " over %" PRIu32 " bytes", id, crc, len);
    else if (st == IOTDATA_NODE_CONTENT_ERR_SIZE)
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " crc64=0x%016" PRIX64 " over %" PRIu32 " bytes -- INCOMPLETE, so the sum is of a fragment", id, crc, len);
    else
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, iotdata_node_content_status_name(st));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_inspect_ota(const uint32_t id, const uint8_t *const peek, const size_t peek_len, const uint32_t file_len) {
    iotdata_node_ota_header_t h;
    const iotdata_node_ota_status_t pst = iotdata_node_ota_parse(peek, peek_len, &h);
    if (pst == IOTDATA_NODE_OTA_ERR_MAGIC)
        return; /* not one, and silence is the right answer: most content is not firmware */
    if (pst != IOTDATA_NODE_OTA_OK) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota header: %s", id, iotdata_node_ota_status_name(pst));
        return;
    }
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota app=%s semver=%s stamp=%s", id, h.app, h.semver, h.stamp);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota form=%s codec=%s flags=0x%02x bootloader>=%" PRIu32, id, iotdata_node_ota_form_name(h.form), iotdata_node_ota_codec_name(h.codec), (unsigned)h.flags, h.bootloader_min);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota output=%" PRIu32 " payload=%" PRIu32 " at %u hashes=%s%s%s", id, h.output_size, h.payload_size, (unsigned)h.payload_offset, h.has_target_hash ? "target " : "",
                                      h.has_base_hash ? "base " : "", h.has_payload_hash ? "payload" : "");
    /* The length check is the one that needs the WHOLE file, and the whole file is what the store
       says it holds -- so a bundle that arrived short is caught here and not at the far end of a
       decode, which is the entire argument for PAYLOAD_SIZE existing. */
    if (_iotdata_node_content_control_device == NULL) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota check: not judged (this build declares nothing about itself)", id);
        return;
    }
    const iotdata_node_ota_status_t cst = iotdata_node_ota_check(&h, _iotdata_node_content_control_device, (size_t)file_len);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota check: %s", id, iotdata_node_ota_status_name(cst));
}

static inline void _iotdata_node_content_control_inspect(iotdata_node_content_t *const c, const uint32_t id) {
    uint32_t total = 0u, held = 0u;
    if (!iotdata_node_content_stat(c, id, &total, &held)) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, iotdata_node_content_status_name(IOTDATA_NODE_CONTENT_ERR_ABSENT));
        return;
    }
    /* THREE ANSWERS AND NOT TWO, because describe() returns 0 for two different situations and
       `orphan` was being printed for both: a file nobody owns, which on a build with no consumer
       registered is EVERY file and perfectly normal, and a file whose owner declines to say
       anything, which is an owned file and not an orphan at all. Asking who owns it separates
       them, and the honest wording for the common case says what is true rather than sounding an
       alarm about it. */
    char what[IOTDATA_NODE_CONTENT_DESCRIBE_MAX];
    const int n = iotdata_node_content_describe(c, id, what, sizeof(what));
    const char *const owner = iotdata_node_content_owner(c, id);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %" PRIu32 " of %" PRIu32 " bytes%s -- %s", id, held, total, iotdata_node_content_id_is_scratch(id) ? ", scratch" : "",
                                      (n > 0)           ? what
                                      : (owner != NULL) ? "claimed, and its owner says nothing about it"
                                                        : "nobody has claimed it");

    uint8_t peek[IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES];
    const uint32_t want = (held < (uint32_t)sizeof(peek)) ? held : (uint32_t)sizeof(peek);
    if (want >= (uint32_t)IOTDATA_NODE_OTA_MAGIC_LEN && iotdata_node_content_read(c, id, 0u, peek, want) == (int)want)
        _iotdata_node_content_control_inspect_ota(id, peek, (size_t)want, held);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_content_control(const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    iotdata_node_content_t *const c = _iotdata_node_content_control_store;
    if (c == NULL || subject != IOTDATA_NODE_TLV_CONTENT)
        return false;
    uint32_t id = 0u;
    switch (action) {
    case IOTDATA_NODE_ACTION_CONTENT_LIST:
        _iotdata_node_content_control_list(c);
        return true;
    case IOTDATA_NODE_ACTION_CONTENT_DELETE:
        if (_iotdata_node_content_control_id(args, arglen, &id))
            _iotdata_node_content_control_delete(c, id);
        else
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
        return true;
    case IOTDATA_NODE_ACTION_CONTENT_INSPECT:
        if (_iotdata_node_content_control_id(args, arglen, &id))
            _iotdata_node_content_control_inspect(c, id);
        else
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
        return true;
    case IOTDATA_NODE_ACTION_CONTENT_VALIDATE:
        if (_iotdata_node_content_control_id(args, arglen, &id))
            _iotdata_node_content_control_validate(c, id);
        else
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
        return true;
    /* THE CONSUMER IS REACHED THROUGH ITS OWN MODULE POINTER, which its begin() sets, rather than
       being registered here a second time. A build with no firmware consumer answers honestly and
       the words still resolve -- which is better than a verb that exists on some nodes and is
       "unknown command" on others. */
    case IOTDATA_NODE_ACTION_CONTENT_OTA_STATUS:
        if (_iotdata_node_ota_apply == NULL)
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: no consumer -- this build does not apply updates");
        else
            iotdata_node_content_ota_apply_report(_iotdata_node_ota_apply, _iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), _iotdata_node_content_say);
        return true;
    case IOTDATA_NODE_ACTION_CONTENT_OTA_APPLY:
        if (_iotdata_node_ota_apply == NULL)
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: no consumer -- this build does not apply updates");
        else if (!_iotdata_node_content_control_id(args, arglen, &id))
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
        else if (!iotdata_node_content_ota_apply_retry(_iotdata_node_ota_apply, id))
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: %06" PRIx32 " not armed -- %s", id, iotdata_node_content_ota_apply_busy(_iotdata_node_ota_apply) ? "an update is already running" : "updates are switched off");
        else
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: %06" PRIx32 " armed -- it will be taken on the next offer", id);
        return true;
    default:
        break;
    }
    return false;
}

static inline bool iotdata_node_content_node_control(__attribute__((unused)) const uint16_t station, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    return iotdata_node_content_control(subject, action, args, arglen);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_CONTROL_H */
