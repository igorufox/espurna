#!/usr/bin/env python3
"""Size budgets of the release images (CI, .github/workflows/esp32.yml, build job after the build).

    python3 code/scripts/ci_budget.py --arch esp8266 --name esp8266 dist/espurna-esp8266.elf

Reads the ELF with the toolchain of the env (`<prefix>-size -A -d`, `<prefix>-nm`) and computes the numbers the way
PlatformIO prints them after the build ("RAM: used N bytes", "Flash: used N bytes", the SIZEDATAREGEXP /
SIZEPROGREGEXP of the platform's builder), plus for ESP8266 the heap region 0x3FFFC000 - _heap_start (the static RAM
eats into it: 06.11 ~13 KB of static init on esp8266-4m left ~4 KB of heap at runtime and the board boot-looped).

Limits come from scripts/ci_budget.json, per release name (`name` of the build matrix): flash_max, ram_max,
heap_min; null = reported, not checked. A name missing from the table fails, so a new image gets its row.
Always prints the table (and appends it to $GITHUB_STEP_SUMMARY), exit 1 when a limit is exceeded.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "ci_budget.json")

# arch of the build matrix -> toolchain prefix and PlatformIO's formulas
# - espressif8266/builder/main.py
# - pioarduino platform-espressif32/builder/main.py
# - libretiny/builder/family/beken-72xx.py
ARCHS = {
    "esp8266": {
        "prefix": ["xtensa-lx106-elf"],
        "flash": r"^(?:\.irom0\.text|\.text|\.text1|\.data|\.rodata|)\s+([0-9]+).*",
        "ram": r"^(?:\.data|\.rodata|\.bss)\s+([0-9]+).*",
        "heap_end": 0x3FFFC000,
    },
    "esp32": {
        "prefix": ["xtensa-esp32-elf", "xtensa-esp-elf"],
        "flash": r"^(?:\.iram0\.text|\.iram0\.vectors|\.dram0\.data|\.flash\.text|\.flash\.rodata|)\s+([0-9]+).*",
        "ram": r"^(?:\.dram0\.data|\.dram0\.bss|\.noinit)\s+([0-9]+).*",
    },
    "beken": {
        "prefix": ["arm-none-eabi"],
        "flash": r"^(?:\.vectors|\.text|\.rodata|\.data|\.ARM\.exidx)\s+([0-9]+).*",
        "ram": r"^(?:\.vectors|\.data|\.bss)\s+([0-9]+).*",
    },
}


def find_tool(prefixes, tool):
    """<prefix>-<tool> of the PlatformIO packages (the env's toolchain), else from PATH"""
    core = os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")
    for prefix in prefixes:
        name = f"{prefix}-{tool}"
        for exe in (name, name + ".exe"):
            found = sorted(glob.glob(os.path.join(core, "packages", "toolchain-*", "bin", exe)))
            if found:
                return found[0]
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit(f"no {' / '.join(p + '-' + tool for p in prefixes)} in {core}/packages or PATH")


def section_sum(output, pattern):
    regexp = re.compile(pattern)
    total = 0
    for line in output.splitlines():
        match = regexp.search(line.strip())
        if match:
            total += sum(int(value) for value in match.groups())
    return total


def measure(arch, elf):
    spec = ARCHS[arch]
    size = subprocess.run([find_tool(spec["prefix"], "size"), "-A", "-d", elf],
                          check=True, capture_output=True, text=True).stdout
    out = {
        "flash": section_sum(size, spec["flash"]),
        "ram": section_sum(size, spec["ram"]),
        "heap": None,
    }
    if "heap_end" in spec:
        nm = subprocess.run([find_tool(spec["prefix"], "nm"), elf],
                            check=True, capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == "_heap_start":
                out["heap"] = spec["heap_end"] - int(parts[0], 16)
                break
        else:
            raise SystemExit(f"{elf}: no _heap_start symbol")
    return out


def check(name, measured, limits):
    """rows: (what, value, limit text, verdict)"""
    rows = []
    failed = False
    for key, what, limit_key, over in (
            ("flash", "flash (bytes)", "flash_max", lambda v, l: v > l),
            ("ram", "static RAM (bytes)", "ram_max", lambda v, l: v > l),
            ("heap", "heap region (bytes)", "heap_min", lambda v, l: v < l)):
        value = measured[key]
        if value is None:
            continue
        limit = limits.get(limit_key)
        if limit is None:
            rows.append((what, value, "-", "report only"))
            continue
        bad = over(value, limit)
        failed |= bad
        sign = ">=" if limit_key == "heap_min" else "<="
        rows.append((what, value, f"{sign} {limit}", "OVER BUDGET" if bad else "ok"))
    return rows, failed


def render(name, env, elf, rows):
    head = f"### Size budget: {name} ({env})\n\n`{elf}`\n\n"
    lines = ["| | value | budget | |", "|---|---:|---:|---|"]
    for what, value, limit, verdict in rows:
        lines.append(f"| {what} | {value} | {limit} | {verdict} |")
    return head + "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--arch", required=True, choices=sorted(ARCHS))
    parser.add_argument("--name", required=True, help="release name (`name` of the build matrix)")
    parser.add_argument("--table", default=TABLE)
    parser.add_argument("elf")
    args = parser.parse_args(argv)

    with open(args.table, encoding="utf-8") as f:
        table = json.load(f)["images"]

    entry = table.get(args.name)
    measured = measure(args.arch, args.elf)
    if entry is None:
        rows = [(k, v, "-", "report only") for k, v in measured.items() if v is not None]
        text = render(args.name, "?", args.elf, rows)
        print(text)
        print(f"FAIL: {args.name} has no row in {os.path.relpath(args.table)}, add it (null = report only)")
        failed = True
    else:
        rows, failed = check(args.name, measured, entry)
        text = render(args.name, entry.get("env", "?"), args.elf, rows)
        print(text)
        if failed:
            print(f"FAIL: {args.name} is over its budget in {os.path.relpath(args.table)}. Either the build grew by "
                  "mistake, or raise the row in the same commit and say why")

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
