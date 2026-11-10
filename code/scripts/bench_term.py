#!/usr/bin/env python3
"""Terminal and read-only inspection of an ESPurna board over the network (WS terminal, WS dump, HTTP GET).

  cmd CMD [CMD ...]     run terminal commands in one WS session, print the (redacted) output
  watch SECONDS         print the WS debug log for SECONDS
  keys [--show RX]      settings key NAMES, count and free space; --show prints values of matching non-secret keys
  dump [--match RX]     fields of the WS initial dump (redacted JSON), top-level names matching RX
  inventory [HOST ...]  identity / size fields of the WS dump for --host and more hosts, no commands sent
  http URI [--auth]     GET a URI: status, headers, size, redacted JSON / text

Secret values (pass*, *key*, *token*, ...) are redacted in every line. `cmd` with anything but a read-only command
(help, info, heap, uptime, keys, get, wifi, mqtt, ntp, date, network, relay without arguments, ...) needs a bench
board or --allow-non-bench (only with the user's OK).

    python code/scripts/bench_term.py --host 10.0.5.66 cmd info heap "get relayBoot0"
    python code/scripts/bench_term.py --host 10.0.5.19 keys --show "^relay"
    python code/scripts/bench_term.py --host 10.0.4.171 inventory 10.0.5.161 10.0.5.0
"""
import json
import re
import time

import bench_common as B

TOOL = "bench_term"
READ_ONLY = {"help", "info", "heap", "uptime", "keys", "get", "wifi", "mqtt", "ntp", "date", "network", "crash",
             "config", "gpio", "button", "sensor", "magnitudes", "light", "relay", "pulse", "api", "web.servers"}
INVENTORY = ("hostname", "desc", "manufacturer", "device", "app_name", "app_version", "chipid", "mac", "sdk", "core",
             "sketch_size", "free_size", "heap", "uptime", "webMode", "btnConfig", "relayConfig")


def is_read_only(line):
    words = line.split()
    if not words:
        return True
    w = words[0].lower()
    if w in ("relay", "pulse", "gpio", "light"):
        return len(words) == 1  # with arguments these switch things
    return w in READ_ONLY


def ws_dump(dev, seconds):
    """Merged top-level fields of the WS initial dump (no command sent)."""
    ws = B.ws_open(dev)
    out, end = {}, time.time() + seconds
    try:
        while time.time() < end and ws.closed is None:
            m = ws.recv(0.5)
            if not m:
                continue
            try:
                obj = json.loads(m)
            except ValueError:
                continue
            if isinstance(obj, dict):
                for k, v in obj.items():
                    if k != "log":
                        out.setdefault(k, v)
    finally:
        ws.close()
    return out


def do_cmd(a, dev):
    if not all(is_read_only(c) for c in a.commands):
        B.check_bench(a)
    errors = 0
    with B.Terminal(dev, settle=a.settle) as t:
        for c in a.commands:
            print("> %s" % c)
            lines, st = t.cmd(c, idle=a.idle, cap=a.cap)
            for l in lines:
                print("  " + B.redact(l))
            errors += 0 if B.is_ok(st) else 1
            if t.ws.closed:
                B.log("!! %s" % t.ws.closed)
                break
    return errors == 0, {"commands": len(a.commands), "errors": errors}


def do_watch(a, dev):
    ws = B.ws_open(dev)
    t0 = last_ping = time.time()
    n = 0
    try:
        while time.time() - t0 < a.seconds and not ws.closed:
            if time.time() - last_ping >= 5:
                ws.send_json({"action": "ping"})
                last_ping = time.time()
            msg = ws.recv(min(1.0, max(0.05, a.seconds - (time.time() - t0))))
            if msg:
                for l in B.log_lines(msg):
                    print("%7.2f %s" % (time.time() - t0, l))
                    n += 1
    finally:
        closed = ws.closed
        ws.close()
    if closed:
        B.log("!! %s after %.1f s" % (closed, time.time() - t0))
    return not closed, {"seconds": a.seconds, "lines": n, "closed": closed}


def do_keys(a, dev):
    with B.Terminal(dev) as t:
        lines, st = t.cmd("keys", cap=90.0)
    names = sorted(B.parse_key_values(lines))
    for n in names:
        print(n)
    for l in lines:
        if re.match(r"^(Number of keys|Available):", l):
            print("# " + l)
    shown = {}
    if a.show:
        rx = re.compile(a.show)
        vals = B.parse_key_values(lines)  # already redacted lines
        for n in names:
            if rx.search(n):
                shown[n] = "<redacted>" if B.SECRET_NAME_RX.search(n) else vals[n]
                print("%s = %s" % (n, shown[n]))
    return B.is_ok(st), {"status": st, "count": len(names), "shown": shown}


def do_dump(a, dev):
    d = ws_dump(dev, a.seconds)
    rx = re.compile(a.match, re.I) if a.match else None
    sel = {k: v for k, v in d.items() if not rx or rx.search(k)}
    text = json.dumps(B.redact_obj(sel), indent=1, ensure_ascii=False)
    print(text[:a.limit] + ("\n... (%d chars, --limit)" % len(text) if len(text) > a.limit else ""))
    return bool(d), {"fields": len(d), "selected": len(sel)}


def do_inventory(a, dev):
    rows, bad = {}, 0
    for host in [a.host] + a.hosts:
        info = {}
        try:
            d = ws_dump(B.Device(host, a.port), a.seconds)
            info = {k: d[k] for k in INVENTORY if k in d}
        except (OSError, ConnectionError, B.SetupError) as e:
            info["error"] = "%s: %s" % (type(e).__name__, e)
            bad += 1
        rows[host] = info
        rc, bc = info.get("relayConfig"), info.get("btnConfig")
        print("%-12s %-20s %-10s %-22s %-36s sketch %-7s free %-7s chip %-12s relays %s buttons %s %s" % (
            host, info.get("hostname", "-"), info.get("manufacturer", "-"), info.get("device", "-"),
            info.get("app_version", "-"), info.get("sketch_size", "-"), info.get("free_size", "-"),
            info.get("chipid", "-"), rc.get("size") if isinstance(rc, dict) else None,
            len(bc.get("values", [])) if isinstance(bc, dict) else None, info.get("error", "")), flush=True)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(B.redact_obj(rows), f, indent=1, ensure_ascii=False)
    return bad == 0, {"hosts": len(rows), "errors": bad, "json": a.json}


def do_http(a, dev):
    st, hdrs, body = dev.request("GET", a.uri, headers={"Accept-Encoding": "gzip"}, auth=a.auth, timeout=15)
    print("GET %s -> HTTP %d, %d bytes" % (a.uri, st, len(body)))
    for h in ("content-type", "content-encoding", "content-disposition", "cache-control", "last-modified"):
        if hdrs.get(h):
            print("  %s: %s" % (h, hdrs[h]))
    if a.mode == "json":
        try:
            print(json.dumps(B.redact_obj(json.loads(body)), indent=1, ensure_ascii=False)[:a.limit])
        except ValueError as e:
            print("  JSON parse error: %s" % e)
    elif a.mode == "text":
        print("\n".join(B.redact(l) for l in body.decode(errors="replace").splitlines())[:a.limit])
    return 200 <= st < 400, {"uri": a.uri, "status": st, "bytes": len(body)}


def main():
    p = B.make_parser(__doc__)
    p.add_argument("--settle", type=float, default=0.0, help="seconds to skip the WS dump before commands")
    sp = p.add_subparsers(dest="action", required=True)
    s = sp.add_parser("cmd", help="run terminal commands in one WS session")
    s.add_argument("commands", nargs="+")
    s.add_argument("--idle", type=float, default=5.0, help="seconds without output that end a command")
    s.add_argument("--cap", type=float, default=60.0, help="max seconds per command")
    s = sp.add_parser("watch", help="print the WS debug log")
    s.add_argument("seconds", type=float)
    s = sp.add_parser("keys", help="settings key names (no values)")
    s.add_argument("--show", default=None, help="regex: also print values of matching non-secret keys")
    s = sp.add_parser("dump", help="WS initial dump fields (redacted)")
    s.add_argument("--seconds", type=float, default=6.0)
    s.add_argument("--match", default=None, help="regex on top-level field names")
    s.add_argument("--limit", type=int, default=20000)
    s = sp.add_parser("inventory", help="identity fields of --host and more hosts")
    s.add_argument("hosts", nargs="*")
    s.add_argument("--seconds", type=float, default=6.0)
    s.add_argument("--json", default=None, help="also write the (redacted) rows to this JSON file")
    s = sp.add_parser("http", help="GET a URI and summarize")
    s.add_argument("uri")
    s.add_argument("--auth", action="store_true")
    s.add_argument("--mode", choices=["json", "text", "none"], default="none")
    s.add_argument("--limit", type=int, default=3000)
    a = p.parse_args()
    fn = {"cmd": do_cmd, "watch": do_watch, "keys": do_keys, "dump": do_dump, "inventory": do_inventory,
          "http": do_http}[a.action]
    raise SystemExit(B.run_tool("%s %s" % (TOOL, a.action), a, lambda args: fn(args, B.Device(args.host, args.port))))


if __name__ == "__main__":
    main()
