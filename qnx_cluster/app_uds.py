"""The diagnostic side of the test app: a UDS client for cluster_diag that returns data, not text.

The frame and ISO-TP code is the one of uds_tester.py (send_message, recv_message). One request at a time
(a lock), because cluster_diag cannot tell two testers apart. A refresh thread asks for the session, the
uptime, the DTC list and the vehicle data about once a second, and a TesterPresent keeps an extended session
alive when you ask for it.
"""
import contextlib
import io
import socket
import threading
import time

import uds_tester as ut

DTC_INFO = {
    "U0100": "CAN 메시지가 0.5초 동안 전혀 없음 (통신 끊김)",
    "U0401": "게이트웨이가 SRC 결함을 알림 (프레임 거부됨)",
    "U1A01": "E2E: CRC 오류",
    "U1A02": "E2E: 카운터(순서) 오류",
    "U1A03": "E2E: 값이 말이 안 됨",
    "U1A04": "화면이 응답 없음 (qt_cluster의 하트비트 없음)",
}
SESSIONS = {1: "default", 3: "extended"}


def decode_vehicle(b):
    return {"speed": ((b[0] << 8) | b[1]) / 10.0, "rpm": (b[2] << 8) | b[3], "coolant": b[4] - 40, "fuel": b[5] / 2.0,
            "gear": b[6], "mode": ut.MODES[b[7]] if b[7] < 3 else "?", "warnings": (b[8] << 8) | b[9]}


def decode_dtcs(resp):
    """Response to 19 02: availability mask, then 4 bytes per DTC."""
    body, out = resp[3:], []
    for i in range(0, len(body) - 3, 4):
        code, st = ut.dtc_text(body[i:i + 2]), body[i + 3]
        out.append({"code": code, "status": st, "flags": [n for bit, n in ut.STATUS_BITS if st & bit],
                    "confirmed": bool(st & 0x08), "failing": bool(st & 0x01), "info": DTC_INFO.get(code, "")})
    return out


def decode_snapshot(resp):
    """Response to 19 04 -> {code, status, records: [{record, vehicle}]}"""
    out = {"code": ut.dtc_text(resp[2:4]), "status": resp[5], "records": []}
    body = resp[6:]
    while len(body) >= 2:
        rec, nid = body[0], body[1]
        body = body[2:]
        item = {"record": rec, "vehicle": None}
        for _ in range(nid):
            if len(body) >= 12 and body[:2] == b"\x02\x00":
                item["vehicle"] = decode_vehicle(body[2:12])
                body = body[12:]
            else:
                body = b""
        out["records"].append(item)
    return out


class Uds:
    def __init__(self, ip, port=5556):
        self.dest = (ip, port)
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.log = []                       # the last requests: {t, req, resp, note}
        self.refresh_on = True
        self.paused = False                 # a test job that talks to cluster_diag itself
        self.keepalive = False
        self.info = {"online": False, "session": None, "uptime": None, "dtcs": [], "vehicle": None, "vehicle_note": "",
                     "unlocked": None, "age": None}
        self.stop = False
        self.misses = 0                     # refreshes without an answer in a row
        self.threads = [threading.Thread(target=self.refresh_loop, daemon=True),
                        threading.Thread(target=self.keepalive_loop, daemon=True)]

    def start(self):
        for t in self.threads:
            t.start()

    def set_ip(self, ip):
        with self.lock:
            self.dest = (ip, self.dest[1])
            self.info["online"] = False

    # ---- one request ----
    def request(self, uds, timeout=1.0, note="", log=True):
        """Sends a request, waits for the answer (also for NRC 0x78). Returns the response bytes or None."""
        uds = bytes(uds)
        with self.lock:
            self.flush()
            buf = io.StringIO()
            resp = None
            try:
                with contextlib.redirect_stdout(buf):
                    refused = ut.send_message(self.sock, self.dest, uds, timeout)
                    if refused is not None:
                        resp = refused
                    else:
                        resp = ut.recv_message(self.sock, self.dest, timeout, 0, 0)
                        while resp is not None and len(resp) == 3 and resp[0] == 0x7F and resp[2] == 0x78:
                            resp = ut.recv_message(self.sock, self.dest, 5.0, 0, 0)
            except (ValueError, OSError) as e:
                note = (note + " " if note else "") + str(e)
            if log:
                self.log.append({"t": time.strftime("%H:%M:%S"), "req": ut.hexs(uds), "resp": ut.hexs(resp) if resp else "",
                                 "note": note or buf.getvalue().strip()})
                del self.log[:-60]
            return resp

    def flush(self):
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recvfrom(64)
        except (BlockingIOError, OSError):
            pass
        self.sock.setblocking(True)

    @staticmethod
    def nrc(resp):
        return resp[2] if resp is not None and len(resp) == 3 and resp[0] == 0x7F else None

    def result(self, uds, resp, extra=None):
        """A response in the form the page shows."""
        out = {"ok": False, "req": ut.hexs(uds), "resp": ut.hexs(resp) if resp else "", "text": ""}
        if resp is None:
            out["text"] = "응답 없음 (cluster_diag가 실행 중이고 IP가 맞나요?)"
        elif self.nrc(resp) is not None:
            n = self.nrc(resp)
            out["text"] = "부정 응답, NRC 0x%02X %s" % (n, ut.NRC.get(n, "?"))
            out["nrc"] = n
        elif resp[0] == uds[0] + 0x40:
            out["ok"] = True
            out["text"] = "긍정 응답"
        else:
            out["text"] = "예상 밖의 응답"
        if extra and out["ok"]:
            out.update(extra(resp))
        return out

    # ---- actions of the page ----
    def session(self, kind):
        uds = [0x10, 0x03 if kind == "extended" else 0x01]
        r = self.request(uds)
        out = self.result(uds, r)
        if out["ok"]:
            self.info.update(unlocked=False, session=kind)
        return out

    def tester_present(self):
        uds = [0x3E, 0x00]
        return self.result(uds, self.request(uds))

    def read_dtcs(self, mask=0xFF):
        uds = [0x19, 0x02, mask]
        return self.result(uds, self.request(uds), lambda r: {"dtcs": decode_dtcs(r)})

    def snapshot(self, code):
        uds = [0x19, 0x04] + list(ut.dtc_value(code)) + [0xFF]
        return self.result(uds, self.request(uds), lambda r: {"snapshot": decode_snapshot(r)})

    def vehicle(self):
        uds = [0x22, 0x02, 0x00]
        return self.result(uds, self.request(uds), lambda r: {"vehicle": decode_vehicle(r[3:13])})

    def read_did(self, did):
        uds = [0x22, did >> 8, did & 0xFF]
        return self.result(uds, self.request(uds), lambda r: {"data": ut.hexs(r[3:]), "ascii": bytes(r[3:]).decode("ascii", "replace")})

    def clear(self, what):
        """Clear all (what='all') or one DTC. Needs the extended session; the page opens it first."""
        r = self.request([0x10, 0x03])
        if r is None or self.nrc(r) is not None:
            return self.result([0x10, 0x03], r)
        uds = [0x14] + (list(b"\xff\xff\xff") if what == "all" else list(ut.dtc_value(what)))
        return self.result(uds, self.request(uds, 1.0, "지우기는 파일 쓰기를 기다립니다 (NRC 0x78)"))

    def unlock(self):
        steps = []
        r = self.request([0x10, 0x03])
        steps.append(self.result([0x10, 0x03], r))
        if not steps[-1]["ok"]:
            return {**steps[-1], "steps": steps}
        r = self.request([0x27, 0x01])
        steps.append(self.result([0x27, 0x01], r))
        if not steps[-1]["ok"] or len(r) != 6:
            return {**steps[-1], "ok": False, "steps": steps}
        seed = int.from_bytes(r[2:6], "big")
        if seed:
            uds = [0x27, 0x02] + list(ut.uds_key(seed).to_bytes(4, "big"))
            steps.append(self.result(uds, self.request(uds)))
        out = {**steps[-1], "steps": steps}
        if out["ok"]:
            out["text"] = "보안 접근이 풀렸습니다 (세션이 바뀔 때까지)"
            self.info.update(unlocked=True, session="extended")
        return out

    def reset(self):
        un = self.unlock()
        if not un["ok"]:
            return un
        uds = [0x11, 0x01]
        out = self.result(uds, self.request(uds))
        if out["ok"]:
            out["text"] = "ECU 리셋 수락: cluster_diag가 다시 시작합니다 (대기 중 DTC는 사라지고 확정된 DTC는 남음)"
            self.info["unlocked"] = False
        return out

    def raw(self, text):
        uds = bytes(int(x, 16) for x in text.replace(",", " ").split())
        if not uds:
            raise ValueError("바이트가 없습니다")
        return self.result(uds, self.request(uds))

    # ---- background threads ----
    def refresh_once(self):
        r = self.request([0x22, 0xF1, 0x86], 0.5, log=False)
        info = self.info
        if r is None:
            self.misses += 1
            if self.misses >= 3:
                info["online"] = False
            return
        self.misses = 0
        info["online"] = True
        if len(r) == 4 and r[0] == 0x62:
            info["session"] = SESSIONS.get(r[3], "0x%02X" % r[3])
            if r[3] == 1:
                info["unlocked"] = False
        r = self.request([0x22, 0x01, 0x00], 0.5, log=False)
        if r and len(r) == 7 and r[0] == 0x62:
            info["uptime"] = int.from_bytes(r[3:7], "big")
        r = self.request([0x19, 0x02, 0xFF], 0.5, log=False)
        if r and r[0] == 0x59:
            info["dtcs"] = decode_dtcs(r)
        r = self.request([0x22, 0x02, 0x00], 0.5, log=False)
        if r and len(r) == 13 and r[0] == 0x62:
            info["vehicle"], info["vehicle_note"] = decode_vehicle(r[3:13]), ""
        elif self.nrc(r) is not None:
            info["vehicle"], info["vehicle_note"] = None, "최신 차량 값이 없음 (NRC 0x%02X): 화면이 보내지 않고 있습니다" % self.nrc(r)

    def refresh_loop(self):
        while not self.stop:
            if self.refresh_on and not self.paused:
                try:
                    self.refresh_once()
                except OSError:
                    self.info["online"] = False
            time.sleep(1.0)

    def keepalive_loop(self):
        while not self.stop:
            time.sleep(2.0)
            if self.keepalive and not self.paused:
                self.request([0x3E, 0x00], 0.5, log=False)
