#!/usr/bin/env python3
"""N relay pulses (ON, wait, OFF) via the WS terminal or the HTTP API, heap at steady state before / after.

Heap = median of K readings after --settle (before) and after --settle-after seconds past the last pulse (ESP8266
heap swings +-4 KB right after a burst). The relay's state from before is restored at the end. Paced at
--period seconds per switch so the relay flood protection (relayFloodTime / relayFloodChanges) does not kick in.
API mode reads apiKey over the WS terminal into memory (never printed) and sends it in the Api-Key header.

Pass = every switch acknowledged (+OK / HTTP 200), final state as expected, no reboot, heap drop <= --tolerance.

    python code/scripts/bench_relay_pulse.py --host 10.0.5.19 -n 100
    python code/scripts/bench_relay_pulse.py --host 10.0.5.66 -n 50 --relay 1 --via api
"""
import re
import time

import bench_common as B

TOOL = "bench_relay_pulse"
RX_STATUS = re.compile(r"^\s*relay(\d+)\s+\{.*Status=(\w+)")


def relay_state(term, idx):
    for l in term.cmd("relay")[0]:
        m = RX_STATUS.match(l)
        if m and int(m.group(1)) == idx:
            return m.group(2).upper()
    return None


def body(a):
    B.check_bench(a)
    dev = B.Device(a.host, a.port)
    with B.Terminal(dev, settle=a.settle) as t:
        up0 = B.parse_uptime(t.cmd("uptime")[0])
        state0 = relay_state(t, a.relay)
        if state0 is None:
            raise B.SetupError("relay%d not found (`relay` lists none with that id)" % a.relay)
        key = dev.api_key(t) if a.via == "api" else None
        if a.via == "api" and not key:
            raise B.SetupError("API disabled or apiKey empty on the board; use --via ws")
        heap0 = B.heap_median(t, k=a.k)
    B.log("%s before: relay%d %s, uptime %ss, heap median %s contiguous %s" % (
        B.stamp(), a.relay, state0, up0, heap0["available"], heap0["contiguous"]))

    fails = []
    term = B.Terminal(dev) if a.via == "ws" else None
    try:
        def switch(on):
            if term is not None:
                lines, st = term.cmd("relay %d %d" % (a.relay, 1 if on else 0), idle=4.0, cap=10.0)
                return B.is_ok(st), st
            st, text = dev.api("PUT", "relay/%d" % a.relay, key, value=1 if on else 0)
            return st == 200, "HTTP %d %s" % (st, text.strip()[:40])

        for i in range(1, a.n + 1):
            for on in (True, False):
                t0 = time.time()
                ok, st = switch(on)
                if not ok:
                    fails.append("#%d %s: %s" % (i, "on" if on else "off", st))
                time.sleep(max(0.0, (a.on_time if on else a.period) - (time.time() - t0)))
            if i % 10 == 0 or i == a.n:
                B.log("%s %d/%d pulses, failures %d" % (B.stamp(), i, a.n, len(fails)))
            if len(fails) > a.max_fail_abort:
                B.log("too many failures, stopping")
                break
    finally:
        if term is not None:
            term.close()

    B.log("waiting %.0fs before the after-reading" % a.settle_after)
    time.sleep(a.settle_after)
    with B.Terminal(dev, settle=a.settle) as t:
        state1 = relay_state(t, a.relay)
        if state0 == "ON":
            t.cmd("relay %d 1" % a.relay)
        heap1 = B.heap_median(t, k=a.k)
        up1 = B.parse_uptime(t.cmd("uptime")[0])
        restored = relay_state(t, a.relay)
    rebooted = up0 is not None and up1 is not None and up1 < up0
    delta = (heap1["available"] - heap0["available"]) if None not in (heap1["available"], heap0["available"]) else None
    ok = not fails and state1 == "OFF" and restored == state0 and not rebooted \
        and delta is not None and delta >= -a.tolerance
    for f in fails[:20]:
        B.log("  fail %s" % f)
    B.log("%s after: relay%d %s (restored to %s), uptime %ss, heap median %s (delta %s) contiguous %s%s" % (
        B.stamp(), a.relay, state1, restored, up1, heap1["available"], delta, heap1["contiguous"],
        "  REBOOTED" if rebooted else ""))
    return ok, {"n": a.n, "via": a.via, "relay": a.relay, "failures": len(fails), "rebooted": rebooted,
                "state_before": state0, "state_end": state1, "state_restored": restored,
                "heap_before": heap0["available"], "heap_after": heap1["available"], "heap_delta": delta,
                "contig_before": heap0["contiguous"], "contig_after": heap1["contiguous"]}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("-n", type=int, default=100, help="pulses (default 100)")
    p.add_argument("--relay", type=int, default=0, help="relay id (default 0)")
    p.add_argument("--via", choices=["ws", "api"], default="ws", help="WS terminal or HTTP API (default ws)")
    p.add_argument("--on-time", type=float, default=1.0, help="seconds ON per pulse (default 1)")
    p.add_argument("--period", type=float, default=1.0, help="seconds OFF between pulses (default 1)")
    p.add_argument("-k", type=int, default=5, help="heap readings per median (default 5)")
    p.add_argument("--settle", type=float, default=3.0, help="seconds after WS connect before reading (default 3)")
    p.add_argument("--settle-after", type=float, default=60.0, help="wait before the after-reading (default 60)")
    p.add_argument("--tolerance", type=int, default=2048, help="allowed heap drop in bytes (default 2048)")
    p.add_argument("--max-fail-abort", type=int, default=10, help="stop after this many failed switches")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
