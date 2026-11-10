// Receives vehicle_msg_t over QNX message passing (the same contract as cluster_display.c)
// and runs the E2E checks of cluster_e2e.h. Everything here runs in its own thread.
#ifndef RECEIVER_H
#define RECEIVER_H

#include <QMetaType>
#include <QThread>

#include "diaglink.h"

// What the GUI thread gets for every message (accepted or not).
struct Snapshot {
    bool accepted = false;      // passed every E2E check; the values below are only meaningful then
    int result = 0;             // e2e_result_t
    float speed = 0, rpm = 0, fuel = 0, coolant = 0;
    int gear = 0, driveMode = 0, warnings = 0;
    quint32 nOk = 0, nCrc = 0, nRepeat = 0, nWrongSeq = 0, nRange = 0, nJump = 0, nSrc = 0, nLost = 0;
};
Q_DECLARE_METATYPE(Snapshot)

class Receiver : public QThread
{
    Q_OBJECT
public:
    explicit Receiver(QObject *parent = nullptr);
    ~Receiver() override;

signals:
    void message(const Snapshot &s);
    void diagState(quint32 state);      // from cluster_diag: bits 0..7 confirmed DTCs, 8..15 failing now, 16..23 stored

protected:
    void run() override;

private:
    DiagLink m_diag;            // reports to cluster_diag; used by the receiver thread only
};

#endif
