#ifndef IOTDATA_NODE_CONTROL_H
#define IOTDATA_NODE_CONTROL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_control.h - driving a node: the command vocabulary, its packaging, and the
// translation to and from whatever medium the operator is using.
//
// A TABLE, so a medium is a thin adapter. Each medium fills an iotdata_node_control_args_t from
// whatever it has -- JSON members here, argv on a console later -- and the packaging is shared.
// Adding a medium should not touch the vocabulary, and adding a command should not touch a medium.
//
// A NODE PLUGS IN ITS OWN. iotdata_node_control_init() registers commands specific to one device (a
// TSA's "calibrate", say). The built-ins are searched FIRST, so an application cannot quietly
// shadow `boot` or `stat` and leave an operator holding a word that means something else here.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* How many stations one request may name. Bounded because each becomes its own DOWN frame in the
   gateway's staging queue: a request that would outrun that queue is refused whole, rather than
   half-delivered with no way to say which half. */
#ifndef IOTDATA_NODE_CONTROL_TARGETS_MAX
#define IOTDATA_NODE_CONTROL_TARGETS_MAX 8
#endif

/*
 * THE MQTT MANAGEMENT CHANNEL. Two topics, suffixed onto whatever prefix a deployment uses: one
 * that requests arrive on, one that every answer goes to.
 */
#define IOTDATA_NODE_MQTT_MANAGE_TOPIC_REQ  "/manage/req"
#define IOTDATA_NODE_MQTT_MANAGE_TOPIC_RESP "/manage/resp"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* What a command does with the request's arguments. The kind is a property of the COMMAND, not of
   the medium, which is what lets one table serve all of them. */
typedef enum {
    IOTDATA_NODE_CONTROL_ARG_NONE,           /* a flag: the key present, no value */
    IOTDATA_NODE_CONTROL_ARG_FIXED,          /* a u8 the command itself fixes (enable = 1, disable = 0) */
    IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS,   /* a u8 from "scope", read as STATUS_SCOPE_* (which groups) */
    IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER,   /* a u8 from "scope", read as FILTER_SCOPE_* (which entries) */
    IOTDATA_NODE_CONTROL_ARG_STATION,        /* { u16 station, u8 action } -- the action fixed by the command */
    IOTDATA_NODE_CONTROL_ARG_STATION_ACTION, /* { u16 station, u8 action } -- the action named in the request */
    IOTDATA_NODE_CONTROL_ARG_REPORTS,        /* not one key: every report that can be asked for */
} iotdata_node_control_arg_t;

#define IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST 0xFF

typedef struct {
    const char *name;
    uint8_t key;     /* IOTDATA_NODE_CONTROL_REQUEST or _CONTROL */
    uint8_t subject; /* a TLV type, or NODE / MESH */
    uint8_t action;  /* only meaningful when key == _CONTROL */
    iotdata_node_control_arg_t arg;
    uint8_t fixed; /* FIXED: the value. STATION: the action. SCOPE: an override, or FROM_REQUEST. */
} iotdata_node_control_command_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Every CONTROL value begins the same way: the subject, and for a command its action. Whatever the
   argument kind adds, it adds after this. */
static inline uint8_t _iotdata_node_control_prefix(const iotdata_node_control_command_t *const c, uint8_t *const out) {
    out[0] = c->subject;
    if (c->key == IOTDATA_NODE_CONTROL_REQUEST)
        return 1;
    out[1] = c->action;
    return 2;
}

/* Everything a command might need, however the medium expressed it. One struct rather than a
   parameter list, because the next medium fills the same fields from argv. */
typedef struct {
    /* Who it is for. `target` is simply the first of `targets`, so a request naming one station --
       which is most of them -- reads exactly as it always did. A LIST is the general case: the
       wire carries ONE 12-bit station per DOWN frame, so addressing several means the gateway
       emitting several frames, and that fan-out is the gateway's business rather than the
       requester's. Broadcast absorbs the rest: asking for "all, plus 0537" is asking for all. */
    uint16_t target; /* the node to drive; BROADCAST when unsaid */
    uint16_t targets[IOTDATA_NODE_CONTROL_TARGETS_MAX];
    uint8_t targets_count;
    bool targets_overflow; /* the request named more than fit: it must be refused, not truncated */
    uint16_t station;      /* the station a mesh command acts ON, which is not the target */
    uint8_t scope_status;  /* STATUS_SCOPE_* bits; 0 = every group */
    uint8_t scope_filter;  /* FILTER_SCOPE_*; ALL when unsaid */
    uint8_t action;        /* FILTERS_BLOCK / _ALLOW / _NONE */
} iotdata_node_control_args_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE VOCABULARY
// -----------------------------------------------------------------------------------------------------------------------------------------

static const iotdata_node_control_command_t iotdata_node_control_commands[] = {

/* Shorthands, so the table reads as a table rather than as a wall of prefixes. */
#define _REQ(subject)         IOTDATA_NODE_CONTROL_REQUEST, (subject), 0
#define _CMD(subject, action) IOTDATA_NODE_CONTROL_CONTROL, (subject), (action)

    /* --- the system TLVs, one request each --------------------------------------------------
       RECEIVE is here now. Under the old scheme its derived key was 0x00, which REBOOT already
       had, so "when are you next reachable?" was a question the protocol could not ask. */
    { "recv", _REQ(IOTDATA_NODE_TLV_RECEIVE), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "vers", _REQ(IOTDATA_NODE_TLV_VERSION), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "vari", _REQ(IOTDATA_NODE_TLV_VARIANT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "ctrl", _REQ(IOTDATA_NODE_TLV_CONTROL), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "stat", _REQ(IOTDATA_NODE_TLV_STATUS), IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS, IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST },
    { "conf", _REQ(IOTDATA_NODE_TLV_CONFIG), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "diag", _REQ(IOTDATA_NODE_TLV_DIAGNOSTICS), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "cont", _REQ(IOTDATA_NODE_TLV_CONTENT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "reports", 0, 0, 0, IOTDATA_NODE_CONTROL_ARG_REPORTS, 0 }, /* every report, in type order */

    /* --- the node itself --------------------------------------------------------------------- */
    { "boot", _CMD(IOTDATA_NODE_SUBJECT_NODE, IOTDATA_NODE_ACTION_NODE_REBOOT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

    /* --- the recorder ------------------------------------------------------------------------ */
    { "diag-enable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 1 },
    { "diag-disable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 0 },
    { "diag-clear", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_CLEAR), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "diag-dump", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

    /* --- the tables ---------------------------------------------------------------------------
       There is no word for "give me the stations table": it is `stat stations`, which the `stat`
       command above already spells, since a table is a STATUS scope. Only the things a scope
       CANNOT express -- dumping to a console, changing a table -- are commands of their own.

       STATIONS and FILTERS are STATUS subjects, not MESH ones: neither is a mesh concept. Any node
       that hears traffic has stations, and any node can refuse one. Only peers is mesh. */
    { "stations-dump", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_STATUS_STATIONS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "filters-update", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_STATUS_FILTERS_UPDATE), IOTDATA_NODE_CONTROL_ARG_STATION_ACTION, 0 },
    { "filters-clear", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_STATUS_FILTERS_CLEAR), IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER, IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST },
    { "filters-dump", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_STATUS_FILTERS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

    /* --- mesh management --------------------------------------------------------------------- */
    { "mesh-peers-update", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_UPDATE), IOTDATA_NODE_CONTROL_ARG_STATION_ACTION, 0 },
    { "mesh-peers-clear", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "mesh-peers-dump", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

#undef _REQ
#undef _CMD
};

#define IOTDATA_NODE_CONTROL_COMMANDS_COUNT ((uint8_t)(sizeof(iotdata_node_control_commands) / sizeof(iotdata_node_control_commands[0])))

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Commands one device adds to the common set -- see iotdata_node_control_init. */
static const iotdata_node_control_command_t *_iotdata_node_control_app = NULL;
static uint8_t _iotdata_node_control_app_count = 0;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_control_init(const iotdata_node_control_command_t *const app, const uint8_t count) {
    _iotdata_node_control_app = app;
    _iotdata_node_control_app_count = (app != NULL) ? count : 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_node_control_command_t *iotdata_node_control_find(const char *const name) {
    if (name == NULL)
        return NULL;
    for (uint8_t i = 0; i < IOTDATA_NODE_CONTROL_COMMANDS_COUNT; i++) // builtins first
        if (strcmp(iotdata_node_control_commands[i].name, name) == 0)
            return &iotdata_node_control_commands[i];
    for (uint8_t i = 0; i < _iotdata_node_control_app_count; i++)
        if (strcmp(_iotdata_node_control_app[i].name, name) == 0)
            return &_iotdata_node_control_app[i];
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_node_control_command_t *iotdata_node_control_at(const uint8_t index) {
    if (index < IOTDATA_NODE_CONTROL_COMMANDS_COUNT)
        return &iotdata_node_control_commands[index];
    const uint8_t i = (uint8_t)(index - IOTDATA_NODE_CONTROL_COMMANDS_COUNT);
    return (i < _iotdata_node_control_app_count) ? &_iotdata_node_control_app[i] : NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// PACKAGING
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Build the CONTROL payload for one command. Returns false when nothing was added. */
static inline bool iotdata_node_control_build(iotdata_kvr_t *const kv, const iotdata_node_control_command_t *const c, const iotdata_node_control_args_t *const a) {
    if (kv == NULL || c == NULL || a == NULL)
        return false;
    static uint8_t v[IOTDATA_TLV_VALUE_MAX]; // XXX
    uint8_t n = _iotdata_node_control_prefix(c, v);

    switch (c->arg) {
    case IOTDATA_NODE_CONTROL_ARG_NONE:
        break;
    case IOTDATA_NODE_CONTROL_ARG_FIXED:
        v[n++] = c->fixed;
        break;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER: {
        const bool status = (c->arg == IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS);
        const uint8_t from_request = status ? a->scope_status : a->scope_filter;
        const uint8_t want = (c->fixed == IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST) ? from_request : c->fixed;
        /*
         * A zero scope is omittable for STATUS and not for FILTERS, and the difference is in what
         * zero MEANS rather than in any declared width.
         *
         * An absent STATUS scope already means "the default groups", so sending an explicit zero
         * would say the same thing a second time and cost a byte. But FILTERS_SCOPE_ALL is 0x00
         * and is a real instruction -- "clear everything" -- so dropping it would turn a request
         * into a silence, and a reader would have to guess which was meant.
         */
        if (want != 0 || !status)
            v[n++] = want;
        break;
    }
    case IOTDATA_NODE_CONTROL_ARG_STATION:
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION:
        v[n++] = (uint8_t)(a->station >> 8);
        v[n++] = (uint8_t)a->station;
        v[n++] = (c->arg == IOTDATA_NODE_CONTROL_ARG_STATION) ? c->fixed : a->action;
        break;
    case IOTDATA_NODE_CONTROL_ARG_REPORTS:
        /* every report that can be asked for, which is now every system type: a node with nothing
           to say for one answers it empty, and empty is an answer */
        for (uint8_t type = 0; type < IOTDATA_NODE_TLV_SYSTEM_COUNT; type++)
            if (iotdata_node_tlv_is_reportable(type))
                iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_REQUEST, &type, 1);
        return !kv->overflow && kv->len > 0;
    default:
        return false;
    }
    return iotdata_kvr_add(kv, c->key, v, n);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE REPORT
//
// The other direction: not a command being sent to a node, but a node saying which commands it
// ACCEPTS. That answer is how a manager discovers a device instead of being configured with a list
// of what it ought to support, so it has to be true rather than aspirational -- a node that
// advertises a key it does not service sends whoever asked on a wild goose chase.
//
// EVERY node answers the same seven, so they are unconditional: the five reports the node layer
// builds -- version, variant, control, status, config -- plus DIAGNOSTICS and REBOOT.
//
// Those last two are universal by DEFINITION rather than by coincidence. A reboot means restarting
// whatever this node is: an MCU restarts itself, an application restarts its own process, and
// neither reboots the machine it happens to be running on. And a diagnostics request is always
// answerable, because "no records" is an answer -- the same rule VARIANT already follows, where a
// node with no telemetry sends the TLV empty rather than staying silent.
//
// What is left over is genuinely per-device, and it is little: whether this node is part of a mesh
// and therefore keeps the tables, and whatever commands the device adds of its own.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const uint8_t *actions; /* (subject, action) PAIRS: whatever else this device implements */
    uint8_t actions_count;  /* the number of PAIRS, not of bytes */
} iotdata_node_control_report_t;

static inline int iotdata_node_control_pack(iotdata_kvr_t *const kv, const iotdata_node_control_report_t *const r) {
    if (kv == NULL || r == NULL)
        return -1;
    /* The inventory is literally a list of the messages you could send: each entry is the VALUE of
       a REQUEST or a CONTROL, in the encoding it would take on the wire. That is more informative
       than the old list of key flags, which could say a node took diagnostics commands but never
       which -- and it needs no second format to describe the first. */
    for (uint8_t type = 0; type < IOTDATA_NODE_TLV_SYSTEM_COUNT; type++)
        if (iotdata_node_tlv_is_reportable(type))
            iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_REQUEST, &type, 1);
    static const uint8_t node_actions[] = { IOTDATA_NODE_ACTION_NODE_REBOOT, IOTDATA_NODE_ACTION_NODE_RESET };
    for (size_t i = 0; i < sizeof(node_actions) / sizeof(node_actions[0]); i++)
        iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_CONTROL, (uint8_t[2]){ IOTDATA_NODE_SUBJECT_NODE, node_actions[i] }, (uint8_t)2);
    /* whatever the device adds of its own, in the same (subject, action) shape */
    for (uint8_t i = 0; i < r->actions_count; i++)
        iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_CONTROL, r->actions + (size_t)i * 2u, 2);
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE WORDS AN ARGUMENT CAN BE
//
// Shared by every medium and by none of them in particular: a scope is spelled "mesh" whether it
// arrived as a JSON member or as a console token. They sit above the adapters, and outside the
// JSON guard, because a node built without JSON still has a console.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* A STATUS scope: "node", "mesh", "node,mesh". The words belong to the TLV they scope, so they are
   defined once in iotdata_node_status.h -- which this header therefore expects to have been
   included first -- and a request typed at a console spells them exactly as the JSON does. */
#define iotdata_node_control_scope_status(s) iotdata_node_status_scope_from_name(s)

static inline uint8_t iotdata_node_control_scope_filter(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "manual") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_MANUAL;
        if (strcmp(s, "auto") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_AUTO;
    }
    return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_ALL;
}

static inline uint8_t iotdata_node_control_action(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "block") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_BLOCK;
        if (strcmp(s, "allow") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_ALLOW;
    }
    return IOTDATA_NODE_CONTROL_MESH_FILTERS_NONE; /* "none", "remove", absent: no entry */
}

static inline bool _iotdata_node_control_is_station(const char *const s) {
    return s != NULL && s[0] >= '0' && s[0] <= '9';
}
static inline bool _iotdata_node_control_is_action(const char *const s) {
    return s != NULL && (strcmp(s, "none") == 0 || strcmp(s, "remove") == 0 || strcmp(s, "block") == 0 || strcmp(s, "allow") == 0);
}
static inline bool _iotdata_node_control_is_scope_filter(const char *const s) {
    return s != NULL && (strcmp(s, "all") == 0 || strcmp(s, "manual") == 0 || strcmp(s, "auto") == 0);
}
static inline bool _iotdata_node_control_is_scope_status(const char *const s) {
    return iotdata_node_status_scope_is_name(s);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: A CONSOLE
//
// argv joined with '-', longest match first: `mesh peers dump` and `mesh-peers-dump` are the same
// command, so an operator can type it either way and neither spelling is a second vocabulary. The
// longest match matters because the trailing words may be ARGUMENTS -- `mesh peers update 0x537
// remove` has to resolve to mesh-peers-update with two arguments, not fail looking for a command
// called mesh-peers-update-0x537-remove.
//
// Whatever is left after the name is read positionally: a station, then an action or scope. That
// is looser than JSON's named members, which is the nature of a console; what matters is that both
// end at the same iotdata_node_control_args_t and the same iotdata_node_control_build.
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdlib.h> /* strtol: both media parse a station id, JSON or not */

#define IOTDATA_NODE_CONTROL_ARGV_NAME_MAX 48

/* Read the words after the name, positionally, as the arguments THIS command takes. Returns false
   if they cannot all be consumed -- too many, too few, or a word that is not one of ours -- which
   is what makes the longest-match loop safe: a name that matches but cannot own its remainder is
   not the command that was typed. */
static inline bool _iotdata_node_control_argv_args(const iotdata_node_control_command_t *const c, char **const argv, const int first, const int argc, iotdata_node_control_args_t *const args) {
    const int n = argc - first;
    switch (c->arg) {
    case IOTDATA_NODE_CONTROL_ARG_NONE:
    case IOTDATA_NODE_CONTROL_ARG_FIXED:
    case IOTDATA_NODE_CONTROL_ARG_REPORTS:
        return n == 0;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_node_control_is_scope_status(argv[first]))
            return false;
        args->scope_status = iotdata_node_control_scope_status(argv[first]);
        return true;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_node_control_is_scope_filter(argv[first]))
            return false;
        args->scope_filter = iotdata_node_control_scope_filter(argv[first]);
        return true;
    case IOTDATA_NODE_CONTROL_ARG_STATION:
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION: {
        /* the action is optional and defaults to none, as it does in JSON, so that a station typed
           on its own means the same thing through either medium */
        const int most = (c->arg == IOTDATA_NODE_CONTROL_ARG_STATION_ACTION) ? 2 : 1;
        if (n < 1 || n > most || !_iotdata_node_control_is_station(argv[first]))
            return false;
        args->station = (uint16_t)(strtol(argv[first], NULL, 0) & 0x0FFF);
        if (n == 2) {
            if (!_iotdata_node_control_is_action(argv[first + 1]))
                return false;
            args->action = iotdata_node_control_action(argv[first + 1]);
        }
        return true;
    }
    default:
        return false;
    }
}

/* How a console SHOWS a command: the name with spaces for hyphens, and what may follow it. A
   console that writes its own version of this prints one that is subtly wrong the first time a row
   above changes, so both come from the table the parser reads. */
static inline const char *iotdata_node_control_argv_name(const iotdata_node_control_command_t *const c, char *const out, const size_t size) {
    size_t n = 0;
    if (out == NULL || size == 0)
        return "";
    if (c != NULL)
        for (const char *p = c->name; *p != '\0' && n + 1u < size; p++)
            out[n++] = (*p == '-') ? ' ' : *p;
    out[n] = '\0';
    return out;
}

static inline const char *iotdata_node_control_argv_help(const iotdata_node_control_command_t *const c) {
    switch ((c != NULL) ? c->arg : IOTDATA_NODE_CONTROL_ARG_NONE) {
    case IOTDATA_NODE_CONTROL_ARG_STATION:
        return " <station>";
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION:
        return " <station> [none|remove|block|allow]";
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER:
        return " [all|manual|auto]";
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
        return " [node|mesh]";
    default:
        return "";
    }
}

static inline const iotdata_node_control_command_t *iotdata_node_control_from_argv(const int argc, char **const argv, iotdata_node_control_args_t *const args, int *const consumed) {
    if (argv == NULL || args == NULL || argc <= 0)
        return NULL;
    *args = (iotdata_node_control_args_t){ 0 };
    args->target = args->targets[0] = IOTDATA_STATION_BROADCAST;
    args->targets_count = 1;
    args->scope_filter = IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_ALL;

    for (int words = argc; words >= 1; words--) {
        char name[IOTDATA_NODE_CONTROL_ARGV_NAME_MAX];
        size_t n = 0;
        bool fits = true;
        for (int i = 0; i < words && fits; i++) {
            const char *p2 = argv[i];
            if (i > 0 && n + 1u < sizeof(name))
                name[n++] = '-';
            for (; *p2 != '\0'; p2++) {
                if (n + 1u >= sizeof(name)) {
                    fits = false;
                    break;
                }
                name[n++] = *p2;
            }
        }
        if (fits) {
            name[n] = '\0';
            const iotdata_node_control_command_t *const c = iotdata_node_control_find(name);
            if (c != NULL)
                if (_iotdata_node_control_argv_args(c, argv, words, argc, args)) { /* the name matched but the rest cannot belong to it: keep shortening */
                    if (consumed != NULL)
                        *consumed = words;
                    return c;
                }
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: JSON
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

/* A station id, as a decimal or 0x string or a number, masked to the 12 bits the header carries. */
static inline uint16_t _iotdata_node_control_json_station(const cJSON *const j, const uint16_t absent) {
    if (j != NULL) {
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            if (strcmp(j->valuestring, "all") == 0 || strcmp(j->valuestring, "broadcast") == 0)
                return IOTDATA_STATION_BROADCAST;
            return (uint16_t)(strtol(j->valuestring, NULL, 0) & 0x0FFF);
        }
        if (cJSON_IsNumber(j))
            return (uint16_t)(j->valueint & 0x0FFF);
    }
    return absent;
}

static inline const char *_iotdata_node_control_json_str(const cJSON *const root, const char *const name) {
    const cJSON *const j = cJSON_GetObjectItem(root, name);
    return (cJSON_IsString(j) && j->valuestring != NULL) ? j->valuestring : NULL;
}

/* A request object -> the command and its arguments. Returns NULL when the object names no command
   this node knows, which the caller reports rather than guessing at. */
/* One station, or a list of them. Duplicates are dropped and a broadcast anywhere in the list
   collapses it to a single broadcast -- both because sending the same node the same command twice
   is airtime for nothing, and because the alternative is a caller having to reason about whether
   its list overlaps. Returns how many are in `out`, always at least one. */
static inline uint8_t _iotdata_node_control_json_targets(const cJSON *const j, uint16_t *const out, const uint8_t max, bool *const overflow) {
    uint8_t n = 0;
    *overflow = false;
    if (cJSON_IsArray(j)) {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, j) {
            const uint16_t st = _iotdata_node_control_json_station(e, IOTDATA_STATION_BROADCAST);
            if (st == IOTDATA_STATION_BROADCAST) {
                out[0] = IOTDATA_STATION_BROADCAST;
                return 1;
            }
            bool seen = false;
            for (uint8_t i = 0; i < n; i++)
                seen = seen || (out[i] == st);
            if (!seen) {
                if (n < max)
                    out[n++] = st;
                else
                    *overflow = true; /* silently dropping a station the caller named is not an option */
            }
        }
    }
    if (n == 0) { /* not a list, or an empty one: the single-target reading, broadcast when absent */
        out[0] = _iotdata_node_control_json_station(j, IOTDATA_STATION_BROADCAST);
        n = 1;
    }
    return n;
}

static inline const iotdata_node_control_command_t *iotdata_node_control_from_json(const cJSON *const root, iotdata_node_control_args_t *const args) {
    if (root == NULL || args == NULL)
        return NULL;
    /* absent target means BROADCAST -- "this command, to whoever hears it" -- while an absent
       station means none, because a mesh command with no subject has nothing to act on */
    args->targets_count = _iotdata_node_control_json_targets(cJSON_GetObjectItem(root, "target"), args->targets, IOTDATA_NODE_CONTROL_TARGETS_MAX, &args->targets_overflow);
    args->target = args->targets[0];
    args->station = _iotdata_node_control_json_station(cJSON_GetObjectItem(root, "station"), 0);
    const char *const scope = _iotdata_node_control_json_str(root, "scope");
    args->scope_status = iotdata_node_control_scope_status(scope);
    args->scope_filter = iotdata_node_control_scope_filter(scope);
    args->action = iotdata_node_control_action(_iotdata_node_control_json_str(root, "action"));
    return iotdata_node_control_find(_iotdata_node_control_json_str(root, "cmd"));
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTROL_H */
