#!/usr/bin/env python3
"""A/B of the radio: our RTOS build against ESPurna 1.15 (NONOS) on the SAME ESP8266 board, in alternating blocks,
under MQTT load, with a control station pinged at the same time.

Every block: the board is switched over the air (RTOS -> 1.15: RBL --flash through our /upgrade; 1.15 -> RTOS:
migrate.bin.gz through 1.15's /upgrade, then one Apply of the modules), settles, then for --minutes: ping once a
second from this PC to the board and to the control, and the relay toggled over MQTT twice a second (each command
checked by the state the board publishes back). One JSON line per block into --log, a summary at the end.

  py -3 code/scripts/bench_ab_ping.py 10.0.5.57 --control 10.0.4.164 --images code/.pio/build_esp8266/ab \
      --modules tools,arduino_emu,... --order rtos,nonos,rtos,nonos --minutes 30 --log ab-ping.log
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", ".."))

import board_modules  # noqa: E402
import firmware_esp_net  # noqa: E402
import firmware_rtos_update as rtos  # noqa: E402

import paho.mqtt.client as mqtt  # noqa: E402

TIME = re.compile(r"time[=<](\d+)ms")


def say(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def firmware(host):
    """'rtos', 'nonos' or None (not answering)"""
    base, op = rtos.opener(host)
    try:
        op.open(base + "modules", timeout=5).read()
        return "rtos"
    except Exception as e:
        if getattr(e, "code", None) == 404:
            return "nonos"
    return None


def wait_firmware(host, want, timeout=240):
    start = time.time()
    time.sleep(8)
    while time.time() - start < timeout:
        if firmware(host) == want:
            return time.time() - start
        time.sleep(3)
    raise RuntimeError(f"{host}: {want} not up within {timeout} s")


def switch(host, want, images, modules):
    now = firmware(host)
    if now == want:
        say(f"already {want}")
    elif want == "nonos":
        base, op = rtos.opener(host)
        say("modules off:", rtos.post(base, op, "modules/off"))
        rtos.wait(base, op, "modules off")
        ok, resp, dt = firmware_esp_net.Device(host, 80).upload(os.path.join(images, "espurna-115-flash.rbl"))
        say(f"1.15 RBL: {resp} in {dt:.1f} s")
        say(f"1.15 up after {wait_firmware(host, 'nonos'):.0f} s")
    else:
        ok, resp, dt = firmware_esp_net.Device(host, 80).upload(os.path.join(images, "espurna-esp8266-migrate.bin.gz"))
        say(f"migrate: {resp} in {dt:.1f} s")
        say(f"RTOS up after {wait_firmware(host, 'rtos'):.0f} s")

    if want == "rtos":
        dev = firmware_esp_net.Device(host, 80)
        st, body = dev.get("/modules", auth=True)
        state = json.loads(body)
        running = sorted(m["name"] for m in state.get("modules", []) if m.get("state") == "running")
        if running != sorted(modules.split(",")) or state.get("off"):
            board_modules.apply(dev, os.path.join(images, "espurna-esp8266.ota"), modules)
            time.sleep(20)
            st, body = dev.get("/modules", auth=True)
            state = json.loads(body)
            running = sorted(m["name"] for m in state.get("modules", []) if m.get("state") == "running")
        say(f"head {state.get('md5', '')[:8]}, running {len(running)}: {','.join(running)}")
        if running != sorted(modules.split(",")):
            raise RuntimeError("not every module is running")


class Pinger(threading.Thread):
    def __init__(self, host, count):
        super().__init__(daemon=True)
        self.host, self.count, self.times, self.lost = host, count, [], 0

    def run(self):
        proc = subprocess.Popen(["ping", "-n", str(self.count), "-w", "1000", self.host], stdout=subprocess.PIPE,
                                text=True, errors="replace")
        for line in proc.stdout:
            m = TIME.search(line)
            if m and "TTL=" in line:
                self.times.append(int(m.group(1)))
            elif ("timed out" in line) or ("unreachable" in line) or ("General failure" in line):
                self.lost += 1
        proc.wait()

    def summary(self):
        t = sorted(self.times)

        def q(p):
            return t[min(len(t) - 1, int(p * len(t)))] if t else None

        return {"sent": len(t) + self.lost, "lost": self.lost, "p50": q(0.5), "p90": q(0.9), "p99": q(0.99),
                "max": t[-1] if t else None}


class Toggler(threading.Thread):
    """the relay over MQTT twice a second; a command counts when its state comes back before the next one"""

    def __init__(self, broker, root, seconds):
        super().__init__(daemon=True)
        self.broker, self.root, self.seconds = broker, root, seconds
        self.sent, self.confirmed, self.latency, self.pending = 0, 0, [], {}
        self.lock = threading.Lock()

    def on_message(self, client, userdata, msg):
        value = msg.payload.decode(errors="replace").strip()
        now = time.time()
        with self.lock:
            at = self.pending.pop(value, None)
            if at is not None and (now - at) < 0.7:
                self.confirmed += 1
                self.latency.append(int((now - at) * 1000))

    def run(self):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="bench-ab-%d" % os.getpid())
        client.on_message = self.on_message
        client.connect(self.broker, 1883, 30)
        client.subscribe(self.root + "/relay/0", 0)
        client.loop_start()
        end = time.time() + self.seconds
        # on, 0.3 s later off: two switches a second, the relay's own 0.5 s pulse (relayPulse0) never fires
        while time.time() < end:
            for value, pause in (("1", 0.3), ("0", 0.7)):
                with self.lock:
                    self.pending[value] = time.time()
                client.publish(self.root + "/relay/0/set", value, qos=0, retain=False)
                self.sent += 1
                time.sleep(pause)
        time.sleep(2)
        client.loop_stop()
        client.disconnect()

    def summary(self):
        t = sorted(self.latency)
        return {"sent": self.sent, "confirmed": self.confirmed,
                "p50": t[len(t) // 2] if t else None, "p90": t[int(0.9 * len(t))] if t else None}


def block(args, kind):
    switch(args.host, kind, args.images, args.modules)
    say(f"settle {args.settle} s")
    time.sleep(args.settle)
    seconds = args.minutes * 60
    board, control = Pinger(args.host, seconds), Pinger(args.control, seconds)
    toggler = Toggler(args.broker, args.root, seconds)
    for thread in (board, control, toggler):
        thread.start()
    say(f"{kind}: {args.minutes} min ping + MQTT")
    for thread in (board, control, toggler):
        thread.join(seconds + 120)
    out = {"at": time.strftime("%Y-%m-%d %H:%M"), "firmware": kind, "board": board.summary(),
           "control": control.summary(), "mqtt": toggler.summary(), "after": firmware(args.host)}
    say(json.dumps(out))
    with open(args.log, "a") as f:
        f.write(json.dumps(out) + "\n")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("--control", required=True)
    p.add_argument("--images", required=True)
    p.add_argument("--modules", required=True)
    p.add_argument("--order", default="rtos,nonos,rtos,nonos")
    p.add_argument("--minutes", type=float, default=30)
    p.add_argument("--settle", type=int, default=60)
    p.add_argument("--broker", default="10.0.0.2")
    p.add_argument("--root", default="ESPURNA-WEMOS")
    p.add_argument("--log", required=True)
    args = p.parse_args()

    results = [block(args, kind) for kind in args.order.split(",")]
    for kind in ("rtos", "nonos"):
        mine = [r for r in results if r["firmware"] == kind]
        sent = sum(r["board"]["sent"] for r in mine)
        lost = sum(r["board"]["lost"] for r in mine)
        csent = sum(r["control"]["sent"] for r in mine)
        clost = sum(r["control"]["lost"] for r in mine)
        msent = sum(r["mqtt"]["sent"] for r in mine)
        mok = sum(r["mqtt"]["confirmed"] for r in mine)
        say(f"TOTAL {kind}: board lost {lost}/{sent} ({100.0 * lost / max(sent, 1):.2f} %), control lost "
            f"{clost}/{csent}, mqtt confirmed {mok}/{msent}")


if __name__ == "__main__":
    main()
