// A connection from the display application to cluster_diag that sends pulses and never blocks.
// One object belongs to one thread (it keeps the connection and the retry time without locking).
#ifndef DIAGLINK_H
#define DIAGLINK_H

class DiagLink
{
public:
    ~DiagLink();

    // Sends one pulse. When cluster_diag is not there the pulse is dropped; the connection is tried
    // again once a second. `now` is a clock in seconds.
    void pulse(int code, int value, double now);

private:
    int m_coid = -1;            // -1 = not connected
    double m_retry = 0;         // earliest time for the next connection attempt
};

#endif
