#!/usr/bin/env python3
"""ArduinoOTA (espota, UDP 8266) upload to an ESP8266 bench board: for images without a web OTA (minimal
ArduinoOTA image) and the softAP rescue of a board stuck in unstable mode (ArduinoOTA still runs there).
ESP32 is not supported on purpose: on the safeboot layout ArduinoOTA is off (it would write the running slot).

Uses the framework's own espota.py from the PlatformIO packages and calls its serve() in-process with the password
read from %USERPROFILE%\\.espurna\\pass (never on argv or in the output). The board connects BACK to this PC
(--host-port) after authentication: the Windows firewall here allows that for C:\\Python313\\python.exe only.
One attempt, never retried: a failed ArduinoOTA on an in-wall board is a stop-and-report (docs/bench.md).

When the board answers the WS terminal, uptime >= 70 s, system OK and a clean last reset reason are checked first;
a minimal image without a terminal is uploaded to as is (--no-precheck skips the check on purpose).

    C:\\Python313\\python.exe code/scripts/bench_espota.py --host 10.0.5.0 --host-port 16065 image.bin.gz
    softAP rescue (PC joined the board's softAP; 192.168.4.1 is not in the bench list, so --allow-non-bench):
    C:\\Python313\\python.exe code/scripts/bench_espota.py --host 192.168.4.1 --host-ip 192.168.4.2 \\
        --host-port 16065 --no-precheck --allow-non-bench image.bin
"""
import logging
import os
import random
import socket
import sys
import time

import bench_common as B

TOOL = "bench_espota"
OTA_PORT = 8266
ESPOTA_DIR = os.path.join(os.environ.get("USERPROFILE") or os.path.expanduser("~"), ".platformio", "packages",
                          "framework-arduinoespressif8266", "tools")
BAD_REASON = ("watchdog", "exception", "brownout", "crash", "panic", "wdt", "trap", "stability", "network stopped")


def local_ip_for(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, OTA_PORT))
        return s.getsockname()[0]
    finally:
        s.close()


def load_espota():
    if not os.path.isfile(os.path.join(ESPOTA_DIR, "espota.py")):
        raise B.SetupError("espota.py not found in %s" % ESPOTA_DIR)
    sys.path.insert(0, ESPOTA_DIR)
    import espota  # noqa: E402  (the framework's copy)
    return espota


def precheck(dev):
    try:
        st = B.read_status(dev)
    except (OSError, ConnectionError, B.SetupError) as e:
        B.log("no WS terminal (%s): minimal image? uploading without the uptime check" % type(e).__name__)
        return
    if st["uptime"] is not None and st["uptime"] < B.MIN_REBOOT_UPTIME:
        raise B.Refused("uptime %ss < %ds, wait" % (st["uptime"], B.MIN_REBOOT_UPTIME))
    if st["system"] not in ("OK", None):
        raise B.Refused("system %s" % st["system"])
    if st["reason"] and any(w in st["reason"].lower() for w in BAD_REASON):
        raise B.Refused("last reset reason '%s': the board rebooted by itself, no OTA (tell the user)" % st["reason"])
    B.log("precheck: %s, uptime %ss, system %s, last reset '%s'" % (st["app"], st["uptime"], st["system"],
                                                                   st["reason"]))


def body(a):
    B.check_bench(a)
    if not os.path.isfile(a.firmware):
        raise B.SetupError("firmware file not found: %s" % a.firmware)
    if not a.no_precheck:
        precheck(B.Device(a.host, a.port))
    espota = load_espota()
    pw = B.load_password()
    host_ip = a.host_ip or local_ip_for(a.host)
    host_port = a.host_port or random.randint(10000, 60000)
    logging.basicConfig(level=logging.DEBUG if a.debug else logging.INFO,
                        format="%(asctime)-8s [%(levelname)s]: %(message)s", datefmt="%H:%M:%S")
    espota.TIMEOUT = a.timeout
    B.log("espota: %s (%d bytes, md5 %s) -> %s:%d, listen %s:%d, python %s" % (
        a.firmware, os.path.getsize(a.firmware), B.file_md5(a.firmware), a.host, OTA_PORT, host_ip, host_port,
        sys.executable))
    t0 = time.time()
    try:
        rc = espota.serve(a.host, host_ip, OTA_PORT, host_port, pw, a.firmware, espota.FLASH)
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else 1
    dt = time.time() - t0
    B.log("espota rc=%s in %.1fs%s" % (rc, dt, "" if rc == 0 else " - NOT retried, report it"))
    return rc == 0, {"firmware": os.path.basename(a.firmware), "rc": rc, "seconds": round(dt, 1)}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("firmware")
    p.add_argument("--host-ip", default=None, help="local IP to listen on (default: the one routing to --host)")
    p.add_argument("--host-port", type=int, default=None, help="local TCP port the board connects back to")
    p.add_argument("--timeout", type=int, default=10, help="espota socket timeout in seconds (default 10)")
    p.add_argument("--no-precheck", action="store_true", help="skip the WS uptime / reset reason check")
    p.add_argument("--debug", action="store_true")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
