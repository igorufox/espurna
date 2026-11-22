#!/usr/bin/env python3
# Wi-Fi RX under a burst: N UDP datagrams to a closed port of the board (as the APs' usteer broadcasts do every second,
# 1500-byte frames and IP fragments), then one ping right after. Run the same against a control board (CAB2).
#   bench_rx_burst.py <host> [--trials 100] [--frames 12] [--size 1400] [--gap 0.02]

import argparse
import socket
import subprocess
import time


def ping_once(host, timeout_ms):
    out = subprocess.run(["ping", "-n", "1", "-w", str(timeout_ms), host], capture_output=True, text=True)
    return "TTL=" in out.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("--trials", type=int, default=100)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--size", type=int, default=1400, help="UDP payload, above 1472 the datagram is fragmented")
    parser.add_argument("--port", type=int, default=16720)
    parser.add_argument("--gap", type=float, default=0.0, help="seconds between the burst and the ping")
    parser.add_argument("--pause", type=float, default=1.0)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = bytes(args.size)
    lost = 0
    for trial in range(args.trials):
        for _ in range(args.frames):
            sock.sendto(payload, (args.host, args.port))
        if args.gap:
            time.sleep(args.gap)
        if not ping_once(args.host, 800):
            lost += 1
        time.sleep(args.pause)

    print(f"{args.host}: frames {args.frames} x {args.size} B, lost {lost} of {args.trials}")


if __name__ == "__main__":
    main()
