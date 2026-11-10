/* Contract between the programs that find a fault (qt_cluster) and cluster_diag, which keeps the
 * diagnostic trouble codes (DTCs). It is the counterpart of the "fault report" an application sends to
 * the diagnostic event manager in an AUTOSAR ECU.
 *
 * The reports are QNX pulses: sending one never blocks, so a stopped or slow cluster_diag can not
 * hold back the thread that checks the vehicle data. A pulse carries a small code and one 32-bit value. */
#ifndef CLUSTER_DIAG_MSG_H
#define CLUSTER_DIAG_MSG_H

#define DIAG_NAME "cluster_diag" /* registered with name_attach() */

/* pulse codes (the user range of a pulse is 0..127) */
#define DIAG_PULSE_E2E 1  /* value = e2e_result_t of the message that was just checked (cluster_e2e.h) */
#define DIAG_PULSE_LINK 2 /* value = 1: no message at all for 0.5 s (NO SIGNAL), 0: messages again */

/* the display application's heartbeat: sent while its screen is really being drawn */
#define DIAG_PULSE_HEARTBEAT 3 /* value = number of frames drawn so far */

/* the vehicle data, for the freeze frame of a DTC: two pulses, sent about 5 times a second while the data is valid */
#define DIAG_PULSE_VEHICLE_A 4 /* value = speed in 0.1 km/h (high 16 bits) and rpm (low 16 bits) */
#define DIAG_PULSE_VEHICLE_B 5 /* value = coolant degC+40 (bits 27..20), fuel in 0.5 % (19..12), gear (11..8), mode (7..6), warnings (5..0) */

static inline int diag_clamp(float v, int lo, int hi)
{
    const int i = (int)(v + 0.5f);   /* the values are never negative here */
    return i < lo ? lo : (i > hi ? hi : i);
}

static inline unsigned diag_pack_a(float speed_kmh, float rpm)
{
    return ((unsigned)diag_clamp(speed_kmh * 10.0f, 0, 0xFFFF) << 16) | (unsigned)diag_clamp(rpm, 0, 0xFFFF);
}

static inline unsigned diag_pack_b(float coolant_c, float fuel_pct, int gear, int mode, int warnings)
{
    return ((unsigned)diag_clamp(coolant_c + 40.0f, 0, 255) << 20) | ((unsigned)diag_clamp(fuel_pct * 2.0f, 0, 255) << 12) |
           ((unsigned)(gear & 0x0F) << 8) | ((unsigned)(mode & 0x03) << 6) | (unsigned)(warnings & 0x3F);
}

/* cluster_diag -> qt_cluster: a pulse to the channel CLUSTER_NAME (cluster_msg.h) with what the screen shows.
 * Sent when the DTC memory changed and once a second otherwise, so a restarted display catches up. */
#define DIAG_UI_PULSE_STATE 10 /* value: bits 0..7 confirmed DTCs (service lamp), 8..15 failing now, 16..23 stored (cluster_dem.h) */

#endif
