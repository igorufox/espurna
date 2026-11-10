#!/usr/bin/env python3
"""N parallel authenticated GET / (the WebUI page, gzip) for R rounds, with a ping running in parallel.

Counts failed / short page loads and lost pings; checks the board did not reboot. Pass = every GET returned 200
with the same body size, ping losses <= --max-ping-loss (default 0), uptime kept growing.

    python code/scripts/bench_parallel_pages.py --host 10.0.5.139 -n 5 --rounds 3
"""
import http.client
import threading
import time

import bench_common as B

TOOL = "bench_parallel_pages"


def fetch(dev, uri, timeout, out, idx):
    t0 = time.time()
    try:
        st, hdrs, body = dev.request("GET", uri, headers={"Accept-Encoding": "gzip"}, auth=True,
                                     timeout=timeout, retries_503=0)
        out[idx] = (st, len(body), time.time() - t0, None)
    except (OSError, http.client.HTTPException) as e:
        out[idx] = (None, 0, time.time() - t0, "%s: %s" % (type(e).__name__, e))


def uptime(dev):
    with B.Terminal(dev) as t:
        return B.parse_uptime(t.cmd("uptime")[0])


def body(a):
    B.check_bench(a)
    dev = B.Device(a.host, a.port)
    up0 = uptime(dev)
    dev.challenge(refresh=True)
    pinger = B.Pinger(a.host, interval=a.ping_interval)
    pinger.start()
    results = []
    try:
        time.sleep(2.0)
        for r in range(1, a.rounds + 1):
            out = [None] * a.n
            th = [threading.Thread(target=fetch, args=(dev, a.uri, a.timeout, out, i)) for i in range(a.n)]
            t0 = time.time()
            for t in th:
                t.start()
            for t in th:
                t.join(a.timeout + 10)
            results.extend(x for x in out if x)
            okn = sum(1 for x in out if x and x[0] == 200)
            B.log("%s round %d/%d: %d/%d HTTP 200 in %.1fs, sizes %s, ping lost so far %d/%d" % (
                B.stamp(), r, a.rounds, okn, a.n, time.time() - t0, sorted({x[1] for x in out if x}),
                pinger.lost, pinger.sent))
            for x in out:
                if x and x[3]:
                    B.log("    error after %.1fs: %s" % (x[2], x[3]))
            time.sleep(a.pause)
        time.sleep(a.tail)
    finally:
        ping = pinger.stop()
    up1 = uptime(dev)
    rebooted = up0 is not None and up1 is not None and up1 < up0
    good = [x for x in results if x[0] == 200]
    sizes = sorted({x[1] for x in good})
    failed = a.n * a.rounds - len(good)
    ok = failed == 0 and len(sizes) == 1 and ping["lost"] <= a.max_ping_loss and not rebooted
    B.log("%s total: %d/%d OK, body sizes %s, slowest %.1fs, ping lost %d/%d (longest gap %d), rebooted %s" % (
        B.stamp(), len(good), a.n * a.rounds, sizes, max((x[2] for x in results), default=0), ping["lost"],
        ping["sent"], ping["longest_gap"], rebooted))
    return ok, {"n": a.n, "rounds": a.rounds, "get_ok": len(good), "get_failed": failed, "sizes": sizes,
                "slowest_s": round(max((x[2] for x in results), default=0), 2), "ping_sent": ping["sent"],
                "ping_lost": ping["lost"], "ping_longest_gap": ping["longest_gap"], "rebooted": rebooted}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("-n", type=int, default=5, help="parallel GETs per round (default 5)")
    p.add_argument("--rounds", type=int, default=3, help="rounds (default 3)")
    p.add_argument("--uri", default="/", help="page to load (default /)")
    p.add_argument("--timeout", type=float, default=30.0, help="per-GET timeout in seconds (default 30)")
    p.add_argument("--pause", type=float, default=5.0, help="seconds between rounds (default 5)")
    p.add_argument("--tail", type=float, default=10.0, help="seconds of ping after the last round (default 10)")
    p.add_argument("--ping-interval", type=float, default=1.0, help="seconds between pings (default 1)")
    p.add_argument("--max-ping-loss", type=int, default=0, help="lost pings tolerated (default 0)")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
