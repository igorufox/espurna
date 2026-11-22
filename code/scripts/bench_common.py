#!/usr/bin/env python3
"""Shared network access to an ESPurna board for the bench_*.py tools (docs/bench.md).

Not a tool by itself: `python bench_common.py --help` only prints this text.

Protocol (code/espurna/{web,ws,terminal,ota_web,api_common}.cpp):
  * HTTP auth: Digest (qop=auth), user "admin"; the nonce is not validated by the board, so one challenge is
    reused; a 401 refreshes it once. Minimal WebUI images have no auth at all (no /auth, only /upgrade).
  * WS: GET /auth (authenticated) registers a ticket for our IP (WS_MAX_CLIENTS tickets, 429 when full), then a
    plain WS upgrade on /ws. Terminal: {"action":"cmd","data":{"line":"<cmd>\\n"}}; output comes back as
    {"log":["...\\n"]} text frames and ends with a "+OK" or "-ERROR: ..." line.
  * OTA: POST multipart to /upgrade, form field "upgrade"; body "OK" on success.
  * API: /api/<path>, key in the "Api-Key" header (never in the URL).

Secrets:
  * the admin password is read from %USERPROFILE%\\.espurna\\pass at runtime; it is never printed, logged, put
    on a command line or into a URL;
  * values of secret-looking keys (pass*, wifiApPass, apiKey, mqttPassword, adminPass, *key*, *token*, ...) are
    redacted in every line and JSON object that reaches the output;
  * key comparison (bench_ota) uses salted hashes kept in memory only; the salt is random per run, so even a
    printed digest would be useless.

Exit codes of every tool: 0 pass, 1 fail (device or check), 2 setup error (password file, arguments, auth),
3 refused (host is not a bench board and --allow-non-bench was not given, or uptime too low for a reboot).
The last output line is always `BENCH_RESULT {json}`.
"""
import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

USER = "admin"
PASS_FILE = os.path.join(os.environ.get("USERPROFILE") or os.path.expanduser("~"), ".espurna", "pass")

# Bench boards (docs/bench.md). Anything else is somebody's in-wall / production board: the tools that load,
# write or reboot refuse it unless --allow-non-bench is given (only with the user's explicit OK).
BENCH_HOSTS = {
    "10.0.5.19": "ESP8266 HW-622 ESPURNA-D390FC (4 MB, one relay)",
    "10.0.5.0": "ESP8266 Sonoff Basic (1 MB)",
    "10.0.5.58": "ESP8266 Sonoff Basic ESPURNA-79E12A (1 MB)",
    "10.0.5.66": "ESP32 LC-Tech relay x2",
    "10.0.5.139": "Beken BK7231N (serial on COM3)",
    "10.0.4.175": "ESP32 DevKit on COM3 (ESP32-D0WDQ6 rev1, 26 MHz crystal), ESPURNA-173F9C",
}

# ESPurna counts soft restarts and resets the counter SYSTEM_CHECK_TIME (60 s) after boot; 3 restarts before that
# = unstable / safe mode. Reboots and OTA only at uptime >= 70 s.
MIN_REBOOT_UPTIME = 70

EXIT_OK, EXIT_FAIL, EXIT_SETUP, EXIT_REFUSED = 0, 1, 2, 3

try:
    sys.stdout.reconfigure(errors="replace", line_buffering=True)
except Exception:
    pass


class SetupError(Exception):
    """Password file, arguments, authentication: exit code 2."""


class Refused(Exception):
    """Safety refusal: exit code 3."""


# --------------------------------------------------------------------------- secrets
SECRET_NAME_RX = re.compile(r"pass|key|secret|token|pwd|psk", re.I)
_SECRET_LINE_RX = re.compile(
    r'^(\s*>?\s*"?)([\w.\-]*(?:pass|key|secret|token|pwd|psk)[\w.\-]*)("?\s*(?:=>|:|=)\s*)(.*?)'
    r'(\s*\((?:\*|default)\))?(,?)$', re.I)
_PW_CACHE = []
_SALT = os.urandom(16)


def load_password():
    if _PW_CACHE:
        return _PW_CACHE[0]
    if not os.path.isfile(PASS_FILE):
        raise SetupError("password file not found: %s (the admin password on one line)" % PASS_FILE)
    with open(PASS_FILE, "r", encoding="utf-8") as f:
        pw = f.read().rstrip("\r\n")
    if not pw:
        raise SetupError("password file is empty: %s" % PASS_FILE)
    _PW_CACHE.append(pw)
    return pw


def redact(line):
    """Hide values of secret-looking keys, and the admin password itself wherever it shows up."""
    line = str(line)
    m = _SECRET_LINE_RX.match(line)
    if m and not line.lstrip().lower().startswith("number of keys"):
        shown = "<redacted>" if m.group(4).strip().strip('"') else "<empty>"
        line = m.group(1) + m.group(2) + m.group(3) + shown + (m.group(5) or "") + m.group(6)
    for pw in _PW_CACHE:
        if pw and len(pw) >= 4 and pw in line:
            line = line.replace(pw, "<redacted>")
    return line


def redact_obj(o):
    """Redacted deep copy of a JSON object (WS dumps, /config, API replies)."""
    if isinstance(o, dict):
        return {k: ("<redacted>" if SECRET_NAME_RX.search(str(k)) and o[k] not in ("", None) else redact_obj(v))
                for k, v in o.items()}
    if isinstance(o, list):
        return [redact_obj(v) for v in o]
    if isinstance(o, str):
        return redact(o)
    return o


def salted_digest(value):
    """Comparable within this run only; never meaningful outside the process."""
    return hmac.new(_SALT, value.encode("utf-8", "replace"), hashlib.sha256).hexdigest()[:16]


def md5hex(s):
    return hashlib.md5(s.encode() if isinstance(s, str) else s).hexdigest()


def file_md5(path, unpack_gz=False):
    data = open(path, "rb").read()
    if unpack_gz and data[:2] == b"\x1f\x8b":
        import gzip
        data = gzip.decompress(data)
    return hashlib.md5(data).hexdigest()


# --------------------------------------------------------------------------- output
def log(msg):
    print(redact(msg), flush=True)


def stamp():
    return time.strftime("%H:%M:%S")


def emit_result(tool, host, ok, **fields):
    """The machine-readable last line: BENCH_RESULT {json}. Values go through redact_obj."""
    out = {"tool": tool, "host": host, "ok": bool(ok)}
    out.update(fields)
    print("BENCH_RESULT " + json.dumps(redact_obj(out), ensure_ascii=False, sort_keys=False), flush=True)


# --------------------------------------------------------------------------- HTTP
class Device:
    def __init__(self, host, port=80):
        self.host, self.port = host, port
        self._chal = None
        self._nc = 0

    def _conn(self, timeout=10):
        return http.client.HTTPConnection(self.host, self.port, timeout=timeout)

    @staticmethod
    def _parse_challenge(hdr):
        return dict(re.findall(r'(\w+)="?([^",]*)"?', hdr[6:]))

    def challenge(self, refresh=False):
        """Digest parameters; {} for a minimal WebUI image (no authentication)."""
        if self._chal is None or refresh:
            self._chal = None
            for method, uri in (("GET", "/auth"), ("POST", "/upgrade")):
                c = self._conn()
                try:
                    c.request(method, uri, body=b"" if method == "POST" else None)
                    r = c.getresponse()
                    r.read()
                finally:
                    c.close()
                hdr = r.getheader("WWW-Authenticate") or ""
                if r.status == 401 and hdr.lower().startswith("digest"):
                    self._chal = self._parse_challenge(hdr)
                    break
                if method == "POST" and r.status != 404:
                    self._chal = {}  # minimal WebUI image: no authentication at all
                    break
            if self._chal is None:
                raise SetupError("no Digest challenge from /auth or /upgrade (HTTP %d)" % r.status)
        return self._chal

    def auth_header(self, method, uri):
        ch = self.challenge()
        if not ch:
            return None
        self._nc += 1
        nc = "%08x" % self._nc
        cnonce = os.urandom(8).hex()
        realm, nonce = ch.get("realm", ""), ch.get("nonce", "")
        ha1 = md5hex("%s:%s:%s" % (USER, realm, load_password()))
        ha2 = md5hex("%s:%s" % (method, uri))
        resp = md5hex("%s:%s:%s:%s:auth:%s" % (ha1, nonce, nc, cnonce, ha2))
        h = ('Digest username="%s", realm="%s", nonce="%s", uri="%s", algorithm=MD5, '
             'response="%s", qop=auth, nc=%s, cnonce="%s"' % (USER, realm, nonce, uri, resp, nc, cnonce))
        if ch.get("opaque"):
            h += ', opaque="%s"' % ch["opaque"]
        return h

    def request(self, method, uri, body=None, headers=None, auth=False, timeout=10, retries_503=3):
        """One HTTP request -> (status, headers dict, body bytes). Retries on 503 (loop busy) and once on 401."""
        refreshed = False
        attempt = 0
        while True:
            h = dict(headers or {})
            if auth:
                a = self.auth_header(method, uri)
                if a:
                    h["Authorization"] = a
            c = self._conn(timeout)
            try:
                c.request(method, uri, body=body, headers=h)
                r = c.getresponse()
                data = r.read()
                hdrs = {k.lower(): v for k, v in r.getheaders()}
            finally:
                c.close()
            if r.status == 401 and auth and not refreshed:
                www = hdrs.get("www-authenticate", "")
                self._chal = self._parse_challenge(www) if www.lower().startswith("digest") else None
                refreshed = True
                continue
            if r.status == 503 and attempt < retries_503:
                attempt += 1
                time.sleep(1.0 + attempt)
                continue
            return r.status, hdrs, data

    def get(self, uri, auth=False, timeout=10):
        st, _, body = self.request("GET", uri, auth=auth, timeout=timeout)
        return st, body

    def discover(self, timeout=2.0):
        """/discover JSON (hostname, device, app, version) or None; no auth needed."""
        try:
            st, body = self.get("/discover", timeout=timeout)
            return json.loads(body) if st == 200 else None
        except (OSError, http.client.HTTPException, ValueError):
            return None

    def ws_ticket(self):
        st, _ = self.get("/auth", auth=True)
        if st == 401:
            raise SetupError("authentication failed (HTTP 401) - check the password file")
        if st == 429:
            raise ConnectionError("/auth: HTTP 429, all WS tickets in use")
        if st != 200:
            raise ConnectionError("/auth: HTTP %d" % st)

    def upload(self, path, progress=True):
        """Web OTA: multipart POST /upgrade. -> (ok, response text, seconds). Never retried here."""
        data = open(path, "rb").read()
        self.challenge(refresh=True)  # fresh nonce, a big body cannot be resent on 401
        boundary = "----bench" + os.urandom(8).hex()
        head = ('--%s\r\nContent-Disposition: form-data; name="upgrade"; filename="%s"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n' % (boundary, os.path.basename(path))).encode()
        tail = ("\r\n--%s--\r\n" % boundary).encode()
        c = self._conn(timeout=60)
        c.putrequest("POST", "/upgrade")
        a = self.auth_header("POST", "/upgrade")
        if a:
            c.putheader("Authorization", a)
        c.putheader("Content-Type", "multipart/form-data; boundary=" + boundary)
        c.putheader("Content-Length", str(len(head) + len(data) + len(tail)))
        c.endheaders()
        t0, sent, last = time.time(), 0, -1
        try:
            c.send(head)
            for i in range(0, len(data), 4096):
                chunk = data[i:i + 4096]
                c.send(chunk)
                sent = i + len(chunk)
                pct = sent * 100 // max(1, len(data))
                if progress and pct != last and pct % 10 == 0:
                    last = pct
                    sys.stdout.write("\r  upload %3d%%  %d/%d bytes  %.1fs" % (pct, sent, len(data), time.time() - t0))
                    sys.stdout.flush()
            c.send(tail)
            if progress:
                print()
            r = c.getresponse()
            body = r.read().decode(errors="replace").strip()
            status = r.status
        except (ConnectionError, socket.timeout, http.client.HTTPException) as e:
            if progress:
                print()
            return False, "connection error after %d/%d bytes: %s" % (sent, len(data), e), time.time() - t0
        finally:
            c.close()
        ok = status == 200 and (body == "OK" or "Rebooting" in body)  # minimal image replies with a refresh page
        return ok, "HTTP %d %s" % (status, body[:80]), time.time() - t0

    def api_key(self, term):
        """apiKey of the board, read over the WS terminal into memory only (None if empty / API off)."""
        lines, st = term.cmd("get apiKey apiEnabled", redact_output=False)
        vals = parse_key_values(lines)
        key, enabled = vals.get("apiKey"), vals.get("apiEnabled")
        if not key or (enabled is not None and enabled not in ("1", "true", "on", "y", "yes")):
            return None
        return key

    def api(self, method, path, key, value=None, timeout=10):
        """/api/<path> with the key in the Api-Key header. PUT sends value= as a form body."""
        uri = "/api/" + path.lstrip("/")
        headers = {"Api-Key": key, "Accept": "*/*"}  # basic handlers want text, JSON ones json; */* fits both
        body = None
        if value is not None:
            body = "value=%s" % value
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        st, _, data = self.request(method, uri, body=body, headers=headers, timeout=timeout)
        return st, data.decode(errors="replace")


# --------------------------------------------------------------------------- WebSocket
class WS:
    def __init__(self, host, port=80, path="/ws", timeout=10, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, key))
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("WS handshake: connection closed")
            buf += chunk
        hdr, self.buf = buf.split(b"\r\n\r\n", 1)
        first = hdr.split(b"\r\n", 1)[0]
        if b" 101 " not in first:
            raise ConnectionError("WS handshake failed: %r" % first)
        self.closed = None
        self._frag = b""

    def _send_frame(self, opcode, payload=b""):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | opcode])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)

    def send_json(self, obj):
        # UTF-8 as is: the board does not decode \uXXXX escapes
        self._send_frame(0x1, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def _need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([self.sock], [], [], left)
            if not r:
                return False
            chunk = self.sock.recv(65536)
            if not chunk:
                self.closed = self.closed or "TCP connection closed by device"
                return False
            self.buf += chunk
        return True

    def recv(self, timeout):
        """Next complete text message (str), or None on timeout / close."""
        deadline = time.time() + timeout
        while self.closed is None:
            if not self._need(2, deadline):
                return None
            b0, b1 = self.buf[0], self.buf[1]
            n, off = b1 & 0x7F, 2
            if n == 126:
                if not self._need(4, deadline):
                    return None
                n, off = struct.unpack(">H", self.buf[2:4])[0], 4
            elif n == 127:
                if not self._need(10, deadline):
                    return None
                n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
            if b1 & 0x80:
                off += 4
            if not self._need(off + n, max(deadline, time.time() + 5)):
                return None
            payload = self.buf[off:off + n]
            if b1 & 0x80:
                m = self.buf[off - 4:off]
                payload = bytes(c ^ m[i & 3] for i, c in enumerate(payload))
            self.buf = self.buf[off + n:]
            op, fin = b0 & 0x0F, b0 & 0x80
            if op == 0x9:
                self._send_frame(0xA, payload)
            elif op == 0x8:
                code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else None
                self.closed = "WS close frame from device (code %s)" % code
                try:
                    self._send_frame(0x8, payload[:2])
                except OSError:
                    pass
                return None
            elif op in (0x0, 0x1, 0x2):
                self._frag += payload
                if fin:
                    msg, self._frag = self._frag, b""
                    return msg.decode("utf-8", errors="replace")
        return None

    def drain(self, seconds):
        """Read and drop messages for `seconds` (the initial dump after connect). -> number of messages."""
        n, end = 0, time.time() + seconds
        while time.time() < end and self.closed is None:
            if self.recv(max(0.05, end - time.time())) is not None:
                n += 1
        return n

    def close(self):
        try:
            if self.closed is None:
                self._send_frame(0x8, struct.pack(">H", 1000))
            self.sock.close()
        except OSError:
            pass


def ws_open(dev, ticket=True, retries=3, rcvbuf=None):
    for attempt in range(retries + 1):
        if ticket:
            dev.ws_ticket()
        try:
            return WS(dev.host, dev.port, rcvbuf=rcvbuf)
        except ConnectionError as e:
            if " 503 " not in str(e) or attempt == retries:
                raise
            time.sleep(1.0 + attempt)


def log_lines(msg, redact_output=True):
    """Terminal / debug output lines from a WS text message."""
    try:
        obj = json.loads(msg)
    except ValueError:
        return []
    if not isinstance(obj, dict):
        return []
    out = []
    for chunk in obj.get("log", []) or []:
        out.extend(str(chunk).splitlines())
    if "message" in obj:
        out.append("[message] %s" % obj["message"])
    return [redact(l) for l in out] if redact_output else out


class Terminal:
    """One WS session for terminal commands. Use as a context manager."""

    def __init__(self, dev, settle=0.0):
        self.dev = dev
        self.ws = ws_open(dev)
        if settle:
            self.ws.drain(settle)

    def cmd(self, line, idle=5.0, cap=60.0, redact_output=True, echo=False):
        """Send one command, collect its output until +OK / -ERROR (or idle). -> (lines, status).

        redact_output=False keeps raw values for in-process use only (key hashing, apiKey); such lines must
        never be printed."""
        ws = self.ws
        ws.send_json({"action": "cmd", "data": {"line": line + "\n"}})
        lines, t0, last, done = [], time.time(), time.time(), None
        while time.time() - t0 < cap:
            wait = 0.3 if done else idle - (time.time() - last)
            if wait <= 0:
                break
            msg = ws.recv(wait)
            if msg is None:
                if ws.closed or done or time.time() - last >= idle:
                    break
                continue
            new = log_lines(msg, redact_output=redact_output)
            if not new:
                continue
            last = time.time()
            for l in new:
                lines.append(l)
                if echo:
                    print("  " + redact(l))
                if done is None and (l.startswith("+OK") or l.startswith("-ERROR")):
                    done = l
        status = done or ("WS closed: %s" % ws.closed if ws.closed else "idle %.0fs, no +OK/-ERROR" % idle)
        return lines, status

    def close(self):
        self.ws.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def is_ok(status):
    return bool(status) and status.startswith("+OK")


def run_cmds(dev, cmds, echo=False):
    """Commands in one WS session -> ({cmd: (lines, status)}, closed reason or None)."""
    results = {}
    with Terminal(dev) as t:
        for c in cmds:
            if echo:
                print("> %s" % c)
            results[c] = t.cmd(c, echo=echo)
            if echo:
                print("  [%s]" % results[c][1])
            if t.ws.closed:
                break
        closed = t.ws.closed
    return results, closed


# --------------------------------------------------------------------------- parsing
def grab(lines, rx, flags=0):
    for l in lines:
        m = re.search(rx, l, flags)
        if m:
            return m.group(1).strip()
    return None


def parse_uptime(lines):
    """'uptime 1h 2m 9s' -> 3729 (seconds), None if not found."""
    s = grab(lines, r"^uptime\s+(.+)$")
    if s is None:
        return None
    mult = {"w": 604800, "d": 86400, "h": 3600, "m": 60, "s": 1}
    return sum(int(v) * mult[u] for v, u in re.findall(r"(\d+)([wdhms])", s))


def parse_heap(lines):
    """'initial: N available: N contiguous: N' (+ 'lwip: ...' on LibreTiny) -> dict of ints / str."""
    out = {"initial": None, "available": None, "contiguous": None, "lwip": None}
    for k in ("initial", "available", "contiguous"):
        v = grab(lines, r"\b%s:\s*(\d+)" % k)
        out[k] = int(v) if v is not None else None
    out["lwip"] = grab(lines, r"^\s*lwip:\s*(.*)$")
    return out


def parse_info(lines):
    return {
        "app": grab(lines, r"^(\S+ \S+) built "),
        "device": grab(lines, r"^device:\s*(.*)$"),
        "md5": grab(lines, r"^md5:\s*(\S+)"),
        "reason": grab(lines, r"last reset reason:\s*(.*)$"),
        "system": grab(lines, r"^system:\s*(\S+)"),
        "boot": grab(lines, r"boot counter:\s*(\d+)"),
    }


def parse_wifi(lines):
    """`wifi` -> STA rssi / bssid / channel, or the STA state when not connected."""
    out = {"rssi": None, "bssid": None, "channel": None, "sta": None}
    for l in lines:
        m = re.match(r"^STA:\s*bssid\s+(\S+)\s+rssi\s+(-?\d+)\s+channel\s+(\d+)", l)
        if m:
            out.update(bssid=m.group(1), rssi=int(m.group(2)), channel=int(m.group(3)), sta="connected")
            break
        m = re.match(r"^STA:\s*(\w+)\s*$", l)
        if m:
            out["sta"] = m.group(1)
    return out


def parse_mqtt(lines):
    """`mqtt` -> 'CONNECTED' / 'CONNECTING' / 'DISCONNECTED' / ... or None (no MQTT support)."""
    s = grab(lines, r"^client\s+(\S+)")
    return s.upper() if s else None


_KEY_LINE_RX = re.compile(r'^\s*>\s*(\S+)\s*=>\s?(.*?)(?:\s*\((?:\*|default)\))?$')


def parse_key_values(lines):
    """'> name => "value"' lines of keys / get -> {name: value} (quotes stripped). Raw values: memory only."""
    out = {}
    for l in lines:
        m = _KEY_LINE_RX.match(l)
        if m:
            v = m.group(2)
            if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                v = v[1:-1]
            out[m.group(1)] = v
    return out


def keys_fingerprint(term):
    """{key name: salted digest of the value} from `keys`. Values never leave this function."""
    lines, st = term.cmd("keys", redact_output=False, cap=90.0)
    if not is_ok(st):
        raise ConnectionError("keys: %s" % redact(st))
    return {k: salted_digest(v) for k, v in parse_key_values(lines).items()}


def diff_fingerprints(before, after, ignore_rx=None):
    """-> {"added": [...], "removed": [...], "changed": [...]} (key names only)."""
    rx = re.compile(ignore_rx) if ignore_rx else None
    keep = (lambda k: not (rx and rx.search(k)))
    b = {k: v for k, v in before.items() if keep(k)}
    a = {k: v for k, v in after.items() if keep(k)}
    return {
        "added": sorted(set(a) - set(b)),
        "removed": sorted(set(b) - set(a)),
        "changed": sorted(k for k in set(a) & set(b) if a[k] != b[k]),
    }


def sample(term):
    """One status sample over an open terminal: uptime, heap, wifi, mqtt."""
    s = {"t": time.time()}
    lines, _ = term.cmd("uptime", idle=4.0, cap=15.0)
    s["uptime"] = parse_uptime(lines)
    lines, _ = term.cmd("heap", idle=4.0, cap=15.0)
    s.update(parse_heap(lines))
    lines, _ = term.cmd("wifi", idle=4.0, cap=15.0)
    s.update(parse_wifi(lines))
    lines, st = term.cmd("mqtt", idle=4.0, cap=15.0)
    s["mqtt"] = parse_mqtt(lines) if is_ok(st) else None
    return s


def heap_median(term, k=5, pause=1.0):
    """Median of k `heap` readings (available, contiguous). ESP8266 heap swings +-4 KB between single readings."""
    avail, contig = [], []
    for i in range(k):
        h = parse_heap(term.cmd("heap", idle=4.0, cap=15.0)[0])
        if h["available"] is not None:
            avail.append(h["available"])
        if h["contiguous"] is not None:
            contig.append(h["contiguous"])
        if i + 1 < k:
            time.sleep(pause)
    return {"available": median(avail), "contiguous": median(contig), "n": len(avail),
            "min": min(avail) if avail else None, "max": max(avail) if avail else None}


def median(values):
    v = sorted(values)
    if not v:
        return None
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) // 2


def slope_per_hour(points):
    """Least-squares slope of (t_seconds, value) points, in value units per hour. None for < 3 points."""
    pts = [(t, v) for t, v in points if v is not None]
    if len(pts) < 3:
        return None
    n = len(pts)
    mt = sum(t for t, _ in pts) / n
    mv = sum(v for _, v in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    if not den:
        return None
    return sum((t - mt) * (v - mv) for t, v in pts) / den * 3600.0


# --------------------------------------------------------------------------- reboot / OTA helpers
def read_status(dev):
    """info + uptime in one session -> dict (info fields + uptime)."""
    res, closed = run_cmds(dev, ["info", "uptime"])
    st = parse_info(res.get("info", ([], ""))[0])
    st["uptime"] = parse_uptime(res.get("uptime", ([], ""))[0])
    st["closed"] = closed
    return st


def wait_stable(dev, min_uptime=MIN_REBOOT_UPTIME, timeout=240.0):
    """Block until uptime >= min_uptime, system OK and boot counter 1 (when the image reports them).

    -> (status dict, None) or (status dict, reason-to-stop)."""
    t0 = time.time()
    while True:
        st = read_status(dev)
        up, system, boot = st["uptime"], st["system"], st["boot"]
        if system and system != "OK":
            return st, "system %s" % system
        if up is not None and up >= min_uptime and system in ("OK", None) and boot in ("1", None):
            return st, None
        waited = time.time() - t0
        if waited > timeout:
            return st, "not stable after %.0fs (uptime %s, system %s, boot counter %s)" % (waited, up, system, boot)
        pause = min(max(min_uptime - up + 1, 3), 30) if (up is not None and up < min_uptime) else 5
        log("  %s waiting %.0fs (uptime %s, system %s, boot counter %s)" % (stamp(), pause, up, system, boot))
        time.sleep(pause)


def wait_back(dev, t_start, timeout=180.0):
    """After an upload: wait until /discover goes away and answers again. -> (seconds or None, went_down)."""
    went_down = False
    while time.time() - t_start < timeout:
        d = dev.discover()
        if d is None:
            went_down = True
        elif went_down:
            return time.time() - t_start, True
        elif time.time() - t_start > 30:
            return time.time() - t_start, False  # never seen offline
        time.sleep(0.5)
    return None, went_down


def tcp_probe(host, ports=(80, 23)):
    out = {}
    for port in ports:
        try:
            s = socket.create_connection((host, port), timeout=3)
            s.close()
            out[port] = "open"
        except OSError as e:
            out[port] = type(e).__name__
    return out


# --------------------------------------------------------------------------- ping
def ping_once(host, timeout_ms=1000):
    """One ICMP echo through the system ping (no raw sockets needed). True on a reply."""
    if os.name == "nt":
        cmd = ["ping", "-n", "1", "-w", str(timeout_ms), host]
    else:
        cmd = ["ping", "-c", "1", "-W", str(max(1, timeout_ms // 1000)), host]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_ms / 1000.0 + 5,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and "ttl=" in r.stdout.lower()


class Pinger(threading.Thread):
    """Background ping every `interval` s; counts losses and the longest run of consecutive losses."""

    def __init__(self, host, interval=1.0):
        super().__init__(daemon=True)
        self.host, self.interval = host, interval
        self.sent = self.lost = 0
        self.run_now = self.run_max = 0
        self.loss_times = []
        self._stop_ev = threading.Event()

    def run(self):
        while not self._stop_ev.is_set():
            t0 = time.time()
            ok = ping_once(self.host)
            self.sent += 1
            if ok:
                self.run_now = 0
            else:
                self.lost += 1
                self.run_now += 1
                self.run_max = max(self.run_max, self.run_now)
                self.loss_times.append(t0)
            self._stop_ev.wait(max(0.0, self.interval - (time.time() - t0)))

    def stop(self):
        self._stop_ev.set()
        self.join(5)
        return {"sent": self.sent, "lost": self.lost, "longest_gap": self.run_max}


# --------------------------------------------------------------------------- CLI plumbing
def make_parser(description, writes=True):
    p = argparse.ArgumentParser(description=description, formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="Admin password: %s (never printed). Bench boards: %s. Docs: docs/bench.md"
                                       % (PASS_FILE, ", ".join(BENCH_HOSTS)))
    p.add_argument("--host", required=True, help="board IP")
    p.add_argument("--port", type=int, default=80)
    if writes:
        p.add_argument("--allow-non-bench", action="store_true",
                       help="run on a board that is not in the bench list - ONLY with the user's explicit OK")
    return p


def check_bench(args):
    """Refuse non-bench hosts for tools that load, write or reboot."""
    if args.host in BENCH_HOSTS:
        log("bench board %s: %s" % (args.host, BENCH_HOSTS[args.host]))
        return
    if getattr(args, "allow_non_bench", False):
        log("WARNING: %s is not a bench board; running because --allow-non-bench was given" % args.host)
        return
    raise Refused("%s is not a bench board (%s); --allow-non-bench only with the user's explicit OK"
                  % (args.host, ", ".join(BENCH_HOSTS)))


def run_tool(tool, args, body):
    """Run body(args) -> (ok, fields). Maps exceptions to exit codes; always prints BENCH_RESULT last."""
    host = getattr(args, "host", None)
    try:
        ok, fields = body(args)
    except KeyboardInterrupt:
        emit_result(tool, host, False, error="interrupted")
        return EXIT_FAIL
    except Refused as e:
        log("REFUSED: %s" % e)
        emit_result(tool, host, False, refused=str(e))
        return EXIT_REFUSED
    except SetupError as e:
        log("ERROR: %s" % e)
        emit_result(tool, host, False, error=str(e))
        return EXIT_SETUP
    except (OSError, http.client.HTTPException, ConnectionError) as e:
        log("NETWORK ERROR: %s: %s" % (type(e).__name__, e))
        emit_result(tool, host, False, error="%s: %s" % (type(e).__name__, e))
        return EXIT_FAIL
    emit_result(tool, host, ok, **fields)
    return EXIT_OK if ok else EXIT_FAIL


if __name__ == "__main__":
    print(__doc__)
