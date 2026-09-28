#ifndef IOTDATA_NODE_MESH_TUNING_H
#define IOTDATA_NODE_MESH_TUNING_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_MESH_ENABLE
#define IOTDATA_CONFIG_MESH_ENABLE 1
#endif
#ifndef IOTDATA_CONFIG_MESH_DEBUG
#define IOTDATA_CONFIG_MESH_DEBUG 0
#endif
#ifndef IOTDATA_CONFIG_MESH_PEER_TTL_MS
#define IOTDATA_CONFIG_MESH_PEER_TTL_MS 300000u /* ~5x a 60s beacon */
#endif
#ifndef IOTDATA_CONFIG_MESH_TTL_INIT
#define IOTDATA_CONFIG_MESH_TTL_INIT IOTDATA_MESH_TTL_DEFAULT
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* How often a relay tells the gateway what it can hear. Observational -- a topology view, not an
   operational path -- so slow is fine and cheap is better. */
#ifndef IOTDATA_CONFIG_MESH_REPORT_PEERS_MS
#define IOTDATA_CONFIG_MESH_REPORT_PEERS_MS 120000u
#endif
#ifndef IOTDATA_CONFIG_MESH_PARENT_TIMEOUT_MS
#define IOTDATA_CONFIG_MESH_PARENT_TIMEOUT_MS 190000u /* ~3 missed 60s beacon rounds */
#endif
#ifndef IOTDATA_CONFIG_MESH_HYSTERESIS_DB
#define IOTDATA_CONFIG_MESH_HYSTERESIS_DB 10
#endif
#ifndef IOTDATA_CONFIG_MESH_REBROADCAST_JITTER_MIN_MS
#define IOTDATA_CONFIG_MESH_REBROADCAST_JITTER_MIN_MS 1000u
#endif
#ifndef IOTDATA_CONFIG_MESH_REBROADCAST_JITTER_MAX_MS
#define IOTDATA_CONFIG_MESH_REBROADCAST_JITTER_MAX_MS 5000u
#endif
#ifndef IOTDATA_CONFIG_MESH_BEACON_EXPIRY_MS
#define IOTDATA_CONFIG_MESH_BEACON_EXPIRY_MS 8000u
#endif
#ifndef IOTDATA_CONFIG_MESH_RERR_EXPIRY_MS
#define IOTDATA_CONFIG_MESH_RERR_EXPIRY_MS 8000u
#endif
#ifndef IOTDATA_CONFIG_MESH_FORWARD_SEEN_TTL_MS
#define IOTDATA_CONFIG_MESH_FORWARD_SEEN_TTL_MS 8000u
#endif
#ifndef IOTDATA_CONFIG_MESH_FORWARD_SUPPRESS
#define IOTDATA_CONFIG_MESH_FORWARD_SUPPRESS 0 /* 0=ack 1=lower-cost 2=always */
#endif
#ifndef IOTDATA_CONFIG_MESH_FORWARD_BACKOFF_MIN_MS
#define IOTDATA_CONFIG_MESH_FORWARD_BACKOFF_MIN_MS 200u
#endif
#ifndef IOTDATA_CONFIG_MESH_FORWARD_BACKOFF_MAX_MS
#define IOTDATA_CONFIG_MESH_FORWARD_BACKOFF_MAX_MS 1000u
#endif
#ifndef IOTDATA_CONFIG_MESH_FORWARD_EXPIRY_MS
#define IOTDATA_CONFIG_MESH_FORWARD_EXPIRY_MS 3000u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_MESH_BEACON_INTERVAL_S
#define IOTDATA_CONFIG_MESH_BEACON_INTERVAL_S 60u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_CONFIG_MESH_ACK_MAX_RETRIES
#define IOTDATA_CONFIG_MESH_ACK_MAX_RETRIES 3 /* 0 = best-effort: no pending table, no retry */
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_TIMEOUT_MS
#define IOTDATA_CONFIG_MESH_ACK_TIMEOUT_MS 3000u
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_EVICT
#define IOTDATA_CONFIG_MESH_ACK_EVICT 0 /* 0=oldest 1=oldest-of-station */
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_STATION_MAX
#define IOTDATA_CONFIG_MESH_ACK_STATION_MAX 0 /* 0 = no per-station cap */
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_BACKOFF
#define IOTDATA_CONFIG_MESH_ACK_BACKOFF 0 /* 0=linear 1=scaled */
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_BACKOFF_FACTOR_PCT
#define IOTDATA_CONFIG_MESH_ACK_BACKOFF_FACTOR_PCT 200u /* x2 per attempt, when scaled */
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_BACKOFF_MAX_MS
#define IOTDATA_CONFIG_MESH_ACK_BACKOFF_MAX_MS 30000u
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_REQUEUE_MS
#define IOTDATA_CONFIG_MESH_ACK_REQUEUE_MS 500u
#endif
#ifndef IOTDATA_CONFIG_MESH_ACK_PARK_MS
#define IOTDATA_CONFIG_MESH_ACK_PARK_MS 30000u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_MESH_TUNING_H */
