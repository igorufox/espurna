#!/usr/bin/env python3
"""Slow readers: N authenticated GET / on sockets with SO_RCVBUF 256, read a few bytes now and then and held open
for H seconds, with a ping in parallel. Repro of the Beken lwIP RX starvation (2026-11-03: every RX frame is a
PBUF_RAM from the 16 KiB lwIP heap; slow readers queue it full and the board stops answering ping / ARP). The
AsyncTCP space() RX reserve that fixed it is not in the registry esp32async AsyncTCP, so run this on every Beken
web-stack change.

Pass = ping losses <= --max-ping-loss (default 0), /discover answers right after the sockets are closed, no reboot.
On LibreTiny the `heap` line `lwip:` is printed before and after.

    python code/scripts/bench_slow_reader.py --host 10.0.5.139 -n 5 --hold 60
"""
import select
import socket
import time

import bench_common as B

TOOL = "bench_slow_reader"


def open_reader(dev, uri, rcvbuf):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)  # before connect: sets the advertised window
    s.settimeout(10)
    s.connect((dev.host, dev.port))
    hdr = ["GET %s HTTP/1.1" % uri, "Host: %s" % dev.host, "Accept-Encoding: gzip", "Connection: close"]
    a = dev.auth_header("GET", uri)
    if a:
        hdr.append("Authorization: " + a)
    s.sendall(("\r\n".join(hdr) + "\r\n\r\n").encode())
    s.setblocking(False)
    return s


def status(dev):
    with B.Terminal(dev) as t:
        up = B.parse_uptime(t.cmd("uptime")[0])
        h = B.parse_heap(t.cmd("heap")[0])
    return up, h


def body(a):
    B.check_bench(a)
    dev = B.Device(a.host, a.port)
    up0, h0 = status(dev)
    B.log("%s before: uptime %ss heap %s contiguous %s%s" % (B.stamp(), up0, h0["available"], h0["contiguous"],
                                                          "  lwip: %s" % h0["lwip"] if h0["lwip"] else ""))
    dev.challenge(refresh=True)
    pinger = B.Pinger(a.host, interval=a.ping_interval)
    pinger.start()
    socks, got, errors, back = [], [], [], None
    try:
        time.sleep(2.0)
        for i in range(a.n):
            try:
                socks.append(open_reader(dev, a.uri, a.rcvbuf))
                got.append(0)
            except OSError as e:
                errors.append("connect #%d: %s" % (i + 1, e))
        B.log("%s %d slow readers open (rcvbuf %d), reading %d B every %.1fs for %.0fs" % (
            B.stamp(), len(socks), a.rcvbuf, a.read_bytes, a.read_every, a.hold))
        end = time.time() + a.hold
        next_report = time.time() + 10
        while time.time() < end:
            if a.read_bytes and socks:
                r, _, _ = select.select(socks, [], [], 0)
                for s in r:
                    try:
                        got[socks.index(s)] += len(s.recv(a.read_bytes))
                    except OSError:
                        pass
            if time.time() >= next_report:
                B.log("%s   read so far %s bytes, ping lost %d/%d" % (B.stamp(), got, pinger.lost, pinger.sent))
                next_report += 10
            time.sleep(a.read_every)
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
        t_closed = time.time()
        while time.time() - t_closed < a.recover:
            if dev.discover(timeout=2.0) is not None:
                back = time.time() - t_closed
                break
            time.sleep(1.0)
        time.sleep(a.tail)
        ping = pinger.stop()
    up1, h1 = status(dev)
    rebooted = up0 is not None and up1 is not None and up1 < up0
    ok = ping["lost"] <= a.max_ping_loss and back is not None and not rebooted and not errors
    for e in errors:
        B.log("  %s" % e)
    B.log("%s after: /discover %s, uptime %ss heap %s contiguous %s%s; ping lost %d/%d (longest gap %d)%s" % (
        B.stamp(), "back after %.1fs" % back if back is not None else "NOT answering", up1, h1["available"],
        h1["contiguous"], "  lwip: %s" % h1["lwip"] if h1["lwip"] else "", ping["lost"], ping["sent"],
        ping["longest_gap"], "  REBOOTED" if rebooted else ""))
    return ok, {"n": a.n, "hold_s": a.hold, "rcvbuf": a.rcvbuf, "bytes_read": got, "connect_errors": len(errors),
                "ping_sent": ping["sent"], "ping_lost": ping["lost"], "ping_longest_gap": ping["longest_gap"],
                "discover_back_s": round(back, 1) if back is not None else None, "rebooted": rebooted,
                "heap_before": h0["available"], "heap_after": h1["available"], "lwip_before": h0["lwip"],
                "lwip_after": h1["lwip"]}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("-n", type=int, default=5, help="slow readers (default 5)")
    p.add_argument("--hold", type=float, default=60.0, help="seconds to hold them open (default 60)")
    p.add_argument("--rcvbuf", type=int, default=256, help="SO_RCVBUF in bytes (default 256)")
    p.add_argument("--read-bytes", type=int, default=64, help="bytes per read, 0 = never read (default 64)")
    p.add_argument("--read-every", type=float, default=0.5, help="seconds between reads (default 0.5)")
    p.add_argument("--uri", default="/", help="page to request (default /)")
    p.add_argument("--recover", type=float, default=30.0, help="seconds to wait for /discover after closing")
    p.add_argument("--tail", type=float, default=5.0, help="seconds of ping after closing (default 5)")
    p.add_argument("--ping-interval", type=float, default=0.5, help="seconds between pings (default 0.5)")
    p.add_argument("--max-ping-loss", type=int, default=0, help="lost pings tolerated (default 0)")
    a = p.parse_args()
    raise SystemExit(B.run_tool(TOOL, a, body))


if __name__ == "__main__":
    main()
