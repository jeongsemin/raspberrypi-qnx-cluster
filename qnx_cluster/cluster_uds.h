/* UDS (ISO 14229-1) services of the cluster: 0x10, 0x3E, 0x22, 0x19 (01, 02, 04), 0x14, 0x27, 0x11.
 *
 * NOT a conformant UDS server: it implements the services below the way the standard describes them,
 * but P2 is announced, not enforced. The session timer S3 is there: the extended session ends after
 * UDS_S3_MS without a request. The "response pending" NRC 0x78 is sent by the caller (cluster_diag.c)
 * around the slow part, the writing of the DTC file.
 *
 * uds_handle() takes one request and builds the response; how the bytes got here (one CAN frame or many,
 * cluster_isotp.h) does not matter. It knows nothing about CAN, sockets or QNX, so it can be unit tested
 * on the host (test_uds.c).
 *
 * Addressing: 0x7E0 is the cluster alone (physical), 0x7DF is every ECU (functional). A functional request
 * gets no negative response for "service / sub-function / request out of range": with many ECUs on the
 * bus only the ones that support the request should answer. */
#ifndef CLUSTER_UDS_H
#define CLUSTER_UDS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cluster_dem.h"
#include "cluster_isotp.h"

#define UDS_PORT 5556        /* UDP port that carries the virtual CAN frames of the diagnostics */
#define UDS_ID_REQ_PHYS 0x7E0
#define UDS_ID_REQ_FUNC 0x7DF
#define UDS_ID_RESP 0x7E8

#define UDS_SESSION_DEFAULT 0x01
#define UDS_SESSION_EXTENDED 0x03
#define UDS_P2_MS 50          /* sent in the response of 0x10; not enforced */
#define UDS_S3_MS 5000        /* the extended session ends when no request came for this long */
#define UDS_P2STAR_10MS 500   /* 5000 ms in units of 10 ms */

#define UDS_SID_SESSION 0x10
#define UDS_SID_ECU_RESET 0x11
#define UDS_SID_SECURITY 0x27
#define UDS_SID_CLEAR_DTC 0x14
#define UDS_SID_READ_DTC 0x19
#define UDS_SID_READ_DID 0x22
#define UDS_SID_TESTER_PRESENT 0x3E
#define UDS_SID_NEGATIVE 0x7F

#define NRC_SERVICE_NOT_SUPPORTED 0x11
#define NRC_SUBFUNCTION_NOT_SUPPORTED 0x12
#define NRC_INCORRECT_LENGTH 0x13
#define NRC_RESPONSE_TOO_LONG 0x14
#define NRC_CONDITIONS_NOT_CORRECT 0x22
#define NRC_REQUEST_SEQUENCE_ERROR 0x24
#define NRC_REQUEST_OUT_OF_RANGE 0x31
#define NRC_SECURITY_ACCESS_DENIED 0x33
#define NRC_INVALID_KEY 0x35
#define NRC_EXCEEDED_ATTEMPTS 0x36
#define NRC_DELAY_NOT_EXPIRED 0x37
#define NRC_RESPONSE_PENDING 0x78
#define NRC_SERVICE_NOT_IN_SESSION 0x7F

#define UDS_DTC_AVAILABILITY_MASK 0x2F  /* the status bits cluster_dem.h knows: 0x01 0x02 0x04 0x08 0x20 */
#define UDS_DTC_FORMAT_14229_1 0x01

#define UDS_SEC_MAX_ATTEMPTS 3   /* wrong keys in a row before the lock */
#define UDS_SEC_LOCK_MS 10000    /* how long it stays locked */

#define UDS_VEHICLE_MAX_AGE_MS 1000   /* the live vehicle data (DID 0200) must be at most this old */
#define UDS_DID_VEHICLE 0x0200   /* the vehicle data of the moment: 10 bytes, also the content of a DTC snapshot */
#define UDS_DID_UPTIME 0x0100    /* seconds since cluster_diag started, 4 bytes */
#define UDS_DID_SESSION 0xF186   /* active diagnostic session, 1 byte */
#define UDS_DID_VIN 0xF190       /* 17 characters (made up): the answer needs several CAN frames */
#define UDS_DID_SW_VERSION 0xF195

#define UDS_VIN "QNXCLUSTER0000001"
#define UDS_SW_VERSION "1.0"

typedef struct {
    uint8_t session;       /* UDS_SESSION_* */
    uint32_t s3_deadline;  /* ms; in a non-default session the time the session ends if no request comes */
    uint8_t unlocked;      /* security access granted (until the session changes) */
    uint8_t seed_sent;     /* a seed was given out and its key is awaited */
    uint32_t seed;
    uint32_t seed_counter; /* makes every seed different */
    uint8_t attempts;      /* wrong keys in a row */
    uint8_t locked;        /* too many wrong keys: no seed until lock_until */
    uint32_t lock_until;
    uint8_t reset_request; /* an ECU reset was accepted: the caller restarts the program after the response */
} uds_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t now_ms;       /* a millisecond clock (may wrap) */
} uds_env_t;

static inline void uds_init(uds_t *u)
{
    memset(u, 0, sizeof *u);
    u->session = UDS_SESSION_DEFAULT;
}

/* The key that belongs to a seed. A made-up algorithm for the practice (anyone who reads this file knows it);
 * a real ECU keeps its algorithm secret and uses a cryptographic function. */
static inline uint32_t uds_key(uint32_t seed)
{
    const uint32_t x = seed ^ 0xA5C3F00Du;
    return ((x << 7) | (x >> 25)) + 0x1234ABCDu;
}

/* A new session (or none) locks the security access again. */
static inline void uds_lock(uds_t *u)
{
    u->unlocked = 0;
    u->seed_sent = 0;
}

/* Call it regularly. Returns 1 when the S3 time ran out and the session went back to the default one. */
static inline int uds_tick(uds_t *u, uint32_t now_ms)
{
    if (u->session != UDS_SESSION_DEFAULT && isotp_due(now_ms, u->s3_deadline)) {
        u->session = UDS_SESSION_DEFAULT;
        uds_lock(u);
        return 1;
    }
    return 0;
}

static inline size_t uds_nrc(uint8_t *r, uint8_t sid, uint8_t nrc)
{
    r[0] = UDS_SID_NEGATIVE;
    r[1] = sid;
    r[2] = nrc;
    return 3;
}

/* The three bytes of a DTC as UDS sends them: the two bytes of SAE J2012 and the fault type (0 here). */
static inline void uds_dtc_bytes(int ev, uint8_t out[3])
{
    out[0] = (uint8_t)(dem_info[ev].dtc >> 8);
    out[1] = (uint8_t)(dem_info[ev].dtc & 0xFF);
    out[2] = 0;
}

/* Appends the data of one DID to r. Returns the number of bytes, or -1 for an unknown DID. */
/* The 10 bytes of the vehicle data: speed (2, 0.1 km/h), rpm (2), coolant (1, degC+40), fuel (1, 0.5 %), gear (1),
 * mode (1), warnings (2). All big endian. */
static inline void uds_vehicle_bytes(const dem_vehicle_t *v, uint8_t *r)
{
    r[0] = (uint8_t)(v->speed >> 8); r[1] = (uint8_t)v->speed;
    r[2] = (uint8_t)(v->rpm >> 8); r[3] = (uint8_t)v->rpm;
    r[4] = v->coolant; r[5] = v->fuel; r[6] = v->gear; r[7] = v->mode;
    r[8] = (uint8_t)(v->warnings >> 8); r[9] = (uint8_t)v->warnings;
}

static inline int uds_did_data(const uds_t *u, const uds_env_t *env, const dem_t *dem, uint16_t did, uint8_t *r)
{
    switch (did) {
    case UDS_DID_VEHICLE:
        if (!dem_vehicle_fresh(dem, env->now_ms, UDS_VEHICLE_MAX_AGE_MS)) return -2;   /* no data, or old */
        uds_vehicle_bytes(&dem->vehicle, r);
        return 10;
    case UDS_DID_SESSION:
        r[0] = u->session;
        return 1;
    case UDS_DID_UPTIME:
        r[0] = (uint8_t)(env->uptime_s >> 24);
        r[1] = (uint8_t)(env->uptime_s >> 16);
        r[2] = (uint8_t)(env->uptime_s >> 8);
        r[3] = (uint8_t)env->uptime_s;
        return 4;
    case UDS_DID_SW_VERSION:
        memcpy(r, UDS_SW_VERSION, sizeof UDS_SW_VERSION - 1);
        return (int)sizeof UDS_SW_VERSION - 1;
    case UDS_DID_VIN:
        memcpy(r, UDS_VIN, sizeof UDS_VIN - 1);
        return (int)sizeof UDS_VIN - 1;
    default:
        return -1;
    }
}

/* One request -> the response in resp (at least ISOTP_BUF bytes). Returns the response length, 0 = send nothing. */
static inline size_t uds_handle(uds_t *u, dem_t *dem, const uds_env_t *env, int functional, const uint8_t *req,
                                size_t n, uint8_t *resp)
{
    uint8_t out[ISOTP_BUF];
    size_t rn = 0;
    int spr = 0;   /* suppressPosRspMsgIndicationBit of the sub-function */
    if (n == 0) return 0;
    const uint8_t sid = req[0];
    if (u->session != UDS_SESSION_DEFAULT) u->s3_deadline = env->now_ms + UDS_S3_MS;   /* every request restarts S3 */

    switch (sid) {
    case UDS_SID_SESSION: {
        if (n != 2) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t sub = req[1] & 0x7F;
        spr = req[1] >> 7;
        if (sub != UDS_SESSION_DEFAULT && sub != UDS_SESSION_EXTENDED) { rn = uds_nrc(out, sid, NRC_SUBFUNCTION_NOT_SUPPORTED); break; }
        u->session = sub;
        uds_lock(u);                                   /* a new session: the security access is locked again */
        u->s3_deadline = env->now_ms + UDS_S3_MS;
        out[0] = sid + 0x40; out[1] = sub;
        out[2] = UDS_P2_MS >> 8; out[3] = UDS_P2_MS & 0xFF;
        out[4] = UDS_P2STAR_10MS >> 8; out[5] = UDS_P2STAR_10MS & 0xFF;
        rn = 6;
        break;
    }
    case UDS_SID_TESTER_PRESENT: {
        if (n != 2) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t sub = req[1] & 0x7F;
        spr = req[1] >> 7;
        if (sub != 0) { rn = uds_nrc(out, sid, NRC_SUBFUNCTION_NOT_SUPPORTED); break; }
        out[0] = sid + 0x40; out[1] = 0;
        rn = 2;
        break;
    }
    case UDS_SID_READ_DID: {
        if (n < 3 || ((n - 1) % 2) != 0) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        out[0] = sid + 0x40;
        rn = 1;
        int bad = 0;
        for (size_t i = 1; i + 1 < n && !bad; i += 2) {
            const uint16_t did = (uint16_t)((req[i] << 8) | req[i + 1]);
            uint8_t data[32];
            const int dn = uds_did_data(u, env, dem, did, data);
            if (dn == -2) { bad = 3; break; }
            if (dn < 0) { bad = 1; break; }
            if (rn + 2 + (size_t)dn > sizeof out) { bad = 2; break; }   /* more than one message can hold */
            out[rn++] = req[i];
            out[rn++] = req[i + 1];
            memcpy(out + rn, data, (size_t)dn);
            rn += (size_t)dn;
        }
        if (bad == 1) rn = uds_nrc(out, sid, NRC_REQUEST_OUT_OF_RANGE);
        else if (bad == 2) rn = uds_nrc(out, sid, NRC_RESPONSE_TOO_LONG);
        else if (bad == 3) rn = uds_nrc(out, sid, NRC_CONDITIONS_NOT_CORRECT);
        break;
    }
    case UDS_SID_READ_DTC: {
        if (n < 2) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t sub = req[1] & 0x7F;
        spr = req[1] >> 7;
        if (sub != 0x01 && sub != 0x02 && sub != 0x04) { rn = uds_nrc(out, sid, NRC_SUBFUNCTION_NOT_SUPPORTED); break; }
        if (sub == 0x04) {   /* reportDTCSnapshotRecordByDTCNumber: 19 04 <DTC 3 bytes> <record number> */
            if (n != 6) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
            int found = -1;
            for (int i = 0; i < DEM_EVENTS; i++) {
                uint8_t b[3];
                uds_dtc_bytes(i, b);
                if (b[0] == req[2] && b[1] == req[3] && b[2] == req[4]) found = i;
            }
            const uint8_t record = req[5];
            if (found < 0 || (record != 0x01 && record != 0xFF)) { rn = uds_nrc(out, sid, NRC_REQUEST_OUT_OF_RANGE); break; }
            out[0] = sid + 0x40; out[1] = sub;
            uds_dtc_bytes(found, out + 2);
            out[5] = dem->e[found].status;
            rn = 6;
            if (dem->e[found].status && dem_vehicle_valid(&dem->e[found].snap)) {   /* record 1 = the moment the failure began */
                out[rn++] = 0x01;                       /* record number */
                out[rn++] = 0x01;                       /* number of data identifiers in it */
                out[rn++] = UDS_DID_VEHICLE >> 8;
                out[rn++] = UDS_DID_VEHICLE & 0xFF;
                uds_vehicle_bytes(&dem->e[found].snap, out + rn);
                rn += 10;
            }
            break;
        }
        if (n != 3) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t mask = req[2];
        out[0] = sid + 0x40; out[1] = sub; out[2] = UDS_DTC_AVAILABILITY_MASK;
        if (sub == 0x01) {
            unsigned count = 0;
            for (int i = 0; i < DEM_EVENTS; i++) if (dem->e[i].status & mask) count++;
            out[3] = UDS_DTC_FORMAT_14229_1;
            out[4] = (uint8_t)(count >> 8);
            out[5] = (uint8_t)count;
            rn = 6;
        } else {
            rn = 3;
            for (int i = 0; i < DEM_EVENTS; i++) {
                if (!(dem->e[i].status & mask)) continue;
                uds_dtc_bytes(i, out + rn);
                out[rn + 3] = dem->e[i].status;
                rn += 4;
            }
        }
        break;
    }
    case UDS_SID_CLEAR_DTC: {
        if (n != 4) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        if (u->session != UDS_SESSION_EXTENDED) { rn = uds_nrc(out, sid, NRC_SERVICE_NOT_IN_SESSION); break; }
        if (req[1] == 0xFF && req[2] == 0xFF && req[3] == 0xFF) {
            dem_clear(dem);
        } else {
            int found = -1;
            for (int i = 0; i < DEM_EVENTS; i++) {
                uint8_t b[3];
                uds_dtc_bytes(i, b);
                if (b[0] == req[1] && b[1] == req[2] && b[2] == req[3]) found = i;
            }
            if (found < 0) { rn = uds_nrc(out, sid, NRC_REQUEST_OUT_OF_RANGE); break; }
            dem_clear_one(dem, (dem_evt_id_t)found);
        }
        out[0] = sid + 0x40;
        rn = 1;
        break;
    }
    case UDS_SID_SECURITY: {
        if (n < 2) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t sub = req[1] & 0x7F;
        spr = req[1] >> 7;
        if (sub != 0x01 && sub != 0x02) { rn = uds_nrc(out, sid, NRC_SUBFUNCTION_NOT_SUPPORTED); break; }
        if ((sub == 0x01 && n != 2) || (sub == 0x02 && n != 6)) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        if (u->session != UDS_SESSION_EXTENDED) { rn = uds_nrc(out, sid, NRC_SERVICE_NOT_IN_SESSION); break; }
        if (u->locked && isotp_due(env->now_ms, u->lock_until)) { u->locked = 0; u->attempts = 0; }   /* the delay is over */
        if (u->locked) { rn = uds_nrc(out, sid, NRC_DELAY_NOT_EXPIRED); break; }
        out[0] = sid + 0x40; out[1] = sub;
        if (sub == 0x01) {   /* requestSeed */
            if (u->unlocked) {
                memset(out + 2, 0, 4);                  /* already unlocked: a seed of zeros, as the standard says */
            } else {
                uint32_t x = env->now_ms ^ (++u->seed_counter * 2654435761u) ^ 0x6C078965u;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift */
                u->seed = x ? x : 0xA5A5A5A5u;
                u->seed_sent = 1;
                out[2] = (uint8_t)(u->seed >> 24); out[3] = (uint8_t)(u->seed >> 16);
                out[4] = (uint8_t)(u->seed >> 8); out[5] = (uint8_t)u->seed;
            }
            rn = 6;
        } else {             /* sendKey */
            if (!u->seed_sent) { rn = uds_nrc(out, sid, NRC_REQUEST_SEQUENCE_ERROR); break; }
            const uint32_t key = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) | ((uint32_t)req[4] << 8) | req[5];
            u->seed_sent = 0;                           /* a seed is good for one try */
            if (key == uds_key(u->seed)) {
                u->unlocked = 1;
                u->attempts = 0;
                rn = 2;
            } else if (++u->attempts >= UDS_SEC_MAX_ATTEMPTS) {
                u->locked = 1;
                u->lock_until = env->now_ms + UDS_SEC_LOCK_MS;
                rn = uds_nrc(out, sid, NRC_EXCEEDED_ATTEMPTS);
            } else {
                rn = uds_nrc(out, sid, NRC_INVALID_KEY);
            }
        }
        break;
    }
    case UDS_SID_ECU_RESET: {
        if (n != 2) { rn = uds_nrc(out, sid, NRC_INCORRECT_LENGTH); break; }
        const uint8_t sub = req[1] & 0x7F;
        spr = req[1] >> 7;
        if (sub != 0x01) { rn = uds_nrc(out, sid, NRC_SUBFUNCTION_NOT_SUPPORTED); break; }   /* hardReset only */
        if (u->session != UDS_SESSION_EXTENDED) { rn = uds_nrc(out, sid, NRC_SERVICE_NOT_IN_SESSION); break; }
        if (!u->unlocked) { rn = uds_nrc(out, sid, NRC_SECURITY_ACCESS_DENIED); break; }
        out[0] = sid + 0x40; out[1] = sub;
        rn = 2;
        u->reset_request = 1;
        break;
    }
    default:
        rn = uds_nrc(out, sid, NRC_SERVICE_NOT_SUPPORTED);
        break;
    }

    if (out[0] == UDS_SID_NEGATIVE) {
        const uint8_t nrc = out[2];
        if (functional && (nrc == NRC_SERVICE_NOT_SUPPORTED || nrc == NRC_SUBFUNCTION_NOT_SUPPORTED ||
                           nrc == NRC_REQUEST_OUT_OF_RANGE || nrc == NRC_SERVICE_NOT_IN_SESSION))
            return 0;
    } else if (spr) {
        return 0;
    }
    memcpy(resp, out, rn);
    return rn;
}

#endif
