#include "clustermodel.h"

#include "cluster_e2e.h"

namespace {

constexpr qint64 kStaleMs = 500;     // no valid message for this long: show the safe state
constexpr qint64 kErrHoldMs = 2000;  // a rejected message keeps the "data error" notice up this long
constexpr qint64 kDiagStaleMs = 3000; // cluster_diag reports once a second: silent for this long = not running

const char *resultName(int r)
{
    switch (r) {
    case E2E_OK: return "OK";
    case E2E_OK_GAP: return "OK (gap)";
    case E2E_CRC_ERR: return "CRC error";
    case E2E_REPEATED: return "repeated counter";
    case E2E_WRONG_SEQ: return "wrong counter";
    case E2E_RANGE_ERR: return "out of range";
    case E2E_JUMP_ERR: return "value jump";
    case E2E_SRC_ERR: return "source fault";
    }
    return "?";
}

}  // namespace

ClusterModel::ClusterModel(QObject *parent) : QObject(parent)
{
    m_clock.start();
    // the state must also change when no message arrives at all
    m_tick.setInterval(50);
    connect(&m_tick, &QTimer::timeout, this, &ClusterModel::evaluate);
    m_tick.start();
}

void ClusterModel::onMessage(const Snapshot &s)
{
    const qint64 now = m_clock.elapsed();
    if (s.accepted) {
        if (m_haveValid) {      // trip: speed x time since the previous accepted message (a gap longer than 0.5 s counts as 0.5 s)
            const double dt = qMin<qint64>(now - m_lastValidMs, kStaleMs) / 1000.0;
            m_tripKm += s.speed * dt / 3600.0;
        }
        m_speed = s.speed;
        m_rpm = s.rpm;
        m_fuel = s.fuel;
        m_coolant = s.coolant;
        m_gear = s.gear;
        m_driveMode = s.driveMode;
        m_warnings = s.warnings;
        m_lastValidMs = now;
        m_haveValid = true;
    } else {
        m_lastErrMs = now;
    }
    auto cap = [](quint32 n) { return qMin<quint32>(n, 999); };   // the panel is narrow
    m_anyErrors = s.nCrc + s.nWrongSeq + s.nRange + s.nJump + s.nLost + s.nRepeat + s.nSrc > 0;
    m_countersA = QStringLiteral("CRC %1 SEQ %2 RNG %3 SRC %4")
                      .arg(cap(s.nCrc)).arg(cap(s.nWrongSeq)).arg(cap(s.nRange)).arg(cap(s.nSrc));
    m_countersB = QStringLiteral("JMP %1 LOST %2 REP %3").arg(cap(s.nJump)).arg(cap(s.nLost)).arg(cap(s.nRepeat));
    m_counters = QStringLiteral("CRC %1  SEQ %2  RNG %3  SRC %4  JMP %5  LOST %6  REP %7")
                     .arg(s.nCrc).arg(s.nWrongSeq).arg(s.nRange).arg(s.nSrc).arg(s.nJump).arg(s.nLost).arg(s.nRepeat);
    m_lastResult = QString::fromLatin1(resultName(s.result));
    emit changed();
    evaluate();
}

void ClusterModel::onDiagState(quint32 state)
{
    m_lastDiagMs = m_clock.elapsed();
    const int confirmed = static_cast<int>(state & 0xFF), failing = static_cast<int>((state >> 8) & 0xFF),
              stored = static_cast<int>((state >> 16) & 0xFF);
    if (!m_diagOnline || confirmed != m_dtcConfirmed || failing != m_dtcFailing || stored != m_dtcStored) {
        m_diagOnline = true;
        m_dtcConfirmed = confirmed;
        m_dtcFailing = failing;
        m_dtcStored = stored;
        emit diagChanged();
    }
}

QString ClusterModel::diagText() const
{
    if (!m_diagOnline) return QStringLiteral("diag off");
    return QStringLiteral("DTC confirmed %1 failing %2 stored %3").arg(m_dtcConfirmed).arg(m_dtcFailing).arg(m_dtcStored);
}

void ClusterModel::evaluate()
{
    const qint64 now = m_clock.elapsed();
    if (m_diagOnline && now - m_lastDiagMs > kDiagStaleMs) {   // cluster_diag stopped: the lamp has nothing to say
        m_diagOnline = false;
        emit diagChanged();
    }
    const bool recentErr = m_lastErrMs >= 0 && now - m_lastErrMs < kErrHoldMs;
    int state;
    QString notice;
    if (m_haveValid && now - m_lastValidMs < kStaleMs) {
        state = recentErr ? Degraded : Valid;
        if (state == Degraded) notice = QStringLiteral("DATA ERROR");
    } else {
        state = Invalid;
        notice = recentErr ? QStringLiteral("DATA INVALID") : QStringLiteral("NO SIGNAL");
    }
    if (state != m_state || notice != m_notice) {
        m_state = state;
        m_notice = notice;
        emit stateChanged();
    }
}
