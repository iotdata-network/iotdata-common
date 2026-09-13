#ifndef IOTDATA_NODE_CONTROL_H
#define IOTDATA_NODE_CONTROL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_control.h - driving a node: the command vocabulary, its packaging, and the
// translation to and from whatever medium the operator is using.
//
// FRAMEWORK, NOT PROTOCOL. iotdata_node.h says what a CONTROL key IS -- its number, its width, its
// meaning on the wire. This says what an operator CALLS it, how a request becomes that key, and
// how the pair survives a trip through JSON. Those are different jobs with different lifetimes: a
// key number is forever, a name is a user interface.
//
// AND IT BELONGS TO EVERY CALLER, not to whichever one happened to need it first. This vocabulary
// lived inside the MQTT gateway, which meant a serial console or a C control tool either grew its
// own words for the same commands or reached into the gateway for them. One node, driven two ways,
// must not need two sets of words -- which is also why the primary names are four letters:
// `vers`, `stat`, `diag` read the same typed at a console as sent over MQTT.
//
// A TABLE, so a medium is a thin adapter. Each medium fills an iotdata_control_args_t from
// whatever it has -- JSON members here, argv on a console later -- and the packaging is shared.
// Adding a medium should not touch the vocabulary, and adding a command should not touch a medium.
//
// A NODE PLUGS IN ITS OWN. iotdata_control_init() registers commands specific to one device (a
// TSA's "calibrate", say). The built-ins are searched FIRST, so an application cannot quietly
// shadow `boot` or `stat` and leave an operator holding a word that means something else here.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * THE MQTT MANAGEMENT CHANNEL. Two topics, suffixed onto whatever prefix a deployment uses: one
 * that requests arrive on, one that every answer goes to.
 *
 * They live here rather than in the gateway because they are not the gateway's: a control tool
 * publishing a request and a monitor subscribing to the answers have to agree on the same two
 * strings, and a second copy of a topic name is a second thing to get wrong. The response topic
 * carries more than control answers -- reports and diagnostics land there too -- but it is one
 * channel, and the alternative was a hodgepodge of topics per producer.
 */
#define IOTDATA_MQTT_MANAGE_TOPIC_REQ  "/manage/req"
#define IOTDATA_MQTT_MANAGE_TOPIC_RESP "/manage/resp"

/* What a command does with the request's arguments. The kind is a property of the COMMAND, not of
   the medium, which is what lets one table serve all of them. */
typedef enum {
    IOTDATA_CONTROL_ARG_NONE,           /* a flag: the key present, no value */
    IOTDATA_CONTROL_ARG_FIXED,          /* a u8 the command itself fixes (enable = 1, disable = 0) */
    IOTDATA_CONTROL_ARG_SCOPE_STATUS,   /* a u8 from "scope", read as STATUS_SCOPE_* (which groups) */
    IOTDATA_CONTROL_ARG_SCOPE_FILTER,   /* a u8 from "scope", read as FILTER_SCOPE_* (which entries) */
    IOTDATA_CONTROL_ARG_STATION,        /* { u16 station, u8 action } -- the action fixed by the command */
    IOTDATA_CONTROL_ARG_STATION_ACTION, /* { u16 station, u8 action } -- the action named in the request */
    IOTDATA_CONTROL_ARG_REPORTS,        /* not one key: every report that can be asked for */
} iotdata_control_arg_t;

#define IOTDATA_CONTROL_SCOPE_FROM_REQUEST 0xFF

typedef struct {
    const char *name;
    uint8_t key;
    iotdata_control_arg_t arg;
    uint8_t fixed; /* FIXED: the value. STATION: the action. SCOPE: an override, or FROM_REQUEST. */
} iotdata_control_command_t;

/* Everything a command might need, however the medium expressed it. One struct rather than a
   parameter list, because the next medium fills the same fields from argv. */
typedef struct {
    uint16_t target;      /* the node to drive; BROADCAST when unsaid */
    uint16_t station;     /* the station a mesh command acts ON, which is not the target */
    uint8_t scope_status; /* STATUS_SCOPE_* bits; 0 = every group */
    uint8_t scope_filter; /* FILTER_SCOPE_*; ALL when unsaid */
    uint8_t action;       /* FILTERS_BLOCK / _ALLOW / _NONE */
} iotdata_control_args_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE VOCABULARY
// -----------------------------------------------------------------------------------------------------------------------------------------

static const iotdata_control_command_t iotdata_control_commands[] = {

    /* --- the system TLVs, one request each ------------------------------------------------- */
    { "vers", IOTDATA_NODE_CONTROL_VERSION_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "vari", IOTDATA_NODE_CONTROL_VARIANT_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "ctrl", IOTDATA_NODE_CONTROL_CONTROL_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "stat", IOTDATA_NODE_CONTROL_STATUS_REQUEST, IOTDATA_CONTROL_ARG_SCOPE_STATUS, IOTDATA_CONTROL_SCOPE_FROM_REQUEST },
    { "conf", IOTDATA_NODE_CONTROL_CONFIG_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "diag", IOTDATA_NODE_CONTROL_DIAGNOSTICS_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "cont", IOTDATA_NODE_CONTROL_CONTENT_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "reports", 0, IOTDATA_CONTROL_ARG_REPORTS, 0 }, /* every report a node can produce, in type order */

    /* --- generic system control -------------------------------------------------------------- */
    { "boot", IOTDATA_NODE_CONTROL_REBOOT, IOTDATA_CONTROL_ARG_NONE, 0 }, /* `boot` on the serial CLI too */

    /* --- the recorder ------------------------------------------------------------------------ */
    { "diag-enable", IOTDATA_NODE_CONTROL_DIAGNOSTICS_ENABLE, IOTDATA_CONTROL_ARG_FIXED, 1 },
    { "diag-disable", IOTDATA_NODE_CONTROL_DIAGNOSTICS_ENABLE, IOTDATA_CONTROL_ARG_FIXED, 0 },
    { "diag-clear", IOTDATA_NODE_CONTROL_DIAGNOSTICS_CLEAR, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "diag-dump", IOTDATA_NODE_CONTROL_DIAGNOSTICS_DUMP, IOTDATA_CONTROL_ARG_NONE, 0 },

    /* --- mesh management --------------------------------------------------------------------- */
    { "mesh-stations", IOTDATA_NODE_CONTROL_MESH_STATIONS_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-stations-dump", IOTDATA_NODE_CONTROL_MESH_STATIONS_DUMP, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-peers", IOTDATA_NODE_CONTROL_MESH_PEERS_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-peers-update", IOTDATA_NODE_CONTROL_MESH_PEERS_UPDATE, IOTDATA_CONTROL_ARG_STATION_ACTION, 0 },
    { "mesh-peers-clear", IOTDATA_NODE_CONTROL_MESH_PEERS_CLEAR, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-peers-dump", IOTDATA_NODE_CONTROL_MESH_PEERS_DUMP, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-filters", IOTDATA_NODE_CONTROL_MESH_FILTERS_REQUEST, IOTDATA_CONTROL_ARG_NONE, 0 },
    { "mesh-filters-update", IOTDATA_NODE_CONTROL_MESH_FILTERS_UPDATE, IOTDATA_CONTROL_ARG_STATION_ACTION, 0 },
    { "mesh-filters-clear", IOTDATA_NODE_CONTROL_MESH_FILTERS_CLEAR, IOTDATA_CONTROL_ARG_SCOPE_FILTER, IOTDATA_CONTROL_SCOPE_FROM_REQUEST },
    { "mesh-filters-dump", IOTDATA_NODE_CONTROL_MESH_FILTERS_DUMP, IOTDATA_CONTROL_ARG_NONE, 0 },
};

#define IOTDATA_CONTROL_COMMANDS_COUNT ((uint8_t)(sizeof(iotdata_control_commands) / sizeof(iotdata_control_commands[0])))

/* Commands one device adds to the common set -- see iotdata_control_init. */
static const iotdata_control_command_t *_iotdata_control_app = NULL;
static uint8_t _iotdata_control_app_count = 0;

static inline void iotdata_control_init(const iotdata_control_command_t *const app, const uint8_t count) {
    _iotdata_control_app = app;
    _iotdata_control_app_count = (app != NULL) ? count : 0;
}

/* Built-ins first, deliberately: an application must not be able to redefine `boot` and leave an
   operator's word meaning something else on one node than on every other. */
static inline const iotdata_control_command_t *iotdata_control_find(const char *const name) {
    if (name == NULL)
        return NULL;
    for (uint8_t i = 0; i < IOTDATA_CONTROL_COMMANDS_COUNT; i++)
        if (strcmp(iotdata_control_commands[i].name, name) == 0)
            return &iotdata_control_commands[i];
    for (uint8_t i = 0; i < _iotdata_control_app_count; i++)
        if (strcmp(_iotdata_control_app[i].name, name) == 0)
            return &_iotdata_control_app[i];
    return NULL;
}

/* Walk every name, built-in then application, so a medium can offer help or completion without
   knowing where a command came from. Returns NULL past the end. */
static inline const iotdata_control_command_t *iotdata_control_at(const uint8_t index) {
    if (index < IOTDATA_CONTROL_COMMANDS_COUNT)
        return &iotdata_control_commands[index];
    const uint8_t i = (uint8_t)(index - IOTDATA_CONTROL_COMMANDS_COUNT);
    return (i < _iotdata_control_app_count) ? &_iotdata_control_app[i] : NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// PACKAGING
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool _iotdata_control_mesh_update(iotdata_kvr_t *const kv, const uint8_t key, const uint16_t station, const uint8_t action) {
    const uint8_t e[IOTDATA_NODE_CONTROL_MESH_UPDATE_ENTRY_SIZE] = { (uint8_t)(station >> 8), (uint8_t)station, action };
    return iotdata_kvr_add(kv, key, e, (uint8_t)sizeof(e));
}

/* Build the CONTROL payload for one command. Returns false when nothing was added. */
static inline bool iotdata_control_build(iotdata_kvr_t *const kv, const iotdata_control_command_t *const c, const iotdata_control_args_t *const a) {
    if (kv == NULL || c == NULL || a == NULL)
        return false;
    switch (c->arg) {
    case IOTDATA_CONTROL_ARG_NONE:
        return iotdata_kvr_add_flag(kv, c->key);
    case IOTDATA_CONTROL_ARG_FIXED:
        return iotdata_kvr_add_u8(kv, c->key, c->fixed);
    case IOTDATA_CONTROL_ARG_SCOPE_STATUS:
    case IOTDATA_CONTROL_ARG_SCOPE_FILTER: {
        const uint8_t from_request = (c->arg == IOTDATA_CONTROL_ARG_SCOPE_STATUS) ? a->scope_status : a->scope_filter;
        const uint8_t want = (c->fixed == IOTDATA_CONTROL_SCOPE_FROM_REQUEST) ? from_request : c->fixed;
        /*
         * A zero scope may or may not be omittable, and the key table already says which.
         *
         * STATUS_REQUEST is declared WIDTH_VARIABLE -- its value is optional, and absent already
         * means "every group", so sending an explicit zero would express that a second way and
         * cost a byte. MESH_FILTERS_CLEAR declares a width of 1: it must carry its byte even when
         * the byte is zero, or the frame contradicts the table and a strict reader is entitled to
         * reject it. So the declared width decides, rather than an assumption about zero.
         */
        if (want == 0 && iotdata_node_tlv_key_width(IOTDATA_NODE_TLV_CONTROL, c->key) == IOTDATA_NODE_WIDTH_VARIABLE)
            return iotdata_kvr_add_flag(kv, c->key);
        return iotdata_kvr_add_u8(kv, c->key, want);
    }
    case IOTDATA_CONTROL_ARG_STATION:
        return _iotdata_control_mesh_update(kv, c->key, a->station, c->fixed);
    case IOTDATA_CONTROL_ARG_STATION_ACTION:
        return _iotdata_control_mesh_update(kv, c->key, a->station, a->action);
    case IOTDATA_CONTROL_ARG_REPORTS:
        /* everything that can be asked for -- so not CONTENT (nothing implements it) and not
           RECEIVE (a node advertises that, it is not requestable) */
        for (uint8_t type = 0; type <= IOTDATA_TLV_TYPE_SYSTEM_MAX; type++)
            if (iotdata_node_tlv_control_key(type) != IOTDATA_NODE_TLV_NONE && type != IOTDATA_NODE_TLV_CONTENT)
                iotdata_kvr_add_flag(kv, iotdata_node_tlv_control_key(type));
        return !kv->overflow && kv->len > 0;
    default:
        return false;
    }
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
    bool tables;         /* keeps the mesh tables, so their requests are answerable too */
    const uint8_t *keys; /* whatever else this device implements, advertised as given */
    uint8_t keys_count;
} iotdata_control_report_t;

static inline int iotdata_control_pack(iotdata_kvr_t *const kv, const iotdata_control_report_t *const r) {
    if (kv == NULL || r == NULL)
        return -1;
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_VERSION_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_VARIANT_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_CONTROL_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_STATUS_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_CONFIG_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_DIAGNOSTICS_REQUEST);
    iotdata_kvr_add_flag(kv, IOTDATA_NODE_CONTROL_REBOOT);
    /* the capability, not the contents: a table is advertised whether or not it happens to be
       empty right now, because "I keep peers" and "I have no peers today" are different answers */
    if (r->tables)
        for (uint8_t type = IOTDATA_NODE_TLV_MESH_STATIONS; type <= IOTDATA_NODE_TLV_MESH_FILTERS; type++)
            iotdata_kvr_add_flag(kv, iotdata_node_tlv_control_key(type));
    for (uint8_t i = 0; i < r->keys_count; i++)
        iotdata_kvr_add_flag(kv, r->keys[i]);
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
#define iotdata_control_scope_status(s) iotdata_status_scope_from_name(s)

static inline uint8_t iotdata_control_scope_filter(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "manual") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_MANUAL;
        if (strcmp(s, "auto") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_AUTO;
    }
    return IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_ALL;
}

static inline uint8_t iotdata_control_action(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "block") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_BLOCK;
        if (strcmp(s, "allow") == 0)
            return IOTDATA_NODE_CONTROL_MESH_FILTERS_ALLOW;
    }
    return IOTDATA_NODE_CONTROL_MESH_FILTERS_NONE; /* "none", "remove", absent: no entry */
}

/* Whether a word IS one of the above, which is a different question from what it parses to. The
   parsers are deliberately lenient -- an unrecognised scope means "every group" -- and that is
   right for JSON, where the member name already said what the word was for. On a console nothing
   says it: a trailing word that means nothing to us is far more likely a mistyped command than an
   argument, and shrugging it off is how `mesh peers remove 0537` silently becomes `mesh peers`,
   a REQUEST, with the removal quietly dropped. So the console asks these first. */
static inline bool _iotdata_control_is_station(const char *const s) {
    return s != NULL && s[0] >= '0' && s[0] <= '9';
}
static inline bool _iotdata_control_is_action(const char *const s) {
    return s != NULL && (strcmp(s, "none") == 0 || strcmp(s, "remove") == 0 || strcmp(s, "block") == 0 || strcmp(s, "allow") == 0);
}
static inline bool _iotdata_control_is_scope_filter(const char *const s) {
    return s != NULL && (strcmp(s, "all") == 0 || strcmp(s, "manual") == 0 || strcmp(s, "auto") == 0);
}
static inline bool _iotdata_control_is_scope_status(const char *const s) {
    return iotdata_status_scope_is_name(s);
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
// end at the same iotdata_control_args_t and the same iotdata_control_build.
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdlib.h> /* strtol: both media parse a station id, JSON or not */

#define IOTDATA_CONTROL_ARGV_NAME_MAX 48

/* Read the words after the name, positionally, as the arguments THIS command takes. Returns false
   if they cannot all be consumed -- too many, too few, or a word that is not one of ours -- which
   is what makes the longest-match loop safe: a name that matches but cannot own its remainder is
   not the command that was typed. */
static inline bool _iotdata_control_argv_args(const iotdata_control_command_t *const c, char **const argv, const int first, const int argc, iotdata_control_args_t *const args) {
    const int n = argc - first;
    switch (c->arg) {
    case IOTDATA_CONTROL_ARG_NONE:
    case IOTDATA_CONTROL_ARG_FIXED:
    case IOTDATA_CONTROL_ARG_REPORTS:
        return n == 0;
    case IOTDATA_CONTROL_ARG_SCOPE_STATUS:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_control_is_scope_status(argv[first]))
            return false;
        args->scope_status = iotdata_control_scope_status(argv[first]);
        return true;
    case IOTDATA_CONTROL_ARG_SCOPE_FILTER:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_control_is_scope_filter(argv[first]))
            return false;
        args->scope_filter = iotdata_control_scope_filter(argv[first]);
        return true;
    case IOTDATA_CONTROL_ARG_STATION:
    case IOTDATA_CONTROL_ARG_STATION_ACTION: {
        /* the action is optional and defaults to none, as it does in JSON, so that a station typed
           on its own means the same thing through either medium */
        const int most = (c->arg == IOTDATA_CONTROL_ARG_STATION_ACTION) ? 2 : 1;
        if (n < 1 || n > most || !_iotdata_control_is_station(argv[first]))
            return false;
        args->station = (uint16_t)(strtol(argv[first], NULL, 0) & 0x0FFF);
        if (n == 2) {
            if (!_iotdata_control_is_action(argv[first + 1]))
                return false;
            args->action = iotdata_control_action(argv[first + 1]);
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
static inline const char *iotdata_control_argv_name(const iotdata_control_command_t *const c, char *const out, const size_t size) {
    size_t n = 0;
    if (out == NULL || size == 0)
        return "";
    if (c != NULL)
        for (const char *p = c->name; *p != '\0' && n + 1u < size; p++)
            out[n++] = (*p == '-') ? ' ' : *p;
    out[n] = '\0';
    return out;
}

static inline const char *iotdata_control_argv_help(const iotdata_control_command_t *const c) {
    switch ((c != NULL) ? c->arg : IOTDATA_CONTROL_ARG_NONE) {
    case IOTDATA_CONTROL_ARG_STATION:
        return " <station>";
    case IOTDATA_CONTROL_ARG_STATION_ACTION:
        return " <station> [none|remove|block|allow]";
    case IOTDATA_CONTROL_ARG_SCOPE_FILTER:
        return " [all|manual|auto]";
    case IOTDATA_CONTROL_ARG_SCOPE_STATUS:
        return " [node|mesh]";
    default:
        return "";
    }
}

static inline const iotdata_control_command_t *iotdata_control_from_argv(const int argc, char **const argv, iotdata_control_args_t *const args, int *const consumed) {
    if (argv == NULL || args == NULL || argc <= 0)
        return NULL;
    memset(args, 0, sizeof(*args));
    args->target = IOTDATA_STATION_BROADCAST;
    args->scope_filter = IOTDATA_NODE_CONTROL_MESH_FILTERS_SCOPE_ALL;

    for (int words = argc; words >= 1; words--) {
        char name[IOTDATA_CONTROL_ARGV_NAME_MAX];
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
        if (!fits)
            continue;
        name[n] = '\0';
        const iotdata_control_command_t *const c = iotdata_control_find(name);
        if (c == NULL)
            continue;
        if (!_iotdata_control_argv_args(c, argv, words, argc, args))
            continue; /* the name matched but the rest cannot belong to it: keep shortening */
        if (consumed != NULL)
            *consumed = words;
        return c;
    }
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: JSON
//
// The first adapter. A second one -- a serial console taking argv -- fills the same
// iotdata_control_args_t and calls the same iotdata_control_build, which is the point of both
// existing.
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

/* A station id, as a decimal or 0x string or a number, masked to the 12 bits the header carries. */
static inline uint16_t _iotdata_control_json_station(const cJSON *const j, const uint16_t absent) {
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

static inline const char *_iotdata_control_json_str(const cJSON *const root, const char *const name) {
    const cJSON *const j = cJSON_GetObjectItem(root, name);
    return (cJSON_IsString(j) && j->valuestring != NULL) ? j->valuestring : NULL;
}

/* A request object -> the command and its arguments. Returns NULL when the object names no command
   this node knows, which the caller reports rather than guessing at. */
static inline const iotdata_control_command_t *iotdata_control_from_json(const cJSON *const root, iotdata_control_args_t *const args) {
    if (root == NULL || args == NULL)
        return NULL;
    /* absent target means BROADCAST -- "this command, to whoever hears it" -- while an absent
       station means none, because a mesh command with no subject has nothing to act on */
    args->target = _iotdata_control_json_station(cJSON_GetObjectItem(root, "target"), IOTDATA_STATION_BROADCAST);
    args->station = _iotdata_control_json_station(cJSON_GetObjectItem(root, "station"), 0);
    const char *const scope = _iotdata_control_json_str(root, "scope");
    args->scope_status = iotdata_control_scope_status(scope);
    args->scope_filter = iotdata_control_scope_filter(scope);
    args->action = iotdata_control_action(_iotdata_control_json_str(root, "action"));
    return iotdata_control_find(_iotdata_control_json_str(root, "cmd"));
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTROL_H */
