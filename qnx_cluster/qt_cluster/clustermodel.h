// What the QML screen sees. The data state (valid / degraded / invalid) is decided here in C++,
// the QML only shows it.
#ifndef CLUSTERMODEL_H
#define CLUSTERMODEL_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "receiver.h"

class ClusterModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double speed READ speed NOTIFY changed)
    Q_PROPERTY(double rpm READ rpm NOTIFY changed)
    Q_PROPERTY(double fuel READ fuel NOTIFY changed)
    Q_PROPERTY(double coolant READ coolant NOTIFY changed)
    Q_PROPERTY(int gear READ gear NOTIFY changed)
    Q_PROPERTY(int driveMode READ driveMode NOTIFY changed)
    Q_PROPERTY(int warnings READ warnings NOTIFY changed)
    Q_PROPERTY(int dataState READ dataState NOTIFY stateChanged)   // DataState
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)     // empty while the data is valid
    Q_PROPERTY(double tripKm READ tripKm NOTIFY changed)           // integrated from the accepted speeds
    Q_PROPERTY(bool anyErrors READ anyErrors NOTIFY changed)       // something was rejected since start
    Q_PROPERTY(QString countersA READ countersA NOTIFY changed)    // "CRC .. SEQ .. RNG .. SRC .."
    Q_PROPERTY(QString countersB READ countersB NOTIFY changed)    // "JMP .. LOST .. REP .."
    Q_PROPERTY(QString counters READ counters NOTIFY changed)
    Q_PROPERTY(QString lastResult READ lastResult NOTIFY changed)
    Q_PROPERTY(bool serviceLamp READ serviceLamp NOTIFY diagChanged)  // cluster_diag holds a confirmed DTC
    Q_PROPERTY(int dtcCount READ dtcCount NOTIFY diagChanged)         // how many confirmed DTCs
    Q_PROPERTY(bool diagOnline READ diagOnline NOTIFY diagChanged)    // cluster_diag reported in the last 3 s

public:
    enum DataState { Valid = 0, Degraded = 1, Invalid = 2 };
    Q_ENUM(DataState)

    explicit ClusterModel(QObject *parent = nullptr);

    double speed() const { return m_speed; }
    double rpm() const { return m_rpm; }
    double fuel() const { return m_fuel; }
    double coolant() const { return m_coolant; }
    int gear() const { return m_gear; }
    int driveMode() const { return m_driveMode; }
    int warnings() const { return m_warnings; }
    int dataState() const { return m_state; }
    QString notice() const { return m_notice; }
    double tripKm() const { return m_tripKm; }
    bool anyErrors() const { return m_anyErrors; }
    QString countersA() const { return m_countersA; }
    QString countersB() const { return m_countersB; }
    QString counters() const { return m_counters; }
    QString lastResult() const { return m_lastResult; }
    bool serviceLamp() const { return m_diagOnline && m_dtcConfirmed > 0; }
    int dtcCount() const { return m_dtcConfirmed; }
    bool diagOnline() const { return m_diagOnline; }
    QString diagText() const;

public slots:
    void onMessage(const Snapshot &s);
    void onDiagState(quint32 state);    // bits 0..7 confirmed DTCs, 8..15 failing now, 16..23 stored (cluster_diag_msg.h)

signals:
    void changed();
    void stateChanged();
    void diagChanged();

private:
    void evaluate();

    double m_speed = 0, m_rpm = 0, m_fuel = 0, m_coolant = 0;
    int m_gear = 0, m_driveMode = 0, m_warnings = 0;
    int m_state = Invalid;
    QString m_notice = QStringLiteral("NO SIGNAL");
    double m_tripKm = 0;
    bool m_anyErrors = false;
    QString m_countersA, m_countersB;
    QString m_counters;
    QString m_lastResult;

    int m_dtcConfirmed = 0, m_dtcFailing = 0, m_dtcStored = 0;
    bool m_diagOnline = false;
    qint64 m_lastDiagMs = 0;

    QElapsedTimer m_clock;
    bool m_haveValid = false;
    qint64 m_lastValidMs = 0;
    qint64 m_lastErrMs = -1;   // -1 = never
    QTimer m_tick;
};

#endif
