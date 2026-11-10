// Qt cluster, step 1: receiver thread + model + a text-only QML screen.
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

#include <atomic>
#include <memory>
#include <QTimer>

#include <time.h>

#include "cluster_diag_msg.h"
#include "clustermodel.h"
#include "diaglink.h"
#include "receiver.h"

namespace {
double monoSeconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
}  // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setFont(QFont("Liberation Sans"));     // the font files come from QT_QPA_FONTDIR (fontconfig is off)

    ClusterModel model;
    Receiver receiver;
    QObject::connect(&receiver, &Receiver::message, &model, &ClusterModel::onMessage, Qt::QueuedConnection);
    QObject::connect(&receiver, &Receiver::diagState, &model, &ClusterModel::onDiagState, Qt::QueuedConnection);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("cluster", &model);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Cluster", "Main");

    receiver.start();

    // Heartbeat for cluster_diag: sent only while the screen is really being drawn. Every 250 ms the GUI thread
    // asks for a new frame; the render thread counts the frames it swaps; if the count moved, the heartbeat goes
    // out. A hung GUI thread or a hung render thread both stop it, and cluster_diag records the DTC U1A04.
    DiagLink heartbeatLink;
    std::atomic<int> frames{0};
    std::atomic<int> gapMaxMs{0};         // longest time between two swapped frames since the last report
    std::atomic<qint64> lastSwapMs{0};
    QTimer heartbeat;
    if (!engine.rootObjects().isEmpty()) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (win) {
            QObject::connect(win, &QQuickWindow::frameSwapped, win, [&frames, &gapMaxMs, &lastSwapMs] {
                frames.fetch_add(1);
                const qint64 now = static_cast<qint64>(monoSeconds() * 1000.0);
                const qint64 prev = lastSwapMs.exchange(now);
                if (prev) {
                    const int gap = static_cast<int>(now - prev);
                    int cur = gapMaxMs.load();
                    while (gap > cur && !gapMaxMs.compare_exchange_weak(cur, gap)) {}
                }
            }, Qt::DirectConnection);
            auto lastFrames = std::make_shared<int>(0);
            QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [win, &frames, &heartbeatLink, lastFrames] {
                const int now = frames.load();
                if (now != *lastFrames) heartbeatLink.pulse(DIAG_PULSE_HEARTBEAT, now, monoSeconds());
                *lastFrames = now;
                win->update();      // make sure there is a frame to count next time, even if nothing moves
            });
            heartbeat.start(250);
        }
    }

    // QC_GRAB=<dir>: save what the scene graph rendered whenever the state changes (the QNX
    // screenshot tool cannot capture OpenGL windows, grabWindow() reads the frame back instead)
    const QString grabDir = qEnvironmentVariable("QC_GRAB");
    if (!grabDir.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *periodic = new QTimer(win);       // also one picture every 4 s, to see the steady state
        auto count = std::make_shared<int>(0);
        QObject::connect(periodic, &QTimer::timeout, win, [win, grabDir, count] {
            win->grabWindow().save(QString("%1/grab_t%2.png").arg(grabDir).arg((*count)++));
        });
        periodic->start(4000);
        QObject::connect(&model, &ClusterModel::stateChanged, win, [&, win, grabDir] {
            const QString tag = model.notice().isEmpty() ? QString("VALID") : model.notice().replace(' ', '_');
            QTimer::singleShot(400, win, [win, grabDir, tag] {
                win->grabWindow().save(QString("%1/grab_%2.png").arg(grabDir, tag));
            });
        });
    }

    // Qt's log goes to slog2 on QNX: one line per second shows what the model holds without looking at the screen
    QTimer report;
    QObject::connect(&report, &QTimer::timeout, &model, [&model, &frames, &gapMaxMs] {
        static int prevFrames = 0;
        const int nowFrames = frames.load();
        const QString fps = QString("fps=%1 gap=%2ms").arg(nowFrames - prevFrames).arg(gapMaxMs.exchange(0));
        prevFrames = nowFrames;
        qInfo().noquote() << fps << QString("state=%1 speed=%2 rpm=%3 gear=%4 fuel=%5 coolant=%6 mode=%7 warn=0x%8 | %9 | last: %10")
                                 .arg(model.notice().isEmpty() ? "VALID" : model.notice())
                                 .arg(model.speed(), 0, 'f', 1).arg(model.rpm(), 0, 'f', 0).arg(model.gear())
                                 .arg(model.fuel(), 0, 'f', 1).arg(model.coolant(), 0, 'f', 1)
                                 .arg(model.driveMode()).arg(model.warnings(), 0, 16)
                                 .arg(model.counters(), model.lastResult())
                             + " | " + model.diagText();
    });
    report.start(1000);

    return app.exec();
}
