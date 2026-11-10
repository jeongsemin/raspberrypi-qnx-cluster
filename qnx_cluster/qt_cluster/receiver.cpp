#include "receiver.h"

#include <QDebug>

#include <errno.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <time.h>

#include "cluster_diag_msg.h"
#include "cluster_e2e.h"

namespace {

constexpr double kStaleSec = 0.5;   // no valid message for this long: the E2E baseline starts afresh
constexpr double kVehicleReportSec = 0.2;   // how often cluster_diag gets the vehicle data
constexpr double kLinkReportSec = 1.0;      // how often cluster_diag gets the link state (besides its changes)
constexpr uint64_t kPollNs = 200000000ULL;  // MsgReceive timeout, so the thread can notice a stop request

double monoNow()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

union RecvBuf {
    uint16_t type;
    vehicle_msg_t vehicle;
    _pulse pulse;
};

}  // namespace

Receiver::Receiver(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<Snapshot>("Snapshot");
}

Receiver::~Receiver()
{
    requestInterruption();
    wait(1000);
}

void Receiver::run()
{
    name_attach_t *att = name_attach(nullptr, CLUSTER_NAME, 0);
    if (!att) {
        qCritical("name_attach(%s) failed: %s (is cluster_display still running?)", CLUSTER_NAME, strerror(errno));
        return;
    }
    qInfo("listening on /dev/name/local/%s", CLUSTER_NAME);

    e2e_state_t e2e;
    memset(&e2e, 0, sizeof e2e);
    bool haveValid = false;
    double lastValid = 0;
    double lastVehicleReport = 0;
    bool rxSeen = false;            // any message has arrived (valid or not)
    bool linkLost = false;          // reported to cluster_diag as lost: no message at all for kStaleSec
    double lastRx = 0;
    double lastLinkReport = 0;

    while (!isInterruptionRequested()) {
        RecvBuf m;
        _msg_info info;
        uint64_t timeout = kPollNs;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, nullptr, &timeout, nullptr);
        int rcvid = MsgReceive(att->chid, &m, sizeof m, &info);
        // communication lost = nothing arrives; messages with bad content are a different DTC
        if (rxSeen && !linkLost && monoNow() - lastRx > kStaleSec) {
            linkLost = true;
            m_diag.pulse(DIAG_PULSE_LINK, 1, monoNow());
        }
        // The edges above and below are single pulses: one sent while cluster_diag restarts (UDS ECU reset) or
        // reconnects is lost, and a ClearDTC during a loss would never see the loss again. So the current state
        // also goes out once a second (repeating it does not change the DTC).
        if (rxSeen && monoNow() - lastLinkReport >= kLinkReportSec) {
            lastLinkReport = monoNow();
            m_diag.pulse(DIAG_PULSE_LINK, linkLost ? 1 : 0, lastLinkReport);
        }
        if (rcvid == -1) continue;      // timeout or interrupted
        if (rcvid == 0) {               // pulse
            if (m.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(m.pulse.scoid);
            else if (m.pulse.code == DIAG_UI_PULSE_STATE) emit diagState(static_cast<quint32>(m.pulse.value.sival_int));
            continue;
        }
        if (m.type != MSG_VEHICLE) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        const double now = monoNow();
        rxSeen = true;
        lastRx = now;
        if (linkLost) {
            linkLost = false;
            m_diag.pulse(DIAG_PULSE_LINK, 0, now);
        }
        // the link was down long enough for the safe state: start the checks afresh
        if (haveValid && now - lastValid > kStaleSec) e2e_reset_baseline(&e2e);

        Snapshot s;
        if (info.msglen < static_cast<int>(sizeof(vehicle_msg_t))) {
            e2e.n_crc++;                // truncated message: treated like a corrupted one
            s.result = E2E_CRC_ERR;
        } else {
            e2e_result_t r = e2e_check(&e2e, &m.vehicle);
            s.result = r;
            s.accepted = e2e_accepted(r);
            if (s.accepted) {
                const vehicle_msg_t &v = m.vehicle;
                s.speed = v.speed_kmh;
                s.rpm = v.rpm;
                s.fuel = v.fuel_pct;
                s.coolant = v.coolant_c;
                s.gear = v.gear;
                s.driveMode = v.drive_mode;
                s.warnings = v.warnings;
                lastValid = now;
                haveValid = true;
                if (now - lastVehicleReport >= kVehicleReportSec) {   // the freeze-frame data for cluster_diag
                    lastVehicleReport = now;
                    m_diag.pulse(DIAG_PULSE_VEHICLE_A, static_cast<int>(diag_pack_a(v.speed_kmh, v.rpm)), now);
                    m_diag.pulse(DIAG_PULSE_VEHICLE_B, static_cast<int>(diag_pack_b(v.coolant_c, v.fuel_pct, v.gear, v.drive_mode, v.warnings)), now);
                }
            }
        }
        m_diag.pulse(DIAG_PULSE_E2E, s.result, now);
        s.nOk = e2e.n_ok;
        s.nCrc = e2e.n_crc;
        s.nRepeat = e2e.n_repeat;
        s.nWrongSeq = e2e.n_wrongseq;
        s.nRange = e2e.n_range;
        s.nJump = e2e.n_jump;
        s.nSrc = e2e.n_src;
        s.nLost = e2e.n_lost;

        if (s.accepted) MsgReply(rcvid, EOK, nullptr, 0);
        else MsgError(rcvid, EBADMSG);  // the sender sees the rejection
        emit message(s);
    }
    name_detach(att, 0);
}
