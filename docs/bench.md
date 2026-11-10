# Bench tests

Network-only bench tools for the fork's hardware tests: `code/scripts/bench_*.py`, Python 3.13 standard library only
(no pip packages). They talk to a board the same way the WebUI does (HTTP Digest, WS terminal, `/upgrade`) and never
need a wire. Serial work (Beken on COM3, ESP8266 rescue by wire) stays manual.

Every tool takes `--host`, prints a human log, and ends with **one machine-readable line**

```
BENCH_RESULT {"tool": "bench_soak", "host": "10.0.5.19", "ok": true, ...}
```

Exit codes: `0` pass, `1` fail (a check or the network), `2` setup error (password file, arguments, authentication),
`3` refused (not a bench board, uptime too low, the board rebooted by itself before an OTA).

## Secrets

- The admin password is read at run time from `%USERPROFILE%\.espurna\pass` (one line). The tools never print it,
  never put it on a command line or into a URL, and never log it. Do not open that file to "check" it.
- Values of secret-looking keys (`pass*`, `wifiApPass`, `apiKey`, `mqttPassword`, `adminPass`, anything with
  `key` / `secret` / `token` / `pwd` / `psk` in its name) are shown as `<redacted>` in every line and JSON dump.
- `bench_ota.py` compares settings by key names plus **salted** hashes of the values (random salt per run, kept in
  memory); only key names are printed.
- `apiKey` for API tests is read over the WS terminal into memory and sent in the `Api-Key` header, never in a URL.

## Tools

| Tool | What it does | Writes / reboots |
|------|--------------|------------------|
| `bench_common.py` | shared module: Digest auth, WS terminal, `/upgrade` upload, `info` / `heap` / `uptime` / `wifi` / `mqtt` / `keys` parsing, redaction, ping thread, bench list | - |
| `bench_soak.py` | N hours, every M s: uptime, heap, max block, RSSI, BSSID, MQTT state; reboots with reset reason, heap trend; CSV log | read-only |
| `bench_heap.py` | median of K `heap` readings | read-only |
| `bench_ws_sessions.py` | N WebSocket sessions open / close in a row, heap before / after | load |
| `bench_parallel_pages.py` | N parallel authenticated `GET /` x rounds, ping in parallel, losses | load |
| `bench_slow_reader.py` | N `GET /` with `SO_RCVBUF` 256 held open, ping in parallel (Beken lwIP RX starvation) | load |
| `bench_relay_pulse.py` | N relay pulses via WS terminal or API, heap at steady state before / after, state restored | switches a relay |
| `bench_regress.py` | quick functional regression: discover, WebUI, WS terminal, info, heap, Wi-Fi, MQTT, NTP, API, temp key | writes + deletes `benchProbe` (skip: `--read-only`) |
| `bench_ota.py` | web OTA of one file, wait for the board, check reset reason / uptime / build / md5 / keys | **flashes, reboots** |
| `bench_espota.py` | ArduinoOTA (ESP8266 only) for minimal images and the softAP rescue | **flashes, reboots** |
| `bench_term.py` | `cmd`, `watch` (WS debug log), `keys` (names only), `dump` (WS dump fields), `inventory`, `http` | `cmd` with a non-read-only command |
| `bench_mqtt.py` | anonymous MQTT 3.1.1 `sub` (state, HA discovery) / `pub` to the broker (default `10.0.0.2`); `pub` only to topics with a bench chip id (D390FC, 79E12A, 4F3E20) | `pub` |

Run them from the repo root, e.g. `python code/scripts/bench_heap.py --host 10.0.5.19`. Every tool has `--help`.
`bench_espota.py` must run with `C:\Python313\python.exe`: the board connects back to this PC and the Windows firewall
allows that interpreter only.

## Bench boards

The tools that load, write or reboot refuse any other host unless `--allow-non-bench` is given, and that flag is used
**only with the user's explicit OK** for that board and that run.

| Board | Chip | Use for | Notes |
|-------|------|---------|-------|
| `10.0.5.19` ESPURNA-D390FC (HW-622) | ESP8266, 4 MB | everything ESP8266: soak, OTA cycles, relay pulses (one relay, `--relay 0`), WS sessions, parallel pages | the test board; recovery without a wire: its softAP + `bench_espota.py` (below) |
| `10.0.5.0` Sonoff Basic | ESP8266, 1 MB | 1 MB images (head + modules), heap, regress, OTA | keys backup in `firmware-dumps/` |
| `10.0.5.58` Sonoff Basic ESPURNA-79E12A | ESP8266, 1 MB | second 1 MB board | runs ESPurna **1.15** until reflashed: its WS terminal action is `dbgcmd`, the tools speak the current `cmd`, so they work on it only after it gets a current image (first flash through its WebUI) |
| `10.0.5.66` LC-Tech relay x2 | ESP32 | ESP32: soak, WS sessions, relay pulses (`--relay 0/1`), OTA (merged `.bin`, md5 not comparable: no `--check-md5`) | same chip as the NSPanel |
| `10.0.5.139` BK7231N | Beken | slow reader, parallel pages, soak (watch `lwip:` in `heap`), regress, web OTA | serial on COM3 is the user's console: read his terminal, never open a second monitor |

### What to run after a refactor (common Wi-Fi, types, ESP32Async web stack, base classes, task / NTP / ISR / OTA interfaces)

On every chip, in this order (stop at the first failure and report it):

1. `bench_regress.py` - everything answers, NTP synced, MQTT connected, settings write / read / delete.
2. `bench_heap.py -k 5` - baseline heap median.
3. `bench_ws_sessions.py -n 100` - WebUI reloads (WS client list, async_tcp client ids).
4. `bench_parallel_pages.py -n 5 --rounds 3` - web stack under parallel load.
5. `bench_slow_reader.py -n 5 --hold 60` - Beken mandatory (the AsyncTCP RX reserve is not upstream), ESP32 / ESP8266
   as a check that slow clients do not stall the loop.
6. `bench_relay_pulse.py -n 100` - relay path, timers, WS / MQTT state broadcast.
7. `bench_ota.py <image>` twice (old -> new -> new again) - OTA interface; ESP8266 with `--check-md5`.
8. `bench_soak.py --hours 8 --interval 60 --csv <file>` - last, overnight on D390FC / 10.0.5.66 / 10.0.5.139.

### Thresholds

| Check | Pass |
|-------|------|
| reboots | none during any test (uptime keeps growing); soak: 0 |
| heap trend | soak: no downward trend (slope below -300 B/h **and** last-quarter median > 1 KB under the first quarter = fail); bursts: median after 60 s within 2 KB of the median before |
| single heap readings | not compared: ESP8266 swings +-4 KB between readings, use `bench_heap.py` medians |
| ping | 0 lost during parallel pages and slow readers |
| WS sessions | 100 of 100 open with an initial dump, heap back within 2 KB |
| relay pulses | 100 of 100 acknowledged, state restored |
| OTA | went offline and back, `Reboot after a successful OTA update`, fresh uptime, system OK, keys unchanged, md5 = image (ESP8266) |
| regress | no FAIL (skip is fine where a module is not built in) |

## Safety rules

- **Never on in-wall or production boards** without the user's explicit OK for that board. Fleet sweeps are read-only
  (`bench_term.py inventory`, `bench_heap.py`, `bench_soak.py`, `bench_regress.py --read-only`).
- **OTA / reboot only at uptime >= 70 s** with system OK and boot counter 1: ESPurna counts soft restarts and resets the
  counter 60 s after boot; three early restarts put the board into unstable mode. `bench_ota.py` and `bench_espota.py`
  wait / refuse accordingly.
- **No OTA after a self reboot.** If `last reset reason` is a watchdog, exception, brownout, crash, trap, stability or
  "network stopped receiving" reboot, the board is unstable: the OTA tools refuse; tell the user (power supply?).
  `--ignore-reset-reason` only with his OK.
- **Never auto-retry an OTA** on in-wall boards. The tools upload once; a failed upload or a board that does not come
  back is reported, then the user decides. Before any second step check that the board did not reboot by itself.
- **Back up keys first** before any OTA or settings write on a board you did not set up yourself:
  `bench_term.py --host <ip> cmd keys > <file>` keeps names and non-secret values (secrets come out redacted); for a
  full backup with secrets use the WebUI backup into `firmware-dumps/` (gitignored), never into the repo or a chat.
- **Never change Wi-Fi settings** (bssid pin, channel, `wifiScan`) on a board without proof it can still join: the
  test board was lost that way once.
- Do not run two load tools against the same board at once, and coordinate with other sessions using the board.

## Recovery of an ESP8266 bench board without a wire

In unstable mode (3 early crashes) only the softAP and ArduinoOTA run. Join the board's softAP (`ESPURNA-<chip>`) with
the PC's second adapter, then

```
C:\Python313\python.exe code/scripts/bench_espota.py --host 192.168.4.1 --host-ip 192.168.4.2 --host-port 16065 --no-precheck --allow-non-bench <image.bin>
```

(`192.168.4.1` is the softAP of whatever board you joined, hence `--allow-non-bench`: make sure it is the bench board.)

## AP-side tests (recipe only, no script)

Roaming / deauth behaviour is tested from the OpenWrt APs. The APs are home infrastructure: every step below needs the
user's OK first, and the cellar AP `10.0.0.9` is never touched. SSH to `lion` (10.0.0.1) goes through WSL:

```
wsl -e bash -c "ssh -o BatchMode=yes -i ~/.ssh/claude_openwrt root@10.0.0.1 sh -s" < script.sh
```

On the AP:

```sh
ubus list 'hostapd.*'                                    # e.g. hostapd.phy0-ap0 (2.4 GHz)
ubus call hostapd.phy0-ap0 get_clients | grep -i -A3 '<board mac>'
# kick the board once: deauth reason 5, no ban (it may rejoin the same AP at once)
ubus call hostapd.phy0-ap0 del_client '{"addr":"<board mac>","reason":5,"deauth":true,"ban_time":0}'
# keep it off this AP for 30 s, so it has to roam to another one
ubus call hostapd.phy0-ap0 del_client '{"addr":"<board mac>","reason":5,"deauth":true,"ban_time":30000}'
logread | grep -i '<board mac>' | tail -20               # association / DHCP afterwards
```

Run `bench_soak.py --interval 10` (or `bench_term.py watch 300` for the debug log) on the board during the kick and
check: no reboot, back on Wi-Fi and MQTT within ~30 s, heap unchanged. Never ban without a time limit.
