# PROTOTYPE (docs/esp8266-modules.md): link ESP8266 modules against the head image and package them.
#
# Extra script of the modules env of platformio_modules.ini. The env compiles the tree with every module's feature on
# (its own firmware.elf is such an image, not used), then each module of
#
#   custom_modules =
#       <name> | <objects always linked> | <objects linked only for what the head does not have> | <API it provides>
#
# is linked twice with scripts/esp8266_module.ld at two sets of placeholder bases. The words that differ by the flash /
# RAM / IRAM base delta are the relocations; anything else differing stops the build.
#
# API it provides: functions (plain names, every overload) other modules call through the head. The head defines them
# as forwarders (modules_api_esp8266.cpp) to a slot of espurna_module_api[], which this module's entry fills; in this
# module's objects the real ones are renamed (espmod_<symbol>), so another module's call always goes through the head
# and finds nothing (a default) when this module is not installed.
#
# Head functions a module needs but the head dropped (--gc-sections) are appended to custom_module_exports, the head
# keeps them (scripts/esp8266_head_exports.py): rebuild the head when the link says so.
#
# Output next to firmware.elf: module-<name>.module (the package for /modules/upload), module-<name>.elf / .map.

import hashlib
import os
import re
import shutil
import struct
import subprocess
import zlib

from SCons.Script import Import

Import("env")
env = globals()["env"]

FLASH_A, FLASH_B = 0x40400000, 0x40600000
RAM_A, RAM_B = 0x50000000, 0x50100000
IRAM_A, IRAM_B = 0x60000000, 0x60300000

# every base moves by its own delta: a word that differs by one of them tells which base it holds
assert len({FLASH_B - FLASH_A, RAM_B - RAM_A, IRAM_B - IRAM_A}) == 3

HEADER_MAGIC = 0x31444D45
PACKAGE_MAGIC = 0x31504D45
PACKAGE = struct.Struct("<II16s16sIIIIIIII")
CHUNK = 4096
RENAMED = "espmod_"


class MissingSymbols(Exception):
    def __init__(self, names, log):
        super().__init__(f"{len(names)} undefined symbols")
        self.names = names
        self.log = log


def option(name, default=""):
    return env.GetProjectOption("custom_module_" + name, default)


def tool(name):
    folder = os.path.dirname(env.subst("$CC"))
    return os.path.join(folder, "xtensa-lx106-elf-" + name) if folder else "xtensa-lx106-elf-" + name


def run(args):
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        print(" ".join(args))
        print(proc.stdout)
        print(proc.stderr)
        raise RuntimeError(f"{args[0]} failed")
    return proc.stdout


def symbols(elf):
    out = {}
    for line in run([tool("nm"), elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def symbol_sizes(elf):
    out = {}
    for line in run([tool("nm"), "-S", elf]).splitlines():
        parts = line.split()
        if len(parts) == 4:
            out[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return out


def read_map(head_dir):
    with open(os.path.join(head_dir, "firmware.map"), errors="replace") as f:
        return f.read().replace("\\", "/")


def archives_in(build_dir):
    out = {}
    for root, _, files in os.walk(build_dir):
        if os.path.basename(root) == "src" or "mod-" in root.replace(chr(92), "/"):
            continue
        for name in files:
            if name.endswith(".a") and not name.startswith("module-"):
                out[name] = os.path.join(root, name)
    return out


def archive_members(archive):
    """{member: [(type, name)]} of the defined symbols, locals too"""
    out = {}
    member = None
    for line in run([tool("nm"), "--defined-only", archive]).splitlines():
        line = line.rstrip()
        if line.endswith(".o:") or line.endswith(".obj:"):
            member = line[:-1]
            out[member] = []
            continue
        parts = line.split()
        if member and len(parts) == 3:
            out[member].append((parts[1], parts[2]))
    return out


def lib_archives(build_dir, head_dir, head_map, head_names):
    """the libraries for a module link, without the members the head has state of.

    A member the head links and that keeps state (.data / .bss: interrupt table, waveform, timer1, the MDNS object)
    exists once, in the head: what a module needs of it is exported by the head. Any other member may come with the
    module (code the head dropped, or never needed), its calls into the head resolve to the head.
    Returns (archives, symbols of the members left out)."""
    work = os.path.join(build_dir, "mod-libs")
    os.makedirs(work, exist_ok=True)
    out = []
    head_only = set()
    for name, path in sorted(archives_in(build_dir).items()):
        members = archive_members(path)
        drop = []
        for member, defined in members.items():
            state = any(kind in "dDbB" for kind, _ in defined)
            in_head = any(symbol in head_names for kind, symbol in defined if kind in "TtDdBbRr")
            if state and in_head:
                drop.append(member)
                head_only.update(symbol for kind, symbol in defined if kind.isupper())
        if not drop:
            out.append(path)
            continue
        keep = [member for member in members if member not in drop]
        if not keep:
            continue
        filtered = os.path.join(work, name)
        shutil.copyfile(path, filtered)
        run([tool("ar"), "d", filtered] + drop)
        run([tool("ar"), "s", filtered])
        out.append(filtered)
    return out, head_only


def head_defined(head_dir, head_only):
    """global symbols of the head's own objects (before --gc-sections) and of the library members it keeps state of"""
    out = set(head_only)
    for root, _, names in os.walk(os.path.join(head_dir, "src")):
        for name in names:
            if name.endswith(".o"):
                proc = subprocess.run([tool("nm"), "-g", "--defined-only", os.path.join(root, name)],
                                      capture_output=True, text=True)
                for line in proc.stdout.splitlines():
                    parts = line.split()
                    if len(parts) == 3 and parts[1] in "TDRBVWtdrb":
                        out.add(parts[2])
    return out


def update_exports(head_dir, head_only, missing):
    path = os.path.join(env.subst("$PROJECT_DIR"), option("exports"))
    defined = head_defined(head_dir, head_only)
    exported = [name for name in missing if name in defined]
    absent = [name for name in missing if name not in defined]
    if exported:
        known = set()
        if os.path.exists(path):
            with open(path) as f:
                known = {line.strip() for line in f if line.strip() and not line.startswith("#")}
        with open(path, "a", newline=chr(10)) as f:
            for name in exported:
                if name not in known:
                    f.write(name + chr(10))
    return exported, absent


def head_object(head_dir, head_map, head_env, object_name):
    """the head links this object and it defines something (an object of a part that is off is empty)"""
    path = os.path.join(head_dir, "src", object_name)
    if not os.path.exists(path) or f"{head_env}/src/{object_name}" not in head_map:
        return False
    return any(len(line.split()) == 3 and line.split()[1] in "TDBR"
               for line in run([tool("nm"), "-g", "--defined-only", path]).splitlines())


def rename_api(work, objects, provides):
    """copies of the objects with the provided API renamed (definitions and the module's own calls)"""
    os.makedirs(work, exist_ok=True)
    copies = []
    for path in objects:
        copy = os.path.join(work, os.path.basename(path))
        shutil.copyfile(path, copy)
        copies.append(copy)
    if not provides:
        return copies, []

    renames = {}
    for copy in copies:
        plain = run([tool("nm"), "-g", "--defined-only", copy]).splitlines()
        demangled = run([tool("nm"), "-g", "-C", "--defined-only", copy]).splitlines()
        for raw, nice in zip(plain, demangled):
            raw_parts = raw.split(None, 2)
            nice_parts = nice.split(None, 2)
            if len(raw_parts) != 3 or raw_parts[1] != "T":
                continue
            base = nice_parts[2].split("(", 1)[0]
            if base in provides:
                renames[raw_parts[2]] = RENAMED + raw_parts[2]

    found = {name for name in provides if any(
        run([tool("c++filt"), raw]).strip().split("(", 1)[0] == name for raw in renames)}
    missing = sorted(set(provides) - found)
    if missing:
        raise RuntimeError(f"provided API not defined in the module objects: {' '.join(missing)}")

    table = os.path.join(work, "renames.txt")
    with open(table, "w", newline=chr(10)) as f:
        for old, new in sorted(renames.items()):
            f.write(f"{old} {new}" + chr(10))
    for copy in copies:
        run([tool("objcopy"), f"--redefine-syms={table}", copy])
    return copies, sorted(renames)


def link(build_dir, name, head_elf, objects, archives, bases, tag):
    template = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "esp8266_module.ld")
    with open(template) as f:
        script = f.read()
    flash, ram, iram = bases
    script = script.replace("@FLASH@", f"0x{flash:08X}").replace("@RAM@", f"0x{ram:08X}").replace("@IRAM@", f"0x{iram:08X}")
    ld = os.path.join(build_dir, f"module-{name}-{tag}.ld")
    with open(ld, "w") as f:
        f.write(script)

    elf = os.path.join(build_dir, f"module-{name}-{tag}.elf")
    mapfile = os.path.join(build_dir, f"module-{name}-{tag}.map")
    libpaths = [env.subst(p) for p in env["LIBPATH"]]
    args = [
        env.subst("$CC"),
        "-nostdlib",
        "-Wl,-static",
        "-Wl,-EL",
        "-Wl,--gc-sections",
        "-Wl,--no-relax",
        "-Wl,--allow-multiple-definition",
        "-Wl,--no-check-sections",
        f"-Wl,--just-symbols={head_elf}",
        f"-Wl,-T,{ld}",
        f"-Wl,-Map,{mapfile}",
        "-Wl,--cref",
        "-o",
        elf,
    ]
    args += objects
    args += [f"-L{p}" for p in libpaths]
    args += ["-Wl,--start-group"] + archives + ["-lstdc++", "-lm", "-lc", "-lgcc", "-lhal", "-Wl,--end-group"]
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        missing = sorted(set(re.findall(r"undefined reference to `([^']+)'", proc.stderr)))
        if missing:
            raise MissingSymbols(missing, proc.stderr)
        print(proc.stdout)
        print(proc.stderr)
        raise RuntimeError(f"module {name}: link failed")

    binary = os.path.join(build_dir, f"module-{name}-{tag}.bin")
    run([tool("objcopy"), "-O", "binary", "-j", ".mod_flash", "-j", ".mod_iram", "-j", ".mod_data", elf, binary])
    with open(binary, "rb") as f:
        return elf, mapfile, f.read()


def pulled_members(mapfile):
    out = []
    with open(mapfile, errors="replace") as f:
        lines = f.read().split("Allocating common symbols")[0].split("Discarded input sections")[0]
    for line in lines.splitlines():
        line = line.strip()
        if line.endswith(")") and "(" in line and ".a(" in line and not line.startswith("("):
            out.append(line.replace("\\", "/").rsplit("/", 1)[-1])
    return sorted(set(out))


def package(build_dir, name, head_env, head_md5, head_syms, objects, archives):
    elf_a, map_a, image_a = link(build_dir, name, head_env["elf"], objects, archives, (FLASH_A, RAM_A, IRAM_A), "a")
    _, _, image_b = link(build_dir, name, head_env["elf"], objects, archives, (FLASH_B, RAM_B, IRAM_B), "b")
    if len(image_a) != len(image_b) or len(image_a) % 4:
        raise RuntimeError(f"module {name}: images differ in size: {len(image_a)} / {len(image_b)}")

    deltas = {FLASH_B - FLASH_A: 1, RAM_B - RAM_A: 2, IRAM_B - IRAM_A: 3}
    kinds = []
    counts = [0, 0, 0, 0]
    for offset in range(0, len(image_a), 4):
        a, = struct.unpack_from("<I", image_a, offset)
        b, = struct.unpack_from("<I", image_b, offset)
        kind = 0
        if a != b:
            kind = deltas.get((b - a) & 0xFFFFFFFF)
            if kind is None:
                raise RuntimeError(f"module {name}: word at +0x{offset:X} differs by 0x{(b - a) & 0xFFFFFFFF:X}")
        kinds.append(kind)
        counts[kind] += 1

    syms = symbols(elf_a)
    ram_size = syms["__mod_bss_end"] - syms["__mod_data_start"]
    iram_size = syms["__mod_iram_end"] - syms["__mod_iram_start"]
    if syms["__mod_flash_start"] != FLASH_A or struct.unpack_from("<I", image_a, 0)[0] != HEADER_MAGIC:
        raise RuntimeError(f"module {name}: no module header at the start (KEEP .module_header)")
    if syms["__mod_flash_end"] - FLASH_A != len(image_a):
        raise RuntimeError(f"module {name}: image is {len(image_a)} bytes, flash range {syms['__mod_flash_end'] - FLASH_A}")

    # the head MD5 into the header (offset 24, 16 bytes), same in every placement
    image = bytearray(image_a)
    image[24:40] = head_md5

    body = bytearray()
    for start in range(0, len(image), CHUNK):
        chunk = image[start : start + CHUNK]
        words = kinds[start // 4 : start // 4 + CHUNK // 4]
        bitmap = bytearray(CHUNK // 16)
        for index, kind in enumerate(words):
            bitmap[index >> 2] |= kind << ((index & 3) * 2)
        body += bitmap + chunk

    header = PACKAGE.pack(PACKAGE_MAGIC, 1, name.encode()[:15].ljust(16, b"\0"), head_md5, len(image), ram_size,
                          iram_size, FLASH_A, RAM_A, IRAM_A, zlib.crc32(body) & 0xFFFFFFFF, 0)
    out = os.path.join(build_dir, f"module-{name}.module")
    with open(out, "wb") as f:
        f.write(header + body)

    ram_arena = head_syms.get("espurna_modules_ram_arena", (0, 0))[1]
    iram_arena = head_syms.get("espurna_modules_iram_arena", (0, 0))[1]
    print(f"Module '{name}': image {len(image)} bytes, RAM {ram_size} (data "
          f"{syms['__mod_data_end'] - syms['__mod_data_start']}, bss {syms['__mod_bss_end'] - syms['__mod_bss_start']})"
          f" of {ram_arena}, IRAM {iram_size} of {iram_arena}; relocations flash {counts[1]}, RAM {counts[2]}, "
          f"IRAM {counts[3]}; package {len(header) + len(body)}")
    print(f"  {name} pulled: {' '.join(pulled_members(map_a))}")

    final_elf = os.path.join(build_dir, f"module-{name}.elf")
    if os.path.exists(final_elf):
        os.remove(final_elf)
    os.replace(elf_a, final_elf)
    os.replace(map_a, os.path.join(build_dir, f"module-{name}.map"))
    return ram_size, iram_size


def parse_modules():
    out = []
    for line in env.GetProjectOption("custom_modules", "").splitlines():
        line = line.strip()
        if not line:
            continue
        parts = [part.strip() for part in line.split("|")] + ["", "", ""]
        out.append({
            "name": parts[0],
            "objects": parts[1].split(),
            "optional": parts[2].split(),
            "provides": parts[3].split(),
        })
    return out


def modules_link(target, source, env):
    build_dir = env.subst("$BUILD_DIR")
    head_env = option("head")
    head_dir = os.path.join(os.path.dirname(build_dir), head_env)
    head_elf = os.path.join(head_dir, "firmware.elf")
    head_bin = os.path.join(head_dir, "firmware.bin")
    if not os.path.exists(head_elf) or not os.path.exists(head_bin):
        raise RuntimeError(f"{head_elf}: build env:{head_env} first")
    with open(head_bin, "rb") as f:
        head_md5 = hashlib.md5(f.read()).digest()
    head_map = read_map(head_dir)
    head_syms = symbol_sizes(head_elf)
    print(f"Head {head_env}: md5 {head_md5.hex()}")

    src = os.path.join(build_dir, "src")
    head_names = set(symbols(head_elf))
    libraries, head_only = lib_archives(build_dir, head_dir, head_map, head_names)
    missing = {}
    totals = [0, 0]
    for module in parse_modules():
        name = module["name"]
        objects = [os.path.join(src, o) for o in module["objects"]]
        optional = []
        for object_name in module["optional"]:
            if head_object(head_dir, head_map, head_env, object_name):
                continue
            if os.path.exists(os.path.join(src, object_name)):
                optional.append(os.path.join(src, object_name))

        work = os.path.join(build_dir, f"mod-{name}")
        renamed, api = rename_api(work, objects + optional, set(module["provides"]))
        objects = renamed[: len(objects)]
        archives = []
        if optional:
            archive = os.path.join(build_dir, f"module-{name}-optional.a")
            if os.path.exists(archive):
                os.remove(archive)
            run([tool("ar"), "rcs", archive] + renamed[len(objects):])
            archives.append(archive)
        archives += libraries
        if api:
            print(f"  {name} provides {len(api)} functions through the head")

        try:
            ram, iram = package(build_dir, name, {"elf": head_elf}, head_md5, head_syms, objects, archives)
            totals[0] += ram
            totals[1] += iram
        except MissingSymbols as e:
            missing[name] = e

    if missing:
        exported_all = []
        absent_all = []
        for name, e in missing.items():
            exported, absent = update_exports(head_dir, head_only, e.names)
            exported_all += exported
            for symbol in exported:
                print(f"  {name}: head export added: {symbol}")
            for symbol in absent:
                print(f"  {name}: NOT IN THE HEAD: {symbol}")
            absent_all += absent
        raise RuntimeError(f"{len(set(exported_all))} head exports added (rebuild env:{head_env}), "
                           f"{len(set(absent_all))} symbols the head does not have")

    ram_arena = head_syms.get("espurna_modules_ram_arena", (0, 0))[1]
    iram_arena = head_syms.get("espurna_modules_iram_arena", (0, 0))[1]
    print(f"All modules: RAM {totals[0]} of {ram_arena}, IRAM {totals[1]} of {iram_arena}")
    container = write_container(build_dir, head_bin, [module["name"] for module in parse_modules()])
    write_images(build_dir, head_bin, head_elf, head_syms, container)


def write_images(build_dir, head_bin, head_elf, head_syms, container):
    """initial (the head) and full (head + every module placed + directory + the modules file behind 1 MB)"""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "esp8266_full_image", os.path.join(env.subst("$PROJECT_DIR"), "scripts", "esp8266_full_image.py"))
    full_image = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(full_image)

    plain = symbols(head_elf)
    head = {
        "ram_arena": head_syms["espurna_modules_ram_arena"][0],
        "ram_arena_size": head_syms["espurna_modules_ram_arena"][1],
        "iram_arena": head_syms["espurna_modules_iram_arena"][0],
        "iram_arena_size": head_syms["espurna_modules_iram_arena"][1],
        "text_end": plain["_text_end"],
    }
    # modules whose RAM goes to the IRAM tail in `full` (the rest: the DRAM arena); keep modules with interrupt code
    # in DRAM (byte access to IRAM is emulated, not from an interrupt)
    in_iram = set(env.GetProjectOption("custom_full_ram_in_iram", "").split())
    full_image.build(head_bin, container, build_dir, head, in_iram)


CONTAINER_MAGIC = 0x31434D45  # "EMC1"
CONTAINER_ENTRY = struct.Struct("<16sIIIII12x")


def webui_package(build_dir, head_bin):
    """the WebUI data module: the 'modules' preset (MODULES menu entry) or the 'sensor' one"""
    import importlib.util

    html = os.path.join(env.subst("$PROJECT_DIR"), "html", "build")
    page = os.path.join(html, "index.modules.html.gz")
    if not os.path.exists(page):
        page = os.path.join(html, "index.sensor.html.gz")
    if not os.path.exists(page):
        return None

    spec = importlib.util.spec_from_file_location(
        "esp8266_data_module", os.path.join(env.subst("$PROJECT_DIR"), "scripts", "esp8266_data_module.py"))
    data_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(data_module)

    out = os.path.join(build_dir, "module-webui.module")
    import sys
    argv = sys.argv
    try:
        sys.argv = ["esp8266_data_module.py", "webui", page, head_bin, out]
        data_module.main()
    finally:
        sys.argv = argv
    print(f"  webui from {os.path.basename(page)}")
    return out


def write_container(build_dir, head_bin, names):
    """espurna-esp8266-1m-modules.bin: every package of this build, for the modules page (EMC1, ref. its parse())"""
    paths = [os.path.join(build_dir, f"module-{name}.module") for name in names]
    webui = webui_package(build_dir, head_bin)
    if webui:
        paths.append(webui)

    packages = []
    for path in paths:
        with open(path, "rb") as f:
            packages.append(f.read())

    offset = 8 + (CONTAINER_ENTRY.size * len(packages))
    table = b""
    for data in packages:
        name, image, ram, iram = struct.unpack_from("<8x16s16xIII", data, 0)
        table += CONTAINER_ENTRY.pack(name, offset, len(data), image, ram, iram)
        offset += len(data)

    out = os.path.join(build_dir, "espurna-esp8266-1m-modules.bin")
    with open(out, "wb") as f:
        f.write(struct.pack("<II", CONTAINER_MAGIC, len(packages)) + table + b"".join(packages))
    print(f"Modules file: {out}, {offset} bytes, {len(packages)} modules")
    return out


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", env.VerboseAction(modules_link, "Linking the modules against the head"))

# a new WebUI build (gulp build --preset modules) has to reach the modules file too
for _page in ("index.modules.html.gz", "index.sensor.html.gz"):
    _path = os.path.join(env.subst("$PROJECT_DIR"), "html", "build", _page)
    if os.path.exists(_path):
        env.Depends("$BUILD_DIR/${PROGNAME}.elf", _path)
        break
