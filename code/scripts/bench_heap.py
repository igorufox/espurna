#!/usr/bin/env python3
"""Median heap of an ESPurna board: K `heap` readings in one WS session, median of available / contiguous.

ESP8266 heap swings +-4 KB between single readings, so compare medians, and re-check a minute after a burst.
Read-only (runs `heap` only), allowed on any board.

    python code/scripts/bench_heap.py --host 10.0.5.19 -k 5
"""
import time

import bench_common as B

TOOL = "bench_heap"


def body(a):
    with B.Terminal(B.Device(a.host, a.port), settle=a.settle) as t:
        h = B.heap_median(t, k=a.k, pause=a.pause)
        lwip = B.parse_heap(t.cmd("heap")[0])["lwip"]
    B.log("%s heap median of %d: available %s contiguous %s (min %s max %s)%s" % (
        B.stamp(), h["n"], h["available"], h["contiguous"], h["min"], h["max"],
        "  lwip: %s" % lwip if lwip else ""))
    ok = h["n"] == a.k and (a.min_available is None or (h["available"] or 0) >= a.min_available)
    return ok, {"k": a.k, "n": h["n"], "available": h["available"], "contiguous": h["contiguous"],
                "min": h["min"], "max": h["max"], "lwip": lwip, "time": time.strftime("%Y-%m-%d %H:%M:%S")}


def main():
    p = B.make_parser(__doc__, writes=False)
    p.add_argument("-k", type=int, default=5, help="number of readings (default 5)")
    p.add_argument("--pause", type=float, default=1.0, help="seconds between readings (default 1)")
    p.add_argument("--settle", type=float, default=3.0,
                   help="seconds to let the WS initial dump pass before the first reading (default 3)")
    p.add_argument("--min-available", type=int, default=None, help="fail when the median is below this")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
