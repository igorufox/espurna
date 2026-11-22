#!/usr/bin/env python3
"""Minimal stdlib MQTT 3.1.1 client (anonymous, QoS 0) for bench checks: watch what a board publishes (state,
Home Assistant discovery, heartbeat) or publish one message to it.

  sub FILTER [--match TEXT] [--seconds S]   print PUBLISH packets (retained ones marked [R]), summary per topic
  pub TOPIC PAYLOAD [--retain]              publish one message (needs a bench board topic or --allow-non-bench)

`pub` refuses topics that do not contain the hostname of a bench board's ESPURNA-xxxx unless --allow-non-bench
(only with the user's OK): a publish reaches production boards just like bench ones.

    python code/scripts/bench_mqtt.py sub "homeassistant/#" --match D390FC --seconds 30
    python code/scripts/bench_mqtt.py sub "ESPURNA-D390FC/#" --seconds 120
    python code/scripts/bench_mqtt.py pub ESPURNA-D390FC/relay/0/set 1
"""
import argparse
import json
import os
import select
import socket
import struct
import sys
import time

TOOL = "bench_mqtt"
BENCH_TOPIC_HINTS = ("D390FC", "79E12A", "4F3E20", "173F9C")  # 10.0.5.19, 10.0.5.58, 10.0.5.66, COM3 ESP32 DevKit (chip id suffixes)

try:
    sys.stdout.reconfigure(errors="replace", line_buffering=True)
except Exception:
    pass


def enc_str(s):
    b = s.encode() if isinstance(s, str) else s
    return struct.pack(">H", len(b)) + b


def enc_len(n):
    out = b""
    while True:
        d, n = n % 128, n // 128
        out += bytes([d | (0x80 if n else 0)])
        if not n:
            return out


def packet(ptype, body):
    return bytes([ptype]) + enc_len(len(body)) + body


class Mqtt:
    def __init__(self, host, port=1883, keepalive=60):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        self.cid = "bench-pc-" + os.urandom(3).hex()
        self.s.sendall(packet(0x10, enc_str("MQTT") + bytes([4, 0x02]) + struct.pack(">H", keepalive)
                              + enc_str(self.cid)))
        self.last_ping = time.time()

    def _read_packet(self):
        if len(self.buf) < 2:
            return None
        mult, n, i = 1, 0, 1
        while True:
            if i >= len(self.buf):
                return None
            d = self.buf[i]
            n += (d & 0x7F) * mult
            mult *= 128
            i += 1
            if not d & 0x80:
                break
        if len(self.buf) < i + n:
            return None
        p = (self.buf[0], self.buf[i:i + n])
        self.buf = self.buf[i + n:]
        return p

    def packets(self, timeout):
        """Yield (type, flags, body) for `timeout` seconds; keeps the connection alive."""
        end = time.time() + timeout
        while time.time() < end:
            if time.time() - self.last_ping > 30:
                self.s.sendall(bytes([0xC0, 0]))
                self.last_ping = time.time()
            r, _, _ = select.select([self.s], [], [], min(0.5, max(0.01, end - time.time())))
            if r:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise ConnectionError("broker closed the connection")
                self.buf += chunk
            while True:
                p = self._read_packet()
                if p is None:
                    break
                yield p[0] >> 4, p[0] & 0x0F, p[1]

    def wait_connack(self, timeout=10):
        for t, _, body in self.packets(timeout):
            if t == 2:
                if body[1] != 0:
                    raise ConnectionError("CONNACK rc=%d" % body[1])
                return
        raise ConnectionError("no CONNACK")

    def subscribe(self, flt):
        self.s.sendall(packet(0x82, struct.pack(">H", 1) + enc_str(flt) + bytes([0])))

    def publish(self, topic, payload, retain=False):
        self.s.sendall(packet(0x30 | (1 if retain else 0), enc_str(topic) + payload.encode()))

    def close(self):
        try:
            self.s.sendall(bytes([0xE0, 0]))
            self.s.close()
        except OSError:
            pass


def result(ok, **fields):
    out = {"tool": TOOL, "ok": bool(ok)}
    out.update(fields)
    print("BENCH_RESULT " + json.dumps(out, ensure_ascii=False), flush=True)
    return 0 if ok else 1


def do_sub(a):
    m = Mqtt(a.broker, a.mqtt_port)
    m.wait_connack()
    m.subscribe(a.filter)
    t0, counts, other, subscribed = time.time(), {}, 0, False
    match = (a.match or "").lower()
    for t, flags, body in m.packets(a.seconds):
        if t == 9:
            subscribed = True
            print("%7.2f SUBACK %s" % (time.time() - t0, a.filter))
        elif t == 3:
            qos = (flags >> 1) & 3
            tl = struct.unpack(">H", body[:2])[0]
            topic = body[2:2 + tl].decode(errors="replace")
            off = 2 + tl + (2 if qos else 0)
            if qos == 1:
                m.s.sendall(bytes([0x40, 2]) + body[2 + tl:4 + tl])
            if match and match not in topic.lower():
                other += 1
                continue
            counts[topic] = counts.get(topic, 0) + 1
            payload = body[off:].decode(errors="replace")
            if len(payload) > a.width:
                payload = payload[:a.width] + "...(%d)" % len(payload)
            print("%7.2f %s%s = %s" % (time.time() - t0, "[R] " if flags & 1 else "", topic, payload))
    m.close()
    print("--- %d matching topics, %d messages; %d non-matching ignored" % (len(counts), sum(counts.values()), other))
    for k in sorted(counts):
        print("  %3d  %s" % (counts[k], k))
    return result(subscribed, broker=a.broker, filter=a.filter, topics=len(counts), messages=sum(counts.values()))


def do_pub(a):
    if not any(h.lower() in a.topic.lower() for h in BENCH_TOPIC_HINTS) and not a.allow_non_bench:
        print("REFUSED: topic %r is not a bench board's (%s); --allow-non-bench only with the user's OK"
              % (a.topic, ", ".join(BENCH_TOPIC_HINTS)))
        print("BENCH_RESULT " + json.dumps({"tool": TOOL, "ok": False, "refused": a.topic}))
        return 3
    m = Mqtt(a.broker, a.mqtt_port)
    m.wait_connack()
    m.publish(a.topic, a.payload, retain=a.retain)
    time.sleep(0.3)
    m.close()
    print("published %s = %s%s" % (a.topic, a.payload, " (retained)" if a.retain else ""))
    return result(True, broker=a.broker, topic=a.topic)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--broker", default="10.0.0.2", help="MQTT broker (default 10.0.0.2)")
    p.add_argument("--mqtt-port", type=int, default=1883)
    sp = p.add_subparsers(dest="action", required=True)
    s = sp.add_parser("sub", help="print messages of a topic filter")
    s.add_argument("filter")
    s.add_argument("--match", default=None, help="only topics containing this text (case-insensitive)")
    s.add_argument("--seconds", type=float, default=30.0)
    s.add_argument("--width", type=int, default=160, help="payload characters shown (default 160)")
    s = sp.add_parser("pub", help="publish one message")
    s.add_argument("topic")
    s.add_argument("payload")
    s.add_argument("--retain", action="store_true")
    s.add_argument("--allow-non-bench", action="store_true",
                   help="publish to a topic of a non-bench board - only with the user's OK")
    a = p.parse_args()
    try:
        rc = do_sub(a) if a.action == "sub" else do_pub(a)
    except (OSError, ConnectionError) as e:
        print("NETWORK ERROR: %s: %s" % (type(e).__name__, e))
        rc = result(False, error="%s: %s" % (type(e).__name__, e))
    raise SystemExit(rc)


if __name__ == "__main__":
    main()
