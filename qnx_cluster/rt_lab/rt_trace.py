#!/usr/bin/env python3
"""Turns a traceprinter text dump of rt_inversion into a time line of one CPU (step 17-5).

Usage: python3 rt_trace.py <trace.txt> <cpu>

tracelogger -w records every thread switch ("THRUNNING pid tid priority") and the text events rt_inversion
writes with rt_mark(). The thread that runs on the CPU when a text event appears is the one that wrote it, so
"L locked" names thread L, "M start" thread M, "H wants the mutex" thread H and "round" the controller C.
Everything else that ran on that CPU is "other" (interrupt threads, drivers, other programs).

It prints, for the last round in the file:
  - the order of events: thread switches on the CPU and the text events, with the priority each thread ran at
  - a chart: one row per thread, one column per slice of time, '#' where that thread had the CPU
  - the punch line: while H waited for the mutex, how long M, L and others had the CPU
"""
import re
import sys

LINE = re.compile(r"^t:0x([0-9a-f]+) CPU:(\d+) (\w+)\s*:(\S+)\s*(.*)$")
RUN = re.compile(r"pid:(\d+) tid:(\d+) priority:(\d+)")
STR = re.compile(r'STR:"([^"]*)"')
WORD = re.compile(r"d\d:0x([0-9a-f]{8})")
ROLES = (("round", "C"), ("L locked", "L"), ("M start", "M"), ("H wants", "H"))
ROWS = ("C", "H", "M", "L", "other")


def marker_text(rest):
    """the text of a user string event; traceprinter sometimes leaves STR out, then the d0.. words hold it"""
    m = STR.search(rest)
    if m and m.group(1):
        return m.group(1)
    raw = b"".join(int(w, 16).to_bytes(4, "little") for w in WORD.findall(rest))
    return raw.split(b"\0")[0].decode("ascii", "replace")


def parse(path, cpu):
    """events on one CPU: ('run', t, pid, tid, prio) and ('mark', t, text), with times in cycles"""
    events = []
    cps = None
    with open(path, errors="replace") as f:
        for line in f:
            if cps is None and "TRACE_CYCLES_PER_SEC::" in line:
                cps = int(line.split("::")[1])
            m = LINE.match(line)
            if not m or int(m.group(2)) != cpu:
                continue
            t, cls, ev, rest = int(m.group(1), 16), m.group(3), m.group(4), m.group(5)
            if cls == "THREAD" and ev == "THRUNNING":
                r = RUN.search(rest)
                if r:
                    events.append(("run", t, int(r.group(1)), int(r.group(2)), int(r.group(3))))
            elif cls == "USREVENT" and ev == "EVENT:100":
                events.append(("mark", t, marker_text(rest)))
    if not cps:
        sys.exit("ERROR: no TRACE_CYCLES_PER_SEC in the file (is it traceprinter output?)")
    return events, cps


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    events, cps = parse(sys.argv[1], int(sys.argv[2]))
    rounds = [i for i, e in enumerate(events) if e[0] == "mark" and e[2].startswith("round")]
    if not rounds:
        sys.exit("ERROR: no 'round' text event on CPU %s; was rt_inversion run with -c %s?" % (sys.argv[2], sys.argv[2]))
    start = rounds[-1]

    # who is running when each event happens, and which thread is which
    cur = None
    for e in events[:start]:
        if e[0] == "run":
            cur = e[2:]
    roles, pid = {}, cur[0] if cur else None
    timeline = []                       # (t, kind, who, prio or text)
    end_t = None
    for e in events[start:]:
        if e[0] == "run":
            cur = e[2:]
            timeline.append((e[1], "run", (cur[0], cur[1]), cur[2]))
        else:
            text = e[2]
            if cur and cur[0] == pid:
                for prefix, role in ROLES:
                    if text.startswith(prefix):
                        roles.setdefault((cur[0], cur[1]), role)
            timeline.append((e[1], "mark", None, text))
            if text.startswith("round") and e is not events[start]:
                break
            if text.startswith(("M end", "L unlock", "H got")):
                end_t = e[1]
    t0 = events[start][1]
    end_t = end_t or timeline[-1][0]
    ms = lambda c: (c - t0) * 1000.0 / cps
    name = lambda who: roles.get(who, "other") if who else ""

    # 1. order of events (switches shorter than 50 us to "other" threads are only counted)
    print("== time line of CPU %s, last round (ms from the round start)" % sys.argv[2])
    runs = [x for x in timeline if x[1] == "run" and x[0] <= end_t]
    # rows: [start ms, "run", who, prio, ms of CPU] or [start ms, "mark", text]. A thread that gets the CPU back
    # after only short runs of others (under 50 us: interrupts) stays one row; its CPU time leaves those out.
    rows, hidden = [], 0
    for i, (t, kind, who, val) in enumerate(timeline):
        if t > end_t:
            break
        if kind == "mark":
            rows.append([ms(t), "mark", val])
            continue
        nxt = next((x[0] for x in timeline[i + 1:] if x[1] == "run"), end_t)
        dur = ms(min(nxt, end_t)) - ms(t)
        if name(who) == "other" and dur < 0.05:
            hidden += 1
            continue
        last = next((r for r in reversed(rows) if r[1] == "run"), None)
        if last and last[2] == who and last[3] == val:
            last[4] += dur
        else:
            rows.append([ms(t), "run", who, val, dur])
    for r in rows:
        if r[1] == "mark":
            print("  %8.3f  %-6s  \"%s\"" % (r[0], "", r[2]))
        else:
            print("  %8.3f  %-6s  runs %7.3f ms at priority %d%s" % (
                r[0], name(r[2]), r[4], r[3], "" if name(r[2]) != "other" else "  (pid %d)" % r[2][0]))
    if hidden:
        print("  (%d short runs of other threads under 50 us left out)" % hidden)

    # 2. chart
    total = ms(end_t)
    cols = 64
    width = total / cols if total > 0 else 1
    busy = {r: [0.0] * cols for r in ROWS}
    for i, (t, kind, who, val) in enumerate(runs):
        a = ms(t)
        b = ms(runs[i + 1][0]) if i + 1 < len(runs) else total
        b = min(b, total)
        c = int(a / width)
        while a < b and c < cols:
            edge = min(b, (c + 1) * width)
            busy[name(who)][c] += edge - a
            a, c = edge, c + 1
    print()
    print("== who had CPU %s (one column = %.2f ms, '#' = more than half of it)" % (sys.argv[2], width))
    for r in ROWS:
        print("  %-5s |%s|" % (r, "".join("#" if x > width / 2 else ("." if x > 0 else " ") for x in busy[r])))
    print("        0 ms%s%.1f ms" % (" " * (cols - 9), total))

    # 3. while H waited
    want = next((t for t, k, w, v in timeline if k == "mark" and v.startswith("H wants")), None)
    got = next((t for t, k, w, v in timeline if k == "mark" and v.startswith("H got")), None)
    if want is None or got is None:
        print("\nH did not wait in this round")
        return
    share = {r: 0.0 for r in ROWS}
    prios = {r: set() for r in ROWS}
    for i, (t, kind, who, val) in enumerate(runs):
        b = runs[i + 1][0] if i + 1 < len(runs) else end_t
        a, b = max(t, want), min(b, got)
        if b > a:
            share[name(who)] += (b - a) * 1000.0 / cps
            prios[name(who)].add(val)
    print()
    print("== while H waited for the mutex (%.3f ms)" % ((got - want) * 1000.0 / cps))
    for r in ("M", "L", "other"):
        p = ",".join(str(x) for x in sorted(prios[r])) or "-"
        print("  %-5s had the CPU %7.3f ms   (priority %s)" % (r, share[r], p))
    print("RESULT trace h_wait_ms=%.3f m_ms=%.3f l_ms=%.3f other_ms=%.3f l_prio=%s" % (
        (got - want) * 1000.0 / cps, share["M"], share["L"], share["other"],
        ",".join(str(x) for x in sorted(prios["L"])) or "-"))


if __name__ == "__main__":
    main()
