/* Message contract between speed_source (client) and cluster_display (server). */
#ifndef CLUSTER_MSG_H
#define CLUSTER_MSG_H

#include <stdint.h>

#define CLUSTER_NAME "cluster_speed" /* registered with name_attach() */

#define MSG_VEHICLE 1

/* drive modes (each has its own colour theme on the display) */
#define DRIVE_ECO     0
#define DRIVE_COMFORT 1
#define DRIVE_SPORT   2

/* warning / indicator bits */
#define W_CHECK_ENGINE (1u << 0)
#define W_BATTERY      (1u << 1)
#define W_OIL          (1u << 2)
#define W_SEATBELT     (1u << 3)
#define W_TURN_LEFT    (1u << 4)
#define W_TURN_RIGHT   (1u << 5)

typedef struct {
    uint16_t type;       /* MSG_VEHICLE */
    uint16_t seq;        /* alive counter: +1 per message, wraps at 65536 */
    float speed_kmh;
    float rpm;
    float fuel_pct;      /* 0..100 */
    float coolant_c;     /* engine coolant temperature */
    uint8_t gear;        /* 0 = neutral, 1..6 */
    uint8_t drive_mode;  /* DRIVE_* */
    uint16_t warnings;   /* W_* bits */
    uint8_t crc;         /* CRC-8 over every other byte plus a data id (cluster_e2e.h) */
    uint8_t src_fault;   /* SRC_*: set by cluster_gateway when the CAN frame it got was bad (part of the CRC) */
    uint8_t reserved[2]; /* always 0 (part of the CRC) */
} vehicle_msg_t;

/* src_fault values: why the data of this message must not be shown */
#define SRC_OK      0
#define SRC_CRC     1  /* the CAN frame's own CRC was wrong */
#define SRC_COUNTER 2  /* the CAN frame's counter repeated or jumped */

#endif
