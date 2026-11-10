#!/usr/bin/env python3
"""Soak an ESPurna board: every M seconds sample uptime, heap (available / max block), RSSI, BSSID, MQTT state;
detect reboots (uptime going down) with their reset reason, unreachable samples and a downward heap trend.

Each sample is its own short WS session (a reboot or a dropped session does not end the soak). Rows go to a CSV
(appended, header when the file is new). Read-only on the board: uptime, heap, wifi, mqtt, info.

Pass = no reboot, unreachable samples <= --max-misses, heap not trending down. Downward trend = least-squares
slope of `available` (after --warmup) below -(--max-drop-per-hour) AND the median of the last quarter more than
1 KB below the median of the first quarter (ESP8266 heap swings +-4 KB between single readings).

    python code/scripts/bench_soak.py --host 10.0.5.19 --hours 8 --interval 60 --csv d390fc_soak.csv
"""
import csv
import os
import time

import bench_common as B

TOOL = "bench_soak"
FIELDS = ["time", "elapsed_s", "uptime_s", "available", "contiguous", "initial", "rssi", "bssid", "channel",
          "mqtt", "event", "detail"]


def take(dev):
    with B.Terminal(dev) as t:
        return B.sample(t)


def reason_after_reboot(dev):
    try:
        st = B.read_status(dev)
        return "%s; system %s, boot counter %s" % (st["reason"], st["system"], st["boot"])
    except (OSError, ConnectionError, B.SetupError) as e:
        return "info failed: %s" % e


def body(a):
    dev = B.Device(a.host, a.port)
    start = B.read_status(dev)
    B.log("%s start: %s, uptime %ss, last reset reason: %s, system %s" % (
        B.stamp(), start["app"], start["uptime"], start["reason"], start["system"]))
    new = not os.path.exists(a.csv) or os.path.getsize(a.csv) == 0
    t0 = time.time()
    end = t0 + a.hours * 3600.0
    last_up, reboots, misses, consecutive, samples = start["uptime"], [], 0, 0, []
    rssi_vals, mqtt_down, bssids = [], 0, set()
    with open(a.csv, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
        B.log("    time  elapsed  uptime_s  avail  contig  rssi  mqtt          bssid")
        while time.time() < end:
            tick = time.time()
            row = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "elapsed_s": int(tick - t0)}
            try:
                s = take(dev)
                consecutive = 0
            except (OSError, ConnectionError) as e:
                misses += 1
                consecutive += 1
                row.update(event="unreachable", detail="%s: %s" % (type(e).__name__, e))
                B.log("%s %7.1fm  UNREACHABLE (%d in a row): %s" % (B.stamp(), (tick - t0) / 60, consecutive, e))
                w.writerow(row)
                f.flush()
                if consecutive >= a.give_up:
                    B.log("giving up after %d unreachable samples in a row" % consecutive)
                    break
                time.sleep(max(1.0, a.interval - (time.time() - tick)))
                continue
            row.update(uptime_s=s["uptime"], available=s["available"], contiguous=s["contiguous"],
                       initial=s["initial"], rssi=s["rssi"], bssid=s["bssid"], channel=s["channel"], mqtt=s["mqtt"])
            if s["uptime"] is not None and last_up is not None and s["uptime"] < last_up:
                why = reason_after_reboot(dev)
                reboots.append({"elapsed_s": int(tick - t0), "uptime_before": last_up, "reason": why})
                row.update(event="REBOOT", detail=why)
                B.log("%s !! REBOOT detected (uptime %s < %s): %s" % (B.stamp(), s["uptime"], last_up, why))
            if s["uptime"] is not None:
                last_up = s["uptime"]
            if s["available"] is not None and tick - t0 >= a.warmup:
                samples.append((tick - t0, s["available"]))
            if s["rssi"] is not None:
                rssi_vals.append(s["rssi"])
            if s["bssid"]:
                bssids.add(s["bssid"])
            if s["mqtt"] not in (None, "CONNECTED"):
                mqtt_down += 1
            B.log("%s %7.1fm  %8s  %5s  %6s  %4s  %-12s  %s" % (
                B.stamp(), (tick - t0) / 60, s["uptime"], s["available"], s["contiguous"], s["rssi"],
                s["mqtt"], s["bssid"]))
            w.writerow(row)
            f.flush()
            if reboots and a.stop_on_reboot:
                B.log("stopping at the first reboot (--stop-on-reboot)")
                break
            time.sleep(max(1.0, a.interval - (time.time() - tick)))

    slope = B.slope_per_hour(samples)
    vals = [v for _, v in samples]
    q = max(1, len(vals) // 4)
    first_q, last_q = B.median(vals[:q]), B.median(vals[-q:])
    trend_down = (slope is not None and slope < -a.max_drop_per_hour
                  and first_q is not None and last_q is not None and last_q < first_q - 1024)
    ok = not reboots and misses <= a.max_misses and not trend_down and bool(vals)
    B.log("%s done: %.2f h, %d heap samples, reboots %d, unreachable %d, heap median first quarter %s last %s, "
          "slope %s B/h, RSSI %s..%s, BSSIDs %d, samples with MQTT not connected %d; csv %s" % (
              B.stamp(), (time.time() - t0) / 3600, len(vals), len(reboots), misses, first_q, last_q,
              "%.0f" % slope if slope is not None else "-", min(rssi_vals) if rssi_vals else "-",
              max(rssi_vals) if rssi_vals else "-", len(bssids), mqtt_down, os.path.abspath(a.csv)))
    return ok, {
        "hours": round((time.time() - t0) / 3600, 3), "samples": len(vals), "reboots": len(reboots),
        "reboot_list": reboots, "unreachable": misses, "heap_first_q": first_q, "heap_last_q": last_q,
        "heap_min": min(vals) if vals else None, "heap_slope_b_per_h": round(slope) if slope is not None else None,
        "heap_trend_down": trend_down, "rssi_min": min(rssi_vals) if rssi_vals else None,
        "rssi_max": max(rssi_vals) if rssi_vals else None, "bssid_count": len(bssids),
        "mqtt_not_connected_samples": mqtt_down, "csv": os.path.abspath(a.csv),
    }


def main():
    p = B.make_parser(__doc__, writes=False)
    p.add_argument("--hours", type=float, required=True, help="soak duration in hours")
    p.add_argument("--interval", type=float, default=60.0, help="seconds between samples (default 60)")
    p.add_argument("--csv", required=True, help="CSV log path (appended)")
    p.add_argument("--warmup", type=float, default=600.0,
                   help="seconds from the start not used for the heap trend (default 600)")
    p.add_argument("--max-drop-per-hour", type=float, default=300.0,
                   help="heap slope (bytes/hour) below minus this counts as a downward trend (default 300)")
    p.add_argument("--max-misses", type=int, default=0, help="unreachable samples tolerated (default 0)")
    p.add_argument("--give-up", type=int, default=10, help="stop after this many unreachable samples in a row")
    p.add_argument("--stop-on-reboot", action="store_true", help="end the soak at the first detected reboot")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
