# What the module links pull from the libraries (regress-todo.md item 30): per module, the bytes of every archive
# (libstdc++, libc_nano, libgcc, libespurna-core, ...) and of its own objects, from the module's GNU ld map
# ($BUILD_DIR/module-<name>.map, link A). Sizes are the input sections placed in the module's output sections.
#
#   py -3 code/scripts/module_size_report.py <build dir> [<module,...>] [--members]

import os
import re
import sys
from collections import defaultdict

SECTION = re.compile(r"^ (\.[\w.]+)?\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$")
LONG_NAME = re.compile(r"^ (\.[\w.]+)\s*$")


def owner(path):
    path = path.strip().replace("\\", "/")
    match = re.search(r"([^/]+\.a)\(([^)]+)\)$", path)
    if match:
        return match.group(1), match.group(2)
    return "(own)", os.path.basename(path)


def parse(path):
    """{archive: {member: bytes}} of the sections placed in the image"""
    out = defaultdict(lambda: defaultdict(int))
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    in_map = False
    pending = None
    counted = True
    for line in lines:
        # an output section starts at column 0: only what goes into the image (no .debug / .xt.prop / .comment)
        if line.startswith(".") or (line and not line[0].isspace() and line[0] not in "*/"):
            name = line.split()[0] if line.split() else ""
            counted = not name.startswith((".debug", ".xt", ".xtensa", ".comment", ".stab", ".note", ".ARM"))
            if in_map:
                continue
        if line.startswith("Linker script and memory map"):
            in_map = True
            continue
        if not in_map:
            continue
        if line.startswith("/DISCARD/"):
            break

        if not counted:
            pending = None
            continue

        if pending is not None:
            match = re.match(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$", line)
            pending = None
            if match:
                size = int(match.group(2), 16)
                if size and int(match.group(1), 16):
                    archive, member = owner(match.group(3))
                    out[archive][member] += size
            continue

        if LONG_NAME.match(line):
            pending = line
            continue

        match = SECTION.match(line)
        if match and match.group(1):
            size = int(match.group(3), 16)
            if size and int(match.group(2), 16):
                archive, member = owner(match.group(4))
                out[archive][member] += size
    return out


def main(argv):
    build = argv[0]
    members = "--members" in argv
    names = None
    rest = [arg for arg in argv[1:] if not arg.startswith("--")]
    if rest:
        names = rest[0].split(",")

    maps = sorted(name for name in os.listdir(build)
                  if name.startswith("module-") and name.endswith(".map") and not name.endswith("-b.map"))
    totals = defaultdict(int)
    for name in maps:
        module = name[len("module-"):-len(".map")]
        if names and module not in names:
            continue
        data = parse(os.path.join(build, name))
        whole = sum(sum(m.values()) for m in data.values())
        parts = sorted(((sum(m.values()), archive) for archive, m in data.items()), reverse=True)
        print("%-16s %7d  %s" % (module, whole, "  ".join("%s %d" % (a, s) for s, a in parts)))
        for size, archive in parts:
            totals[archive] += size
            if members and archive != "(own)":
                for member, msize in sorted(data[archive].items(), key=lambda x: -x[1]):
                    print("    %-20s %-28s %6d" % (archive, member, msize))
    print("total by archive: " + "  ".join("%s %d" % (a, s) for a, s in sorted(totals.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main(sys.argv[1:])
