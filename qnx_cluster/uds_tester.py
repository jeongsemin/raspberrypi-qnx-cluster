#!/usr/bin/env python3
"""A small UDS tester for cluster_diag on the QNX target. Run it in the Ubuntu (WSL) terminal.

The requests travel as virtual CAN frames in UDP datagrams (port 5556), the same way as the signal frames
of send_signal.py. A frame is 16 bytes in the layout of Linux struct can_frame. Request id 0x7E0 addresses
the cluster, 0x7DF is the functional (broadcast) address, the response comes with id 0x7E8. The UDS bytes
are put into ISO-TP frames: a message of up to 7 bytes in one single frame, a longer one in a first frame
and consecutive frames, with flow control from the receiver.

    python3 uds_tester.py 192.168.122.146 session extended        10 03
    python3 uds_tester.py 192.168.122.146 session default         10 01
    python3 uds_tester.py 192.168.122.146 tester-present          3E 00
    python3 uds_tester.py 192.168.122.146 read-did F186           22 F1 86 (several DIDs: F186 0100)
    python3 uds_tester.py 192.168.122.146 dtc-count [MASK]        19 01 MASK   (default mask FF)
    python3 uds_tester.py 192.168.122.146 read-dtc [MASK]         19 02 MASK   (default mask FF)
    python3 uds_tester.py 192.168.122.146 clear-dtc [all|U0401]   14 ...       (needs the extended session)
    python3 uds_tester.py 192.168.122.146 raw 22 F1 95            any bytes, in hex
    python3 uds_tester.py 192.168.122.146 demo                    a walk through every service
    python3 uds_tester.py 192.168.122.146 s3                      the session timer: 5 s without a request ends the extended session (takes 20 s)
    python3 uds_tester.py 192.168.122.146 unlock                  10 03, then 27 01 (seed) and 27 02 (key): security access
    python3 uds_tester.py 192.168.122.146 reset                   unlock, then 11 01: cluster_diag restarts (needs the unlock)
    python3 uds_tester.py 192.168.122.146 demo-security           a walk through 27 and 11, with wrong keys and refusals
    python3 uds_tester.py 192.168.122.146 snapshot U0401 [REC]    19 04: the vehicle data frozen when the DTC began (REC 01 or FF)
    python3 uds_tester.py 192.168.122.146 vehicle                 22 0200: the vehicle data now
  --functional   send to the functional address 0x7DF (no negative response for unsupported things)
  --timeout S    wait this long for the response (default 1 s)
  --bs N         block size we ask for when the answer is long: a flow control after every N consecutive frames (default 0 = all)
  --stmin MS     pause we ask for between two consecutive frames (default 0)
  --port N       default 5556
  --selftest     check the frame and DTC code conversions

Exit status: 0 for a positive response, 1 for a negative response, 2 for no response or a bad command.
The session stays as it is in cluster_diag between two runs, but only for 5 s (S3 timer)."""
import select
import socket
import struct
import sys
import time

ID_PHYS, ID_FUNC, ID_RESP = 0x7E0, 0x7DF, 0x7E8
MAX_FC_WAITS = 10       # flow control "wait" frames we put up with (as cluster_isotp.h)
MAX_PENDING = 6         # "response pending" answers we put up with: 6 x 5 s, then the request counts as unanswered
PAD = 0xAA
FC_BS = 0       # block size and STmin we ask the server for when it sends a long response (--bs, --stmin)
FC_STMIN = 0

NRC = {
    0x10: "generalReject", 0x11: "serviceNotSupported", 0x12: "subFunctionNotSupported",
    0x13: "incorrectMessageLengthOrInvalidFormat", 0x14: "responseTooLong", 0x22: "conditionsNotCorrect",
    0x31: "requestOutOfRange", 0x33: "securityAccessDenied", 0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}
SERVICE = {0x10: "DiagnosticSessionControl", 0x14: "ClearDiagnosticInformation", 0x19: "ReadDTCInformation",
           0x22: "ReadDataByIdentifier", 0x3E: "TesterPresent"}
NRC.update({0x22: "conditionsNotCorrect", 0x24: "requestSequenceError", 0x33: "securityAccessDenied", 0x35: "invalidKey",
            0x36: "exceededNumberOfAttempts", 0x37: "requiredTimeDelayNotExpired", 0x78: "requestCorrectlyReceivedResponsePending",
            0x72: "generalProgrammingFailure"})   # cluster_diag: the DTC memory changed but its file could not be written
SERVICE.update({0x11: "ECUReset", 0x27: "SecurityAccess"})
MODES = ("ECO", "COMFORT", "SPORT")
STATUS_BITS = [(0x01, "testFailed"), (0x02, "testFailedThisOperationCycle"), (0x04, "pendingDTC"),
               (0x08, "confirmedDTC"), (0x10, "testNotCompletedSinceLastClear"), (0x20, "testFailedSinceLastClear"),
               (0x40, "testNotCompletedThisOperationCycle"), (0x80, "warningIndicatorRequested")]


def can_frame(can_id, data):
    return struct.pack("<IB3x8s", can_id, len(data), bytes(data).ljust(8, b"\0"))


def sf_frame(can_id, uds):
    """UDS bytes -> ISO-TP single frame, 8 data bytes, padded."""
    if not 1 <= len(uds) <= 7:
        raise ValueError("a single frame carries 1..7 bytes (multi-frame comes with stage C)")
    return can_frame(can_id, bytes([len(uds)]) + bytes(uds) + bytes([PAD] * (7 - len(uds))))


def parse_frame(datagram):
    if len(datagram) != 16 or datagram[5:8] != b"\0\0\0":
        return None
    can_id, dlc = struct.unpack_from("<IB", datagram)
    return can_id, datagram[8:8 + min(dlc, 8)]


def dtc_text(b):
    """The two DTC bytes of SAE J2012 -> 'U0401'."""
    v = (b[0] << 8) | b[1]
    return "%s%d%03X" % ("PCBU"[v >> 14], (v >> 12) & 3, v & 0xFFF)


def dtc_value(text):
    """'U0401' -> the three bytes of the DTC (fault type 0)."""
    t = text.strip().upper()
    if len(t) != 5 or t[0] not in "PCBU" or t[1] not in "0123" or any(c not in "0123456789ABCDEF" for c in t[2:]):
        raise ValueError("DTC는 U0401 같은 형식이어야 합니다: %r" % text)
    v = ("PCBU".index(t[0]) << 14) | (int(t[1]) << 12) | int(t[2:], 16)
    return bytes([v >> 8, v & 0xFF, 0])


def status_text(st):
    return " ".join(n for bit, n in STATUS_BITS if st & bit) or "-"


def uds_key(seed):
    """The key for a seed. The same made-up algorithm as uds_key() in cluster_uds.h."""
    x = seed ^ 0xA5C3F00D
    return ((((x << 7) | (x >> 25)) & 0xFFFFFFFF) + 0x1234ABCD) & 0xFFFFFFFF


def vehicle_text(b):
    """The 10 bytes of DID 0200 (and of a DTC snapshot)."""
    return "speed %.1f km/h, %d rpm, coolant %d C, fuel %.1f %%, gear %d, mode %s, warnings 0x%02X" % (
        ((b[0] << 8) | b[1]) / 10.0, (b[2] << 8) | b[3], b[4] - 40, b[5] / 2.0, b[6],
        MODES[b[7]] if b[7] < 3 else "?", (b[8] << 8) | b[9])


def hexs(b):
    return " ".join("%02X" % x for x in b)


def wait_frame(sock, end):
    """The next frame with the response id before the time `end` (time.monotonic), or None."""
    while True:
        left = end - time.monotonic()
        if left <= 0 or not select.select([sock], [], [], left)[0]:
            return None
        got = parse_frame(sock.recvfrom(64)[0])
        if got and got[0] == ID_RESP and got[1]:
            return got[1]


def send_message(sock, dest, uds, timeout):
    """Sends a request of any length (up to 4095 bytes). Returns None when it went out, or the flow control
    bytes of a receiver that refused it (overflow)."""
    if len(uds) <= 7:
        sock.sendto(sf_frame(ID_PHYS, uds), dest)
        return None
    n = len(uds)
    if n > 4095:
        raise ValueError("an ISO-TP message carries at most 4095 bytes (this one has %d)" % n)
    waits = 0
    sock.sendto(can_frame(ID_PHYS, bytes([0x10 | (n >> 8), n & 0xFF]) + bytes(uds[:6])), dest)
    pos, sn = 6, 1
    while pos < n:
        fc = wait_frame(sock, time.monotonic() + timeout)
        if fc is None or fc[0] >> 4 != 3:
            raise ValueError("no flow control after the first frame" if fc is None else "unexpected frame %s" % hexs(fc))
        fs, bs, stmin = fc[0] & 0x0F, fc[1] if len(fc) > 1 else 0, fc[2] if len(fc) > 2 else 0
        if fs == 1:                                     # wait: the receiver sends another flow control later
            waits += 1
            if waits > MAX_FC_WAITS:
                raise ValueError("the receiver sent %d flow control 'wait' frames in a row" % waits)
            continue
        if fs != 0:
            return bytes(fc[:3])                        # overflow
        gap = stmin / 1000.0 if stmin <= 0x7F else (0.001 if 0xF1 <= stmin <= 0xF9 else 0.127)
        sent = 0
        while pos < n and (bs == 0 or sent < bs):
            chunk = bytes(uds[pos:pos + 7])
            sock.sendto(can_frame(ID_PHYS, bytes([0x20 | sn]) + chunk.ljust(7, bytes([PAD]))), dest)
            pos += len(chunk)
            sn = (sn + 1) & 0x0F
            sent += 1
            if pos < n and gap:
                time.sleep(gap)
    return None


def recv_message(sock, dest, timeout, bs, stmin):
    """Reads a response of any length. A first frame is answered with a flow control frame (block size bs,
    STmin stmin). Returns the bytes, or None."""
    data = wait_frame(sock, time.monotonic() + timeout)
    if data is None:
        return None
    pci = data[0] >> 4
    if pci == 0 and 1 <= (data[0] & 0x0F) <= len(data) - 1:
        return bytes(data[1:1 + (data[0] & 0x0F)])
    if pci == 3:
        return bytes(data[:3])
    if pci != 1 or len(data) < 8:
        print("  <- (unexpected frame) %s" % hexs(data))
        return None
    total = ((data[0] & 0x0F) << 8) | data[1]
    if total <= 6:
        print("  <- (a first frame for %d bytes: that fits in a single frame)" % total)
        return None
    msg = bytearray(data[2:8])
    sock.sendto(can_frame(ID_PHYS, bytes([0x30, bs, stmin])), dest)
    sn, block = 1, 0
    while len(msg) < total:
        cf = wait_frame(sock, time.monotonic() + 1.0)
        if cf is None:
            print("  <- (the consecutive frames stopped after %d of %d bytes)" % (len(msg), total))
            return None
        if cf[0] != (0x20 | sn):
            print("  <- (wrong sequence number %02X, wanted %02X)" % (cf[0], 0x20 | sn))
            return None
        msg += cf[1:1 + min(7, total - len(msg))]
        sn = (sn + 1) & 0x0F
        block += 1
        if bs and block == bs and len(msg) < total:
            block = 0
            sock.sendto(can_frame(ID_PHYS, bytes([0x30, bs, stmin])), dest)
    return bytes(msg)


def request(sock, dest, uds, functional=False, timeout=1.0):
    """Sends one request and returns the response bytes, or None. Prints both."""
    print("  -> %s%s" % (hexs(uds), "   (functional)" if functional else ""))
    if functional:
        if len(uds) > 7:
            raise ValueError("a functional request must fit in one frame")
        sock.sendto(sf_frame(ID_FUNC, uds), dest)
    else:
        refused = send_message(sock, dest, uds, timeout)
        if refused is not None:
            print("  <- flow control %s (overflow: the server will not take %d bytes)" % (hexs(refused), len(uds)))
            return refused
    resp = recv_message(sock, dest, timeout, FC_BS, FC_STMIN)
    pending = 0
    while resp is not None and len(resp) == 3 and resp[0] == 0x7F and resp[2] == 0x78 and pending < MAX_PENDING:
        pending += 1
        print("  <- %s   (response pending: the server needs more time, waiting up to 5 s)" % hexs(resp))
        resp = recv_message(sock, dest, 5.0, FC_BS, FC_STMIN)
    if resp is None:
        print("  <- (no response)")
    else:
        print("  <- %s%s" % (hexs(resp), "   (%d bytes)" % len(resp) if len(resp) > 7 else ""))
    return resp


def explain(uds, resp):
    """Prints what a response means. Returns 0 positive, 1 negative, 2 unusable."""
    if resp is None:
        return 2
    sid = uds[0]
    name = SERVICE.get(sid, "service 0x%02X" % sid)
    if resp[0] == 0x7F and len(resp) == 3:
        print("     negative response to %s: NRC 0x%02X %s" % (name, resp[2], NRC.get(resp[2], "?")))
        return 1
    if resp[0] != sid + 0x40:
        print("     unexpected response")
        return 2
    if sid == 0x10 and len(resp) == 6:
        print("     session 0x%02X, P2 %d ms, P2* %d ms" % (resp[1], (resp[2] << 8) | resp[3], ((resp[4] << 8) | resp[5]) * 10))
    elif sid == 0x19 and resp[1] == 0x01 and len(resp) == 6:
        print("     %d DTC(s) match (status availability mask 0x%02X, format %d)" % ((resp[4] << 8) | resp[5], resp[2], resp[3]))
    elif sid == 0x19 and resp[1] == 0x02:
        body = resp[3:]
        print("     status availability mask 0x%02X, %d DTC(s)" % (resp[2], len(body) // 4))
        for i in range(0, len(body) - 3, 4):
            print("     %s  fault type %02X  status 0x%02X  %s" % (dtc_text(body[i:i + 2]), body[i + 2], body[i + 3], status_text(body[i + 3])))
    elif sid == 0x19 and resp[1] == 0x04:
        status = resp[5]
        print("     %s  status 0x%02X  %s" % (dtc_text(resp[2:4]), status, status_text(status)))
        body = resp[6:]
        if not body:
            print("     no snapshot record")
        while len(body) >= 2:
            rec, nid = body[0], body[1]
            body = body[2:]
            print("     snapshot record %d, %d data identifier(s):" % (rec, nid))
            for _ in range(nid):
                did = (body[0] << 8) | body[1]
                if did == 0x0200 and len(body) >= 12:
                    print("       %04X  %s" % (did, vehicle_text(body[2:12])))
                    body = body[12:]
                else:
                    print("       %04X  (unknown)" % did)
                    body = b""
    elif sid == 0x22 and len(resp) == 13 and resp[1:3] == b"\x02\x00":
        print("     the vehicle data now: %s" % vehicle_text(resp[3:13]))
    elif sid == 0x22:
        print("     data: %s" % hexs(resp[1:]))
    elif sid == 0x27 and resp[1] == 0x01 and len(resp) == 6:
        seed = int.from_bytes(resp[2:6], "big")
        print("     seed 0x%08X%s" % (seed, "   (zero: already unlocked)" if seed == 0 else "   (the key is uds_key(seed) = 0x%08X)" % uds_key(seed)))
    elif sid == 0x27:
        print("     the key was accepted: security access is unlocked until the session changes")
    elif sid == 0x11:
        print("     the ECU resets: cluster_diag starts again (pending DTCs are gone, confirmed ones stay)")
    elif sid == 0x14:
        print("     DTC memory cleared")
    elif sid == 0x3E:
        print("     the ECU is there")
    return 0


def do(sock, dest, uds, functional, timeout):
    r = request(sock, dest, bytes(uds), functional, timeout)
    return explain(bytes(uds), r), r


def unlock(sock, dest, timeout):
    """Extended session, then seed and key. Returns 0 when security access is unlocked."""
    status, _ = do(sock, dest, [0x10, 0x03], False, timeout)
    if status:
        return status
    status, r = do(sock, dest, [0x27, 0x01], False, timeout)
    if status or r is None or len(r) != 6:
        return status or 2
    seed = int.from_bytes(r[2:6], "big")
    if seed == 0:
        return 0                                   # unlocked already
    key = uds_key(seed)
    status, _ = do(sock, dest, [0x27, 0x02] + list(key.to_bytes(4, "big")), False, timeout)
    return status


def reset(sock, dest, timeout):
    """Unlock, then hard reset, then ask for the uptime to see that the program started again."""
    status = unlock(sock, dest, timeout)
    if status:
        return status
    status, _ = do(sock, dest, [0x11, 0x01], False, timeout)
    if status:
        return status
    print("waiting 3 s for cluster_diag to start again")
    time.sleep(3)
    status, r = do(sock, dest, [0x22, 0x01, 0x00], False, timeout)
    if r is not None and len(r) == 7 and r[0] == 0x62:
        print("     uptime %d s: that is the new run" % int.from_bytes(r[3:7], "big"))
    return status


def demo_security(sock, dest, timeout):
    steps = [
        ("ECU reset in the default session: refused", [0x11, 0x01]),
        ("security access in the default session: refused", [0x27, 0x01]),
        ("extended session", [0x10, 0x03]),
        ("ECU reset without the unlock: security access denied", [0x11, 0x01]),
        ("key without a seed: request sequence error", [0x27, 0x02, 0, 0, 0, 0]),
    ]
    for title, uds in steps:
        print(title)
        do(sock, dest, uds, False, timeout)
    print("request a seed")
    status, r = do(sock, dest, [0x27, 0x01], False, timeout)
    if r is None or len(r) != 6:
        return 2
    seed = int.from_bytes(r[2:6], "big")
    print("a wrong key (one bit off): invalid key; the seed is spent")
    do(sock, dest, [0x27, 0x02] + list((uds_key(seed) ^ 1).to_bytes(4, "big")), False, timeout)
    print("request a new seed and answer with the right key")
    status, r = do(sock, dest, [0x27, 0x01], False, timeout)
    if r is None or len(r) != 6:
        return 2
    seed = int.from_bytes(r[2:6], "big")
    do(sock, dest, [0x27, 0x02] + list(uds_key(seed).to_bytes(4, "big")), False, timeout)
    print("a seed when unlocked is zero")
    do(sock, dest, [0x27, 0x01], False, timeout)
    print("a new session locks it again: reset is denied again")
    do(sock, dest, [0x10, 0x03], False, timeout)
    do(sock, dest, [0x11, 0x01], False, timeout)
    print("back to the default session")
    do(sock, dest, [0x10, 0x01], False, timeout)
    return 0


def demo(sock, dest, timeout):
    steps = [
        ("TesterPresent", [0x3E, 0x00]),
        ("session now (F186)", [0x22, 0xF1, 0x86]),
        ("software version (F195)", [0x22, 0xF1, 0x95]),
        ("uptime in seconds (0100)", [0x22, 0x01, 0x00]),
        ("VIN (F190): 20 bytes, sent in three CAN frames", [0x22, 0xF1, 0x90]),
        ("three DIDs in one request (7 bytes) and a 58-byte answer: the VIN three times", [0x22, 0xF1, 0x90, 0xF1, 0x90, 0xF1, 0x90]),
        ("a 9-byte request (first frame + consecutive frame), four DIDs", [0x22, 0xF1, 0x86, 0xF1, 0x95, 0x01, 0x00, 0xF1, 0x90]),
        ("unknown DID", [0x22, 0x12, 0x34]),
        ("unknown service", [0x27, 0x01]),
        ("how many DTCs", [0x19, 0x01, 0xFF]),
        ("which DTCs (more than one: a long answer)", [0x19, 0x02, 0xFF]),
        ("only the confirmed ones", [0x19, 0x02, 0x08]),
        ("clear in the default session: refused", [0x14, 0xFF, 0xFF, 0xFF]),
        ("extended session", [0x10, 0x03]),
        ("clear U0401 only", [0x14, 0xC4, 0x01, 0x00]),
        ("clear an unknown DTC", [0x14, 0x12, 0x34, 0x00]),
        ("clear everything", [0x14, 0xFF, 0xFF, 0xFF]),
        ("how many DTCs now", [0x19, 0x01, 0xFF]),
        ("back to the default session", [0x10, 0x01]),
    ]
    for title, uds in steps:
        print("%s" % title)
        do(sock, dest, uds, False, timeout)
    return 0


def s3_test(sock, dest, timeout):
    """The extended session ends 5 s after the last request; a TesterPresent keeps it alive."""
    bad = 0

    def session():
        r = request(sock, dest, bytes([0x22, 0xF1, 0x86]), False, timeout)
        return r[3] if r and r[0] == 0x62 and len(r) == 4 else None

    def check(what, ok):
        nonlocal bad
        bad += not ok
        print("  %s  %s" % ("PASS" if ok else "FAIL", what))

    print("extended session, then 6 s of silence")
    request(sock, dest, bytes([0x10, 0x03]), False, timeout)
    time.sleep(6)
    check("S3 (5 s) ended the extended session: F186 is 1", session() == 1)
    print("extended session with a TesterPresent every 2 s for 8 s")
    request(sock, dest, bytes([0x10, 0x03]), False, timeout)
    for _ in range(4):
        time.sleep(2)
        request(sock, dest, bytes([0x3E, 0x00]), False, timeout)
    check("the session is still extended after 8 s: F186 is 3", session() == 3)
    request(sock, dest, bytes([0x10, 0x01]), False, timeout)
    return 1 if bad else 0


def selftest():
    assert dtc_text(bytes([0xC1, 0x00])) == "U0100" and dtc_text(bytes([0xC4, 0x01])) == "U0401"
    assert dtc_text(bytes([0xDA, 0x03])) == "U1A03"
    assert dtc_value("U0401") == bytes([0xC4, 0x01, 0]) and dtc_value("U1A02") == bytes([0xDA, 0x02, 0])
    assert uds_key(0) == 0xF42CB29F and uds_key(0x12345678) == 0x0E07E6A8 and uds_key(0xDEADBEEF) == 0xC95C1D0A, "key vectors of cluster_uds.h"
    assert vehicle_text(bytes([0x01, 0xF4, 0x09, 0xC4, 0x82, 0x78, 0x03, 0x01, 0x00, 0x05])).startswith("speed 50.0 km/h, 2500 rpm, coolant 90 C, fuel 60.0 %, gear 3, mode COMFORT")
    f = sf_frame(0x7E0, [0x22, 0xF1, 0x86])
    assert len(f) == 16 and f[:5] == bytes([0xE0, 0x07, 0, 0, 8]) and f[8:] == bytes([3, 0x22, 0xF1, 0x86] + [PAD] * 4), hexs(f)
    assert parse_frame(f) == (0x7E0, f[8:16])
    assert parse_frame(f[:15]) is None
    assert status_text(0x27) == "testFailed testFailedThisOperationCycle pendingDTC testFailedSinceLastClear"
    print("selftest: frame and DTC conversions ok")


def main(argv):
    if argv and argv[0] == "--selftest":
        selftest()
        return 0

    def option(name, default, cast):
        if name in argv:
            i = argv.index(name)
            v = cast(argv[i + 1])
            del argv[i:i + 2]
            return v
        return default

    global FC_BS, FC_STMIN
    FC_BS = option("--bs", 0, int)
    FC_STMIN = option("--stmin", 0, int)
    port = option("--port", 5556, int)
    timeout = option("--timeout", 1.0, float)
    functional = "--functional" in argv
    if functional:
        argv.remove("--functional")
    if len(argv) < 2:
        print(__doc__)
        return 2
    ip, cmd, args = argv[0], argv[1], argv[2:]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (ip, port)

    try:
        if cmd == "session" and args and args[0] in ("default", "extended"):
            uds = [0x10, 0x01 if args[0] == "default" else 0x03]
        elif cmd == "tester-present":
            uds = [0x3E, 0x00]
        elif cmd == "read-did" and args:
            uds = [0x22]
            for a in args:
                uds += [int(a, 16) >> 8, int(a, 16) & 0xFF]
        elif cmd in ("dtc-count", "read-dtc"):
            uds = [0x19, 0x01 if cmd == "dtc-count" else 0x02, int(args[0], 16) if args else 0xFF]
        elif cmd == "clear-dtc":
            what = args[0] if args else "all"
            uds = [0x14] + (list(b"\xff\xff\xff") if what == "all" else list(dtc_value(what)))
        elif cmd == "raw" and args:
            uds = [int(a, 16) for a in args]
        elif cmd == "demo":
            return demo(sock, dest, timeout)
        elif cmd == "s3":
            return s3_test(sock, dest, timeout)
        elif cmd == "unlock":
            return unlock(sock, dest, timeout)
        elif cmd == "reset":
            return reset(sock, dest, timeout)
        elif cmd == "demo-security":
            return demo_security(sock, dest, timeout)
        elif cmd == "vehicle":
            uds = [0x22, 0x02, 0x00]
        elif cmd == "snapshot" and args:
            uds = [0x19, 0x04] + list(dtc_value(args[0])) + [int(args[1], 16) if len(args) > 1 else 0xFF]
        else:
            print(__doc__)
            return 2
    except (ValueError, IndexError) as e:
        print("bad command: %s" % e)
        return 2

    try:
        status, _ = do(sock, dest, uds, functional, timeout)
    except ValueError as e:
        print("bad command: %s" % e)
        return 2
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
