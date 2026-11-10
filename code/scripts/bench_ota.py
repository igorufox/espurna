#!/usr/bin/env python3
"""Web OTA of one image file to a bench board, then verify it came back right.

Before the upload (refuses otherwise): bench board (or --allow-non-bench with the user's OK), uptime >= 70 s,
system OK, boot counter 1, and a last reset reason that is not a crash / watchdog / brownout / stability reboot
(a board that rebooted by itself is unstable: no OTA, tell the user). Settings keys are fingerprinted: key names +
salted hashes of the values, kept in memory only, never printed.

The upload is done ONCE. A failed upload or a board that does not come back is reported, never retried.

After: the board went offline and came back, `last reset reason` is the OTA one, uptime is fresh, system OK,
build line / md5 reported (compared with --expect-md5, or with the file's md5 by --check-md5 for images whose
running md5 equals the file's, i.e. ESP8266 .bin / unpacked .bin.gz), keys unchanged (names and values).

    python code/scripts/bench_ota.py --host 10.0.5.19 --check-md5 path/to/espurna-esp8266-4m.bin
"""
import os
import re
import time

import bench_common as B

TOOL = "bench_ota"
OTA_REASON = "Reboot after a successful OTA update"
BAD_REASON_RX = re.compile(r"watchdog|exception|brownout|crash|panic|wdt|trap|stability|network stopped", re.I)


def body(a):
    B.check_bench(a)
    if not os.path.isfile(a.firmware):
        raise B.SetupError("firmware file not found: %s" % a.firmware)
    size = os.path.getsize(a.firmware)
    with open(a.firmware, "rb") as f:
        first = f.read(1)
    want_md5 = a.expect_md5.lower() if a.expect_md5 else (B.file_md5(a.firmware, unpack_gz=True) if a.check_md5 else None)
    B.log("%s image %s: %d bytes, file md5 %s, first byte 0x%02x%s" % (
        B.stamp(), a.firmware, size, B.file_md5(a.firmware), first[0] if first else 0,
        ", expected running md5 %s" % want_md5 if want_md5 else ""))
    dev = B.Device(a.host, a.port)

    st, why = B.wait_stable(dev, min_uptime=max(a.min_uptime, B.MIN_REBOOT_UPTIME), timeout=a.stable_timeout)
    if why:
        raise B.Refused("board not ready for OTA: %s" % why)
    if st["reason"] and BAD_REASON_RX.search(st["reason"]) and not a.ignore_reset_reason:
        raise B.Refused("last reset reason is '%s': the board rebooted by itself, no OTA (tell the user; "
                        "--ignore-reset-reason only with the user's OK)" % st["reason"])
    with B.Terminal(dev) as t:
        keys0 = B.keys_fingerprint(t)
        heap0 = B.heap_median(t, k=3)
    B.log("%s before: %s, md5 %s, uptime %ss, reset reason '%s', system %s, %d keys, heap median %s" % (
        B.stamp(), st["app"], st["md5"], st["uptime"], st["reason"], st["system"], len(keys0), heap0["available"]))

    ok_up, resp, dt = dev.upload(a.firmware, progress=not a.quiet)
    B.log("%s upload: %s in %.1fs" % (B.stamp(), resp, dt))
    fields = {"firmware": os.path.basename(a.firmware), "size": size, "upload_s": round(dt, 1),
              "app_before": st["app"], "md5_before": st["md5"]}
    if not ok_up:
        B.log("upload refused or broken - NOT retried")
        fields.update(stage="upload", response=resp)
        return False, fields
    t_up = time.time()
    back, down = B.wait_back(dev, t_up, timeout=a.back_timeout)
    if back is None:
        probe = B.tcp_probe(a.host)
        B.log("%s board did NOT come back within %.0fs; tcp probe %s - NOT retried, tell the user" % (
            B.stamp(), a.back_timeout, probe))
        fields.update(stage="back", went_down=down, tcp_probe={str(k): v for k, v in probe.items()})
        return False, fields
    B.log("%s back after %.1fs%s" % (B.stamp(), back, "" if down else " (never seen offline - did it reboot?)"))
    time.sleep(a.settle)

    after = B.read_status(dev)
    since_upload = time.time() - t_up
    with B.Terminal(dev) as t:
        keys1 = B.keys_fingerprint(t)
        heap1 = B.heap_median(t, k=3)
    diff = B.diff_fingerprints(keys0, keys1, a.ignore_keys)
    checks = {
        "went_down": down,
        "reason_ota": after["reason"] == OTA_REASON,
        "uptime_fresh": after["uptime"] is not None and after["uptime"] <= since_upload + 5,
        "system_ok": after["system"] in ("OK", None),
        "keys_unchanged": not (diff["added"] or diff["removed"] or diff["changed"]),
    }
    if want_md5:
        checks["md5_match"] = (after["md5"] or "").lower() == want_md5
    ok = all(checks.values())
    B.log("%s after: %s, md5 %s, uptime %ss, reset reason '%s', system %s, boot counter %s, heap median %s" % (
        B.stamp(), after["app"], after["md5"], after["uptime"], after["reason"], after["system"], after["boot"],
        heap1["available"]))
    B.log("  keys: %d before, %d after; added %s removed %s changed %s" % (
        len(keys0), len(keys1), diff["added"], diff["removed"], diff["changed"]))
    for k, v in checks.items():
        B.log("  %-15s %s" % (k, "ok" if v else "FAIL"))
    fields.update(stage="done", back_s=round(back, 1), app_after=after["app"], md5_after=after["md5"],
                  reason=after["reason"], uptime_after=after["uptime"], system=after["system"],
                  heap_before=heap0["available"], heap_after=heap1["available"], keys=len(keys1),
                  keys_added=diff["added"], keys_removed=diff["removed"], keys_changed=diff["changed"],
                  checks=checks)
    return ok, fields


def main():
    p = B.make_parser(__doc__)
    p.add_argument("firmware", help="image file (.bin / .bin.gz / Beken OTA image)")
    p.add_argument("--check-md5", action="store_true",
                   help="running md5 must equal the file md5 (gz unpacked) - ESP8266 images")
    p.add_argument("--expect-md5", default=None, help="running md5 the board must report after the update")
    p.add_argument("--min-uptime", type=int, default=B.MIN_REBOOT_UPTIME, help="minimum uptime before upload (>= 70)")
    p.add_argument("--stable-timeout", type=float, default=240.0, help="max wait for a stable board (default 240)")
    p.add_argument("--back-timeout", type=float, default=180.0, help="max wait for the board to return (default 180)")
    p.add_argument("--settle", type=float, default=5.0, help="seconds after it is back before checking (default 5)")
    p.add_argument("--ignore-keys", default=None, help="regex of key names left out of the key comparison")
    p.add_argument("--ignore-reset-reason", action="store_true",
                   help="upload even after a crash / watchdog reset - only with the user's OK")
    p.add_argument("--quiet", action="store_true", help="no upload progress")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
