#!/usr/bin/env python3
"""Cross-check of cluster_diag with two independent open source libraries: can-isotp (the ISO-TP layer)
and udsoncan (the UDS client). uds_tester.py and cluster_uds.h / cluster_isotp.h were written by the same
person; if they misread the standard in the same way they would agree with each other. These libraries
were not.

Install them once into a virtual environment (Ubuntu does not allow pip in the system Python):
    python3 -m venv ~/uds-venv && ~/uds-venv/bin/pip install udsoncan can-isotp
Run it with the Python of that environment, with cluster_diag running on the target:
    ~/uds-venv/bin/python ~/qnx_cluster/uds_crosscheck.py 192.168.122.146

It changes the DTC memory in the middle (clear all) and restarts cluster_diag at the end (ECU reset). To see the
DTC list and the snapshot read, make the memory hold two DTCs first: run the crc fault of send_signal.py (U0401, and
U0100 when the sender stops), with qt_cluster running. The security lock-out test waits 11 s: about 50 s in all.
Exit status 0 when every check passed."""
import os
import select
import socket
import struct
import sys
import time

import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.exceptions import NegativeResponseException
from udsoncan.services import DiagnosticSessionControl, ECUReset

ID_REQ, ID_RESP = 0x7E0, 0x7E8
VIN = "QNXCLUSTER0000001"


def key_for(level, seed, params):
    """The key for a seed: the made-up algorithm of cluster_uds.h (written down here again, not shared with it)."""
    x = int.from_bytes(seed, "big") ^ 0xA5C3F00D
    return ((((x << 7) | (x >> 25)) & 0xFFFFFFFF) + 0x1234ABCD & 0xFFFFFFFF).to_bytes(4, "big")
passed = failed = 0


def check(what, ok, detail=""):
    global passed, failed
    passed += ok
    failed += not ok
    print("  %s  %s%s" % ("PASS" if ok else "FAIL", what, ("   (" + str(detail) + ")") if detail and not ok else ""))


def val(r, did):
    """The value of a DID; struct formats (">B", ">L") come back as a tuple."""
    v = r.service_data.values[did]
    return v[0] if isinstance(v, tuple) else v


class UdpCan:
    """The virtual CAN bus: one 16-byte can_frame per UDP datagram."""

    def __init__(self, ip, port=5556):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.dest = (ip, port)
        self.frames = []   # everything we sent and received, for the log

    def tx(self, msg):
        data = bytes(msg.data)
        self.frames.append(("tx", msg.arbitration_id, data))
        self.sock.sendto(struct.pack("<IB3x8s", msg.arbitration_id, len(data), data.ljust(8, b"\0")), self.dest)

    def rx(self, timeout):
        if not select.select([self.sock], [], [], timeout)[0]:
            return None
        d = self.sock.recvfrom(64)[0]
        can_id, dlc = struct.unpack_from("<IB", d)
        data = d[8:8 + dlc]
        self.frames.append(("rx", can_id, data))
        return isotp.CanMessage(arbitration_id=can_id, dlc=dlc, data=bytearray(data), extended_id=False)

    def frames_since(self, n, direction, first_byte):
        return [f for f in self.frames[n:] if f[0] == direction and f[2][:1] == first_byte]


def main(ip):
    bus = UdpCan(ip)
    address = isotp.Address(isotp.AddressingMode.Normal_11bits, txid=ID_REQ, rxid=ID_RESP)
    # the library's own flow control towards the server: blocks of 4 frames, 2 ms apart
    layer = isotp.TransportLayer(rxfn=bus.rx, txfn=bus.tx, address=address, params={"blocksize": 4, "stmin": 2})
    conn = PythonIsoTpConnection(layer)
    config = dict(udsoncan.configs.default_client_config)
    config["request_timeout"] = 2
    config["p2_timeout"] = 1.0
    # By default udsoncan replaces p2_timeout with the P2server the server announces (50 ms). The client must also allow
    # for the network delay (ISO 14229-2: P2client = P2server + delta P2). The VM is on the same PC and answers in about
    # 1 ms, so the strict 50 ms stays. The Raspberry Pi (QNX_ARCH=aarch64le) is on Wi-Fi with 25-55 ms round trips,
    # which alone can pass 50 ms, so there the client keeps its own 1 s.
    config["use_server_timing"] = os.environ.get("QNX_ARCH", "x86_64") != "aarch64le"
    config["data_identifiers"] = {0xF186: ">B", 0xF195: udsoncan.AsciiCodec(3), 0x0100: ">L", 0xF190: udsoncan.AsciiCodec(17),
                                  0x1234: ">B",   # 1234 does not exist on the server
                                  0x0200: ">HHBBBBH"}   # the vehicle data: speed, rpm, coolant, fuel, gear, mode, warnings
    config["security_algo"] = key_for
    config["security_algo_params"] = None
    with Client(conn, request_timeout=2, config=config) as c:
        print("session control and tester present")
        r = c.change_session(udsoncan.services.DiagnosticSessionControl.Session.defaultSession)
        check("10 01 accepted, session echo 1", r.positive and r.service_data.session_echo == 1)
        r = c.change_session(udsoncan.services.DiagnosticSessionControl.Session.extendedDiagnosticSession)
        check("10 03 accepted, session echo 3", r.positive and r.service_data.session_echo == 3)
        check("P2 server max 0.05 s, P2* 5 s", abs(r.service_data.p2_server_max - 0.05) < 1e-6 and abs(r.service_data.p2_star_server_max - 5.0) < 1e-6,
              (r.service_data.p2_server_max, r.service_data.p2_star_server_max))
        r = c.tester_present()
        check("3E 00 accepted", r.positive)

        print("read data by identifier")
        r = c.read_data_by_identifier(0xF186)
        check("F186 = 3 (extended session)", val(r, 0xF186) == 3, r.service_data.values)
        r = c.read_data_by_identifier(0xF195)
        check("F195 = '1.0'", val(r, 0xF195) == "1.0", r.service_data.values)
        r = c.read_data_by_identifier(0x0100)
        check("0100 = uptime in seconds, more than 0", val(r, 0x0100) > 0, r.service_data.values)

        print("long messages")
        n0 = len(bus.frames)
        r = c.read_data_by_identifier(0xF190)
        check("F190: the VIN, a 20-byte answer in a first frame and two consecutive frames", val(r, 0xF190) == VIN, r.service_data.values)
        check("   the server sent a first frame (10 14 ..) for it", bool(bus.frames_since(n0, "rx", b"\x10")))
        n0 = len(bus.frames)
        r = c.read_data_by_identifier([0xF186, 0xF195, 0x0100, 0xF190])
        v = r.service_data.values
        check("a 9-byte request (the library sends a first frame, the server a flow control) with a 34-byte answer",
              v[0xF186][0] == 3 and v[0xF195] == "1.0" and v[0xF190] == VIN and v[0x0100][0] > 0, v)
        check("   the server asked for the rest with a flow control frame (30 ..)", bool(bus.frames_since(n0, "rx", b"\x30")))
        n0 = len(bus.frames)
        resp = c.send_request(udsoncan.Request.from_payload(bytes([0x22, 0xF1, 0x90, 0xF1, 0x90, 0xF1, 0x90])))
        pl = resp.get_payload()
        check("the VIN three times: a 58-byte answer, which needs 8 consecutive frames", len(pl) == 58 and pl[:3] == bytes([0x62, 0xF1, 0x90]) and pl[3:20] == VIN.encode(), len(pl))
        check("   the library's flow control (block size 4) was needed twice",
              len(bus.frames_since(n0, "tx", b"\x30")) >= 2, [f[2].hex() for f in bus.frames[n0:] if f[0] == "tx"])

        print("negative responses (each one must come with the right NRC)")
        try:
            c.read_data_by_identifier(0x1234)
            check("unknown DID 1234", False, "no negative response")
        except NegativeResponseException as e:
            check("unknown DID 1234 -> NRC 0x31", e.response.code == 0x31, hex(e.response.code))
        try:
            c.send_request(udsoncan.Request.from_payload(bytes([0x22] + [0xF1, 0x90] * 7)))   # 1 + 7 x 19 = 134 bytes of answer
            check("an answer too long for the buffer -> NRC 0x14", False, "no negative response")
        except NegativeResponseException as e:
            check("an answer too long for the buffer (7 x VIN) -> NRC 0x14", e.response.code == 0x14, hex(e.response.code))
        try:
            c.send_request(udsoncan.Request.from_payload(bytes([0x2E, 0xF1, 0x90, 0x41])))   # write data: not implemented
            check("write data (0x2E) -> NRC 0x11", False, "no negative response")
        except NegativeResponseException as e:
            check("write data (0x2E, not implemented) -> NRC 0x11", e.response.code == 0x11, hex(e.response.code))

        print("a request longer than the server's buffer (131 bytes): the server must refuse")
        n0 = len(bus.frames)
        try:
            c.send_request(udsoncan.Request.from_payload(bytes([0x22] + [0xF1, 0x86] * 65)))
            check("131 bytes are refused", False, "no error")
        except Exception as e:
            check("server answered the first frame with flow control overflow (32 00 00)", bool(bus.frames_since(n0, "rx", b"\x32")), repr(e))

        print("read DTC information")
        r = c.get_number_of_dtc_by_status_mask(0xFF)
        count = r.service_data.dtc_count
        print("     (%d DTC(s) in the memory)" % count)
        check("19 01 FF answers, status availability mask 0x2F, format 14229-1",
              r.service_data.status_availability.get_byte_as_int() == 0x2F and r.service_data.dtc_format == 1,
              (r.service_data.status_availability.get_byte_as_int(), r.service_data.dtc_format))
        r = c.get_dtc_by_status_mask(0xFF)
        ids = sorted(d.id for d in r.service_data.dtcs)
        print("     DTCs: %s" % ", ".join("0x%06X (status 0x%02X)" % (d.id, d.status.get_byte_as_int()) for d in r.service_data.dtcs))
        check("19 02 FF lists as many DTCs as 19 01 counts, every fault type byte 0", len(ids) == count and all(i & 0xFF == 0 for i in ids), ids)
        if count == 2:
            check("   the two DTCs are U0100 (C10000) and U0401 (C40100)", ids == [0xC10000, 0xC40100], [hex(i) for i in ids])
        else:
            print("     (run the crc fault of send_signal.py first to get two DTCs: U0401 and U0100)")

        print("DTC snapshot (freeze frame)")
        if count == 2:
            r = c.get_dtc_snapshot_by_dtc_number(udsoncan.Dtc(0xC40100), record_number=0xFF)
            snaps = r.service_data.dtcs[0].snapshots
            vals = snaps[0].data if snaps else None
            print("     U0401 snapshot: %s" % (vals,))
            check("19 04 C40100 FF: one record, DID 0200, 10 bytes of vehicle data", len(snaps) == 1 and snaps[0].did == 0x0200 and snaps[0].record_number == 1, snaps)
            check("   about 50 km/h, 90 degC, fuel 60 %, gear 3, comfort mode, no warnings",
                  vals is not None and 400 <= vals[0] <= 600 and vals[2] == 130 and vals[3] == 120 and vals[4] == 3 and vals[5] == 1 and vals[6] == 0, vals)
        else:
            print("     (skipped: the memory should hold U0401 and U0100 with the vehicle data of the crc fault)")
        try:
            c.get_dtc_snapshot_by_dtc_number(udsoncan.Dtc(0x123400), record_number=0xFF)
            check("19 04 for an unknown DTC", False, "no negative response")
        except NegativeResponseException as e:
            check("19 04 for an unknown DTC -> NRC 0x31", e.response.code == 0x31, hex(e.response.code))

        print("clear (extended session): the server writes the DTC file, so it first answers 'response pending' (NRC 0x78)")
        n0 = len(bus.frames)
        r = c.clear_dtc(0xFFFFFF)
        check("14 FFFFFF accepted (after the pending answer)", r.positive)
        check("   the library saw a 7F 14 78 frame and waited for the real answer", any(f[2][:4] == bytes([0x03, 0x7F, 0x14, 0x78]) for f in bus.frames[n0:] if f[0] == "rx"),
              [f[2].hex() for f in bus.frames[n0:] if f[0] == "rx"])
        r = c.get_number_of_dtc_by_status_mask(0xFF)
        check("19 01 FF: 0 DTCs afterwards", r.service_data.dtc_count == 0, r.service_data.dtc_count)
        c.change_session(udsoncan.services.DiagnosticSessionControl.Session.defaultSession)
        try:
            c.clear_dtc(0xFFFFFF)
            check("14 FFFFFF in the default session is refused", False, "no negative response")
        except NegativeResponseException as e:
            check("14 FFFFFF in the default session -> NRC 0x7F", e.response.code == 0x7F, hex(e.response.code))

        print("session timer S3 (6 s of silence)")
        c.change_session(udsoncan.services.DiagnosticSessionControl.Session.extendedDiagnosticSession)
        time.sleep(6)
        r = c.read_data_by_identifier(0xF186)
        check("5 s without a request: back in the default session (F186 = 1)", val(r, 0xF186) == 1, r.service_data.values)

        print("security access 0x27")
        c.change_session(DiagnosticSessionControl.Session.extendedDiagnosticSession)
        try:
            c.ecu_reset(ECUReset.ResetType.hardReset)
            check("11 01 before the unlock", False, "no negative response")
        except NegativeResponseException as e:
            check("11 01 before the unlock -> NRC 0x33", e.response.code == 0x33, hex(e.response.code))
        r = c.unlock_security_access(1)
        check("the library asks for a seed (27 01), computes the key and sends it (27 02): unlocked", r.positive)
        r = c.send_request(udsoncan.Request.from_payload(bytes([0x27, 0x01])))
        check("27 01 when unlocked: a seed of zeros", r.positive and r.get_payload()[2:6] == bytes(4), r.get_payload().hex())
        c.change_session(DiagnosticSessionControl.Session.extendedDiagnosticSession)
        wrong = udsoncan.Request.from_payload(bytes([0x27, 0x02, 0, 0, 0, 0]))
        for i, want in enumerate((0x35, 0x35, 0x36)):
            c.send_request(udsoncan.Request.from_payload(bytes([0x27, 0x01])))
            try:
                c.send_request(wrong)
                check("a wrong key", False, "no negative response")
            except NegativeResponseException as e:
                check("wrong key %d -> NRC 0x%02X (%s)" % (i + 1, want, "invalid key" if want == 0x35 else "exceeded number of attempts"), e.response.code == want, hex(e.response.code))
        try:
            c.send_request(udsoncan.Request.from_payload(bytes([0x27, 0x01])))
            check("a seed during the lock-out", False, "no negative response")
        except NegativeResponseException as e:
            check("a seed request during the lock-out -> NRC 0x37 (time delay not expired)", e.response.code == 0x37, hex(e.response.code))
        print("     (waiting 11 s for the lock-out to end)")
        time.sleep(11)
        c.change_session(DiagnosticSessionControl.Session.extendedDiagnosticSession)
        r = c.unlock_security_access(1)
        check("after the delay: a seed again, unlocked with the right key", r.positive)

        print("ECU reset 0x11")
        before = val(c.read_data_by_identifier(0x0100), 0x0100)
        r = c.ecu_reset(ECUReset.ResetType.hardReset)
        check("11 01 after the unlock accepted (51 01)", r.positive)
        time.sleep(3)
        after = val(c.read_data_by_identifier(0x0100), 0x0100)
        check("cluster_diag started again: the uptime counts from 0 (%d s before, %d s after)" % (before, after), after < before, (before, after))
        r = c.read_data_by_identifier(0xF186)
        check("and it is in the default session (F186 = 1)", val(r, 0xF186) == 1, r.service_data.values)

    print("\ncross-check: %d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
