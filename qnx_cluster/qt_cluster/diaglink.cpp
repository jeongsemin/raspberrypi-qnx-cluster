#include "diaglink.h"

#include <QDebug>

#include <errno.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cluster_diag_msg.h"

namespace {
constexpr double kRetrySec = 1.0;
}

DiagLink::~DiagLink()
{
    if (m_coid != -1) name_close(m_coid);
}

void DiagLink::pulse(int code, int value, double now)
{
    if (m_coid == -1) {
        if (now < m_retry) return;
        m_retry = now + kRetrySec;
        m_coid = name_open(DIAG_NAME, 0);
        if (m_coid == -1) return;
        qInfo("connected to %s", DIAG_NAME);
    }
    if (MsgSendPulse(m_coid, SIGEV_PULSE_PRIO_INHERIT, code, value) == -1) {
        qInfo("lost the connection to %s: %s", DIAG_NAME, strerror(errno));
        name_close(m_coid);
        m_coid = -1;
        m_retry = now + kRetrySec;
    }
}
