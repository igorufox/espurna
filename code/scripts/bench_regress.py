#!/usr/bin/env python3
"""Quick functional regression of an ESPurna board over the network, for any chip (ESP8266, ESP32, Beken).

Checks (each pass / FAIL / skip):
  discover    /discover answers JSON (hostname, app, version)
  webui       authenticated GET / -> 200 with a page body
  ws_terminal WS session + `help` -> +OK
  info        `info`: build line, md5, system OK (when SYSTEM_CHECK is built in)
  heap        `heap` parses (available / contiguous)
  wifi        STA connected, RSSI
  mqtt        client CONNECTED (skip when MQTT is not built in or mqttEnabled is 0)
  ntp         `ntp` -> synced (skip when NTP is not built in)
  api         GET /api/list with the board's apiKey -> 200, with a wrong key -> refused (skip when the API is off)
  settings    set / get / del / get of the temp key benchProbe (skip with --read-only)
  no_reboot   uptime kept growing during the run

The apiKey is read over the WS terminal into memory only and goes in the Api-Key header; nothing secret is printed.

    python code/scripts/bench_regress.py --host 10.0.5.139
"""
import http.client
import os
import time

import bench_common as B

TOOL = "bench_regress"
TEMP_KEY = "benchProbe"


class Checks:
    def __init__(self):
        self.rows = []

    def add(self, name, result, detail=""):
        """result: True (pass), False (FAIL), None (skip)."""
        self.rows.append((name, result, detail))
        B.log("%-12s %-5s %s" % (name, {True: "pass", False: "FAIL", None: "skip"}[result], detail))


def body(a):
    if not a.read_only:
        B.check_bench(a)
    dev = B.Device(a.host, a.port)
    c = Checks()

    d = dev.discover(timeout=5.0)
    c.add("discover", d is not None, "%s %s %s" % (d.get("hostname"), d.get("app"), d.get("version")) if d else "no answer")

    try:
        st, hdrs, page = dev.request("GET", "/", headers={"Accept-Encoding": "gzip"}, auth=True, timeout=20)
        c.add("webui", st == 200 and len(page) > 512, "HTTP %d, %d bytes%s" % (
            st, len(page), ", gzip" if hdrs.get("content-encoding") == "gzip" else ""))
    except (OSError, http.client.HTTPException) as e:
        c.add("webui", False, "%s: %s" % (type(e).__name__, e))

    try:
        t = B.Terminal(dev, settle=2.0)
    except (OSError, ConnectionError) as e:
        c.add("ws_terminal", False, "%s: %s" % (type(e).__name__, e))
        return finish(c)
    with t:
        lines, st = t.cmd("help")
        c.add("ws_terminal", B.is_ok(st), "help: %d lines, %s" % (len(lines), st))

        up0 = B.parse_uptime(t.cmd("uptime")[0])
        info = B.parse_info(t.cmd("info")[0])
        c.add("info", bool(info["app"]) and bool(info["md5"]) and info["system"] in ("OK", None),
              "%s, md5 %s, system %s, boot counter %s, last reset '%s'" % (
                  info["app"], info["md5"], info["system"], info["boot"], info["reason"]))

        h = B.parse_heap(t.cmd("heap")[0])
        c.add("heap", h["available"] is not None, "available %s contiguous %s%s" % (
            h["available"], h["contiguous"], ", lwip: %s" % h["lwip"] if h["lwip"] else ""))

        w = B.parse_wifi(t.cmd("wifi")[0])
        c.add("wifi", w["sta"] == "connected", "rssi %s channel %s bssid %s" % (w["rssi"], w["channel"], w["bssid"])
              if w["sta"] == "connected" else "STA %s" % w["sta"])

        lines, st = t.cmd("mqtt")
        enabled = B.parse_key_values(t.cmd("get mqttEnabled")[0]).get("mqttEnabled")
        if not B.is_ok(st):
            c.add("mqtt", None, "no mqtt command (%s)" % st)
        elif enabled in ("0", "false", "off"):
            c.add("mqtt", None, "mqttEnabled %s" % enabled)
        else:
            state = B.parse_mqtt(lines)
            c.add("mqtt", state == "CONNECTED", "client %s" % state)

        lines, st = t.cmd("ntp")
        if B.is_ok(st):
            c.add("ntp", True, next((l for l in lines if "local time" in l), "synced"))
        elif "not synced" in st.lower() or "waiting" in st.lower():
            c.add("ntp", False, st)
        else:
            c.add("ntp", None, "no ntp command (%s)" % st)

        key = dev.api_key(t)
        if not key:
            c.add("api", None, "apiEnabled off or apiKey empty")
        else:
            s1, text = dev.api("GET", "list", key)
            s2, _ = dev.api("GET", "list", "wrong-" + os.urandom(4).hex())
            c.add("api", s1 == 200 and s2 != 200, "list HTTP %d (%d paths), wrong key HTTP %d" % (
                s1, len([l for l in text.splitlines() if l.strip()]), s2))
            key = None

        if a.read_only:
            c.add("settings", None, "--read-only")
        else:
            value = "bench-" + os.urandom(4).hex()
            _, s_set = t.cmd("set %s %s" % (TEMP_KEY, value))
            got = B.parse_key_values(t.cmd("get %s" % TEMP_KEY)[0]).get(TEMP_KEY)
            _, s_del = t.cmd("del %s" % TEMP_KEY)
            gone = B.parse_key_values(t.cmd("get %s" % TEMP_KEY)[0]).get(TEMP_KEY)
            c.add("settings", B.is_ok(s_set) and got == value and B.is_ok(s_del) and not gone,
                  "set %s, read back %s, del %s, after del %r" % (
                      s_set, "ok" if got == value else repr(got), s_del, gone))

        time.sleep(1.0)
        up1 = B.parse_uptime(t.cmd("uptime")[0])
        c.add("no_reboot", up0 is not None and up1 is not None and up1 >= up0, "uptime %ss -> %ss" % (up0, up1))
    return finish(c)


def finish(c):
    passed = [n for n, r, _ in c.rows if r is True]
    failed = [n for n, r, _ in c.rows if r is False]
    skipped = [n for n, r, _ in c.rows if r is None]
    B.log("%s %d pass, %d FAIL %s, %d skip %s" % (B.stamp(), len(passed), len(failed), failed, len(skipped), skipped))
    return not failed, {"pass": len(passed), "fail": failed, "skip": skipped,
                        "checks": {n: {True: "pass", False: "fail", None: "skip"}[r] for n, r, _ in c.rows}}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("--read-only", action="store_true",
                   help="skip the temp-key write test (then also allowed on non-bench boards)")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
