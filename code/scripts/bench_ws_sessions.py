#!/usr/bin/env python3
"""Open and close N WebSocket sessions in a row (the way the WebUI does on every page load: /auth ticket, /ws,
initial dump, close) and compare the heap before and after.

Pass = every session opened and delivered its initial dump, no reboot (uptime keeps growing), and the median heap
after --settle-after seconds is at most --tolerance bytes below the median before.

    python code/scripts/bench_ws_sessions.py --host 10.0.5.66 -n 100
"""
import time

import bench_common as B

TOOL = "bench_ws_sessions"


def measure(dev, k):
    with B.Terminal(dev, settle=3.0) as t:
        h = B.heap_median(t, k=k)
        up = B.parse_uptime(t.cmd("uptime")[0])
    return h, up


def body(a):
    B.check_bench(a)
    dev = B.Device(a.host, a.port)
    before, up0 = measure(dev, a.k)
    B.log("%s before: heap median %s contiguous %s, uptime %ss" % (B.stamp(), before["available"],
                                                                   before["contiguous"], up0))
    fails, times, msgs = [], [], []
    for i in range(1, a.n + 1):
        t0 = time.time()
        try:
            ws = B.ws_open(dev, ticket=not a.one_ticket or i == 1)
            try:
                n = ws.drain(a.hold)
                closed = ws.closed
            finally:
                ws.close()
            if closed or n == 0:
                fails.append("#%d: %s" % (i, closed or "no initial dump"))
            msgs.append(n)
        except (OSError, ConnectionError) as e:
            fails.append("#%d: %s: %s" % (i, type(e).__name__, e))
        times.append(time.time() - t0)
        if i % 10 == 0 or i == a.n:
            B.log("%s %d/%d sessions, failures %d, last %.1fs" % (B.stamp(), i, a.n, len(fails), times[-1]))
        if a.pause:
            time.sleep(a.pause)
    B.log("waiting %.0fs before the after-reading" % a.settle_after)
    time.sleep(a.settle_after)
    after, up1 = measure(dev, a.k)
    rebooted = up0 is not None and up1 is not None and up1 < up0
    delta = (after["available"] - before["available"]) if None not in (after["available"], before["available"]) else None
    ok = not fails and not rebooted and delta is not None and delta >= -a.tolerance
    for f in fails[:20]:
        B.log("  fail %s" % f)
    B.log("%s after: heap median %s (delta %s) contiguous %s, uptime %ss%s" % (
        B.stamp(), after["available"], delta, after["contiguous"], up1, "  REBOOTED" if rebooted else ""))
    return ok, {"n": a.n, "failures": len(fails), "rebooted": rebooted, "heap_before": before["available"],
                "heap_after": after["available"], "heap_delta": delta, "contig_before": before["contiguous"],
                "contig_after": after["contiguous"], "session_s_max": round(max(times), 2) if times else None,
                "dump_msgs_min": min(msgs) if msgs else None}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("-n", type=int, default=100, help="sessions (default 100)")
    p.add_argument("--hold", type=float, default=2.0, help="seconds each session reads its dump (default 2)")
    p.add_argument("--pause", type=float, default=0.5, help="seconds between sessions (default 0.5)")
    p.add_argument("--one-ticket", action="store_true", help="GET /auth only before the first session")
    p.add_argument("-k", type=int, default=5, help="heap readings per median (default 5)")
    p.add_argument("--settle-after", type=float, default=60.0, help="wait before the after-reading (default 60)")
    p.add_argument("--tolerance", type=int, default=2048, help="allowed heap drop in bytes (default 2048)")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
