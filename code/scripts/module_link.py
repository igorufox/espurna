# PROTOTYPE (docs/modules.md): linking modules against a head image and packaging them, every chip.
#
# One env per chip builds everything (platformio_<chip>.ini, env:<chip>): the head is the env's own image, the module
# objects are compiled in the same build with the module flags, then every module is linked against the head and the
# release images are written. A chip script (the last extra script of the env: esp8266_module_link.py, ...) subclasses
# Linker below with what only that chip has (its toolchain, the bases of its linker script, its relocation records,
# its head identity, what it writes after the modules file), then calls install().
#
#   custom_module_flags = -D...      macros of the module objects: replace the head's macros of the same names
#   custom_module_undef = NAME ...   macros of the head the module objects do not get (MODULES_SUPPORT ...)
#   custom_module_libs = NAME ...    libraries only the modules use, compiled with the module flags (the head's
#                                    lib_ignore keeps them out of its LDF): <libdeps>/<NAME> or the framework's
#   custom_modules =
#       <name> | <objects always linked> | <objects linked only for what the head does not have> | <API it provides>
#           | <modules it needs>
#
# The module objects go to $BUILD_DIR/mod-src (the head's are in $BUILD_DIR/src), the module libraries to
# $BUILD_DIR/libmod. Each module is linked against the head's ELF: its calls go to the head, what it needs that the
# head dropped (--gc-sections) is appended to custom_head_exports (scripts/module_head.py keeps it in the head) and the
# head is linked again in the same build (up to RELINKS times), then the modules.
#
# API it provides: functions (plain names, every overload) other modules call through the head. The head defines them
# as forwarders to a slot of espurna_module_api[], which this module's entry fills; in this module's objects the real
# ones are renamed (espmod_<symbol>), so another module's call always goes through the head.
#
# Library members the head links and that keep state (.data / .bss) exist once, in the head; any other member may
# come with the module.
#
# The link: scripts/<chip>_module.ld with the shared part scripts/module_sections.ld (@SECTIONS@), linked twice at two
# sets of placeholder bases (Linker.bases, @<NAME>@ in the script). The words that differ by the delta of one base are
# its relocations (kind 1, 2, 3 in the order of Linker.bases); anything else differing stops the build, unless the chip
# knows it (Beken: branches into the head).
#
# The package: PACKAGE, then per CHUNK of the image the map of 2 bits per word (the relocation kind), what the chip
# adds per chunk (Beken: its branch list) and the chunk. The module header (modules.h) in the image gets the head id
# (offset 24) and the modules it needs (HEADER_NEEDS).

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import zlib

# loaded with this file: the folder of the scripts is on sys.path only while the env's scripts run
import module_ota  # noqa: E402  pylint: disable=wrong-import-position

HEADER_MAGIC = 0x31444D45   # "EMD1"
PACKAGE_MAGIC = 0x31504D45  # "EMP1"
# magic, version, name, head id, image size, RAM, IRAM, the link A bases of kind 1, 2, 3, crc32 of the body, branches
PACKAGE = struct.Struct("<II16s16sIIIIIIII")
CHUNK = 4096
MAP_SIZE = CHUNK // 16
RENAMED = "espmod_"

# the modules file: name, offset, package bytes, image bytes, RAM, second arena, the modules it needs (EMC2)
CONTAINER_MAGIC = 0x32434D45  # "EMC2"
CONTAINER_ENTRY = struct.Struct("<16sIIIII32sI")
# flags of a container entry: its RAM takes byte access from interrupts (custom_module_byte_ram), not into a pool that
# takes 32-bit access only (ESP8266: the IRAM tail)
CONTAINER_BYTE_RAM = 1

# the module header (modules.h): the head id at 24, `needs` after the 96 bytes of the first version
HEADER = struct.Struct("<II16s16s14I32s")
HEADER_ID = 24
HEADER_REQUIRES = 96
REQUIRES_SIZE = 32

# head links in one build when the modules need head exports it does not have yet
RELINKS = 3

MODULE_SRC = "mod-src"
MODULE_LIBS = "libmod"

# the module link
LINK_FLAGS = (
    "-nostdlib",
    "-Wl,-static",
    "-Wl,-EL",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,--allow-multiple-definition",
    "-Wl,--no-check-sections",
    "-Wl,--orphan-handling=place",
)
# sections of a module ELF that take no memory
NO_MEMORY = (".debug", ".xt", ".ARM.attributes", ".comment", ".rel")


def round_up(value, to):
    return (value + to - 1) // to * to


def module_name(name):
    return name.encode()[:15].ljust(16, b"\0")


def pack_body(image, kinds, chunk_extra=None):
    """per CHUNK: the map (2 bits per word), chunk_extra(start, size) when given, the bytes"""
    body = bytearray()
    for start in range(0, len(image), CHUNK):
        chunk = image[start: start + CHUNK]
        bitmap = bytearray(MAP_SIZE)
        for index, kind in enumerate(kinds[start // 4: start // 4 + CHUNK // 4]):
            bitmap[index >> 2] |= kind << ((index & 3) * 2)
        body += bitmap
        if chunk_extra:
            body += chunk_extra(start, len(chunk))
        body += chunk
    return bytes(body)


def package_bytes(version, name, head_id, image, kinds, ram, iram, bases, chunk_extra=None, count=0):
    """PACKAGE + body; bases: the link A bases of kind 1, 2, 3 (0 for what the chip has not)"""
    body = pack_body(image, kinds, chunk_extra)
    bases = list(bases) + [0] * (3 - len(bases))
    header = PACKAGE.pack(PACKAGE_MAGIC, version, module_name(name), head_id, len(image), ram, iram, *bases,
                          zlib.crc32(body) & 0xFFFFFFFF, count)
    return header + body


def data_package(name, data, head_id, link_flash):
    """a data module (no code, entry 0, e.g. the WebUI page): the header + the data, padded to 4 bytes; only
    flash_start / flash_end hold addresses, they are its two flash relocations (words 10 and 11)"""
    size = round_up(HEADER.size + len(data), 4)
    fields = [0] * 14  # the last one: priority 0
    fields[0] = link_flash         # flash_start
    fields[1] = link_flash + size  # flash_end
    fields[4] = len(data)          # data_end: the data size
    image = HEADER.pack(HEADER_MAGIC, 1, module_name(name), head_id, *fields, b"") + data
    image += b"\0" * (size - len(image))
    kinds = [0] * (size // 4)
    kinds[10] = kinds[11] = 1
    out = package_bytes(1, name, head_id, image, kinds, 0, 0, (link_flash, 0, 0))
    print(f"data module '{name}': {len(data)} bytes of data, image {len(image)}, package {len(out)}, "
          f"head md5 {head_id.hex()}")
    return out


def head_exports_path(env):
    path = env.GetProjectOption("custom_head_exports", "")
    return os.path.join(env.subst("$PROJECT_DIR"), path) if path else ""


def read_head_exports(env):
    path = head_exports_path(env)
    if not path or not os.path.exists(path):
        return []
    with open(path) as f:
        return [line.strip() for line in f if line.strip() and not line.startswith("#")]


def write_head_exports(env):
    """$BUILD_DIR/head-exports.ld: EXTERN() of every head export (an implicit linker script: -Wl,-u for each made the
    command line too long for Windows). Returns (script, number of names)"""
    names = read_head_exports(env)
    script = os.path.join(env.subst("$BUILD_DIR"), "head-exports.ld")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    with open(script, "w", newline=chr(10)) as f:
        for name in names:
            f.write(f"EXTERN({name})" + chr(10))
    return script.replace(chr(92), "/"), len(names)


def define_name(item):
    if isinstance(item, (list, tuple)):
        return str(item[0])
    if isinstance(item, dict):
        return next(iter(item), "")
    return str(item).split("=", 1)[0]


class MissingSymbols(Exception):
    def __init__(self, names, log):
        super().__init__(f"{len(names)} undefined symbols")
        self.names = names
        self.log = log


class Linker:
    # the chip's toolchain prefix ("xtensa-lx106-elf-", when $CC is not <prefix>gcc), its modules file name
    prefix = ""
    container_name = ""
    # the head's files in its build dir (LibreTiny: raw_firmware.elf / raw_firmware.map)
    head_elf_name = "firmware.elf"
    head_bin_name = "firmware.bin"
    head_map_name = "firmware.map"
    # the head's outputs (in $BUILD_DIR) the modules link waits for, built again in this order after a head relink
    head_outputs = ("${PROGNAME}.elf", "${PROGNAME}.bin")

    # the link of one module: scripts/<link_script>, its placeholder bases in the order of the relocation kinds
    # 1, 2, 3 of the package: (placeholder, base of link A, base of link B, name in the summary)
    link_script = ""
    bases = ()
    package_version = 1
    # sections of the module ELF that may take memory; the ones its image is made of, in flash order
    own_sections = ()
    image_sections = ()
    # the image is padded to this (Beken: the block of the flash format)
    image_align = 4
    # flags of the head link that a module link takes too (prefixes of LINKFLAGS), more options of the module link
    head_link_flags = ()
    link_options = ()
    # the toolchain's libraries every module links with
    system_libs = ("-lstdc++", "-lm", "-lc", "-lgcc")
    # the module's IRAM (ESP8266): its own arena, its size in the package
    has_iram = False
    # RAM of a module above the head's arena stops the build (ESP8266: RAM may go to the IRAM tail instead)
    ram_in_arena = True

    def __init__(self, env, projenv=None):
        self.env = env
        # the sources of the project are built with projenv (build_src_flags, library include paths)
        self.projenv = projenv if projenv is not None else env
        self._executors = []
        self._members_cache = None
        assert len({b - a for _, a, b, _ in self.bases}) == len(self.bases), "every base needs its own delta"

    # --- chip hooks ------------------------------------------------------------------------------------------------

    def head_id(self, ctx):
        """the id the modules carry of the head they are linked against: the MD5 of firmware.bin"""
        with open(ctx["head_bin"], "rb") as f:
            return hashlib.md5(f.read()).digest()

    def head_symbols(self, build_dir, head_elf):
        """the head's symbols in a module link"""
        return f"-Wl,--just-symbols={head_elf}"

    def library_flags(self):
        """more options in front of the libraries of a module link (-L)"""
        return []

    def branches(self, name, elf, image_a, image_b):
        """{offset: kind} of the words the board moves other than by the map (Beken: branches into the head)"""
        return {}

    def odd_word(self, image_a, image_b, offset, branches):
        """a word that differs by no base delta and that the chip knows (adds it to branches): True"""
        return False

    def chunk_extra(self, branches):
        """what the package has per chunk between the map and the bytes: f(start, size) or None"""
        return None

    def check(self, name, syms):
        """more checks of the linked module (its symbols)"""

    def image_detail(self, syms):
        """the image line of the summary, after its size"""
        return ""

    def report(self, name, mapfile):
        """warnings about the linked module"""

    def extra_packages(self, ctx):
        """more packages for the modules file (a data module), paths"""
        return []

    def extra_sources(self):
        """more files the modules link depends on (the WebUI page of a data module)"""
        return []

    def after(self, ctx, container):
        """after the modules file (images, reports)"""

    def ota_heads(self, ctx):
        """[(name, path)] of the heads in the install image espurna-<chip>.ota (scripts/module_ota.py)"""
        return []

    def arenas(self, ctx):
        """names and sizes of the head's arenas: RAM first"""
        return [("RAM", self.arena())]

    # --- tools -----------------------------------------------------------------------------------------------------

    def option(self, name, default=""):
        return self.env.GetProjectOption("custom_module_" + name, default)

    def tool(self, name):
        cc = self.env.subst("$CC")
        if cc.endswith("gcc"):
            return cc[: -len("gcc")] + name
        folder = os.path.dirname(cc)
        return os.path.join(folder, self.prefix + name) if folder else self.prefix + name

    def script(self, name):
        return os.path.join(self.env.subst("$PROJECT_DIR"), "scripts", name)

    def arena(self):
        """RAM of the modules: no fixed arena on any chip (the Apply sizes it, a heap block and the chip's pool), 0"""
        return 0

    @staticmethod
    def run(args):
        proc = subprocess.run(args, capture_output=True, text=True)
        if proc.returncode != 0:
            print(" ".join(args))
            print(proc.stdout)
            print(proc.stderr)
            raise RuntimeError(f"{args[0]} failed")
        return proc.stdout

    def symbols(self, elf):
        out = {}
        for line in self.run([self.tool("nm"), elf]).splitlines():
            parts = line.split()
            if len(parts) == 3:
                out[parts[2]] = int(parts[0], 16)
        return out

    def symbol_sizes(self, elf):
        out = {}
        for line in self.run([self.tool("nm"), "-S", elf]).splitlines():
            parts = line.split()
            if len(parts) == 4:
                out[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        return out

    def read_map(self, head_dir):
        with open(os.path.join(head_dir, self.head_map_name), errors="replace") as f:
            return f.read().replace("\\", "/")

    # --- libraries -------------------------------------------------------------------------------------------------

    @staticmethod
    def archives_in(build_dir):
        out = {}
        for root, _, files in os.walk(build_dir):
            if os.path.basename(root) == "src" or "mod-" in root.replace(chr(92), "/"):
                continue
            for name in files:
                if name.endswith(".a") and not name.startswith("module-"):
                    out[name] = os.path.join(root, name)
        return out

    def framework_archives(self):
        """the prebuilt libraries the head links (LIBS of the env, found on LIBPATH)"""
        paths = [self.env.subst(str(p)) for p in self.env.get("LIBPATH", [])]
        out = {}
        for lib in self.env.get("LIBS", []):
            name = self.env.subst(str(lib))
            if name.startswith("-l"):
                name = name[2:]
            if os.path.isabs(name) and name.endswith(".a"):
                out[os.path.basename(name)] = name
                continue
            for folder in paths:
                path = os.path.join(folder, f"lib{name}.a")
                if os.path.isfile(path):
                    out.setdefault(f"lib{name}.a", path)
                    break
        return out

    def archive_members(self, archive):
        """{member: [(type, name)]} of the defined symbols, locals too; cached by size + mtime (ESP32: ~150 framework
        libraries)"""
        cache_path = os.path.join(self.env.subst("$BUILD_DIR"), "mod-libs", "members.json")
        if self._members_cache is None:
            self._members_cache = {}
            if os.path.exists(cache_path):
                with open(cache_path) as f:
                    self._members_cache = json.load(f)
        stat = os.stat(archive)
        key = f"{archive}|{stat.st_size}|{int(stat.st_mtime)}"
        if key in self._members_cache:
            return self._members_cache[key]

        out = {}
        member = None
        for line in self.run([self.tool("nm"), "--defined-only", archive]).splitlines():
            line = line.rstrip()
            if line.endswith(".o:") or line.endswith(".obj:"):
                member = line[:-1]
                out[member] = []
                continue
            parts = line.split()
            if member and len(parts) == 3:
                out[member].append((parts[1], parts[2]))
        self._members_cache[key] = out
        os.makedirs(os.path.dirname(cache_path), exist_ok=True)
        with open(cache_path, "w") as f:
            json.dump(self._members_cache, f)
        return out

    def lib_archives(self, build_dir, head_names):
        """the libraries for a module link, without the members the head has state of.
        Returns (archives, symbols of the members left out)."""
        work = os.path.join(build_dir, "mod-libs")
        os.makedirs(work, exist_ok=True)
        out = []
        head_only = set()
        for name, path in sorted(self.archives_in(build_dir).items()):
            members = self.archive_members(path)
            drop = []
            for member, defined in members.items():
                state = any(kind in "dDbB" for kind, _ in defined)
                # only global names: locals (init, ready, state) of unrelated objects share names with the head's
                # (Beken: members of libMatterApp were dropped and their functions went to the head's exports)
                in_head = any(symbol in head_names for kind, symbol in defined if kind in "TDBR")
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
            # in batches: a framework archive drops hundreds (the Windows command line)
            for start in range(0, len(drop), 50):
                self.run([self.tool("ar"), "d", filtered] + drop[start: start + 50])
            self.run([self.tool("ar"), "s", filtered])
            out.append(filtered)
        return out, head_only

    # --- head exports ----------------------------------------------------------------------------------------------

    def head_defined(self, head_dir, head_only):
        """global symbols of the head's objects (before --gc-sections: its own and its libraries') and of the library
        members it keeps state of"""
        out = set(head_only)
        for root, folders, names in os.walk(head_dir):
            # the module objects and libraries of the same build dir are not the head's
            if root == head_dir:
                folders[:] = [f for f in folders if not f.startswith(("mod-", MODULE_LIBS))]
            for name in names:
                if name.endswith(".o"):
                    proc = subprocess.run([self.tool("nm"), "-g", "--defined-only", os.path.join(root, name)],
                                          capture_output=True, text=True)
                    for line in proc.stdout.splitlines():
                        parts = line.split()
                        if len(parts) == 3 and parts[1] in "TDRBVWtdrb":
                            out.add(parts[2])
        return out

    def update_exports(self, head_dir, head_only, missing):
        path = head_exports_path(self.env)
        defined = self.head_defined(head_dir, head_only)
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

    def head_object(self, head_dir, head_map, head_env, object_name):
        """the head links this object and it defines something (an object of a part that is off is empty)"""
        path = os.path.join(head_dir, "src", object_name)
        if not os.path.exists(path) or f"{head_env}/src/{object_name}" not in head_map:
            return False
        return any(len(line.split()) == 3 and line.split()[1] in "TDBR"
                   for line in self.run([self.tool("nm"), "-g", "--defined-only", path]).splitlines())

    def rename_api(self, work, objects, provides):
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
            plain = self.run([self.tool("nm"), "-g", "--defined-only", copy]).splitlines()
            demangled = self.run([self.tool("nm"), "-g", "-C", "--defined-only", copy]).splitlines()
            for raw, nice in zip(plain, demangled):
                raw_parts = raw.split(None, 2)
                nice_parts = nice.split(None, 2)
                if len(raw_parts) != 3 or raw_parts[1] != "T":
                    continue
                base = nice_parts[2].split("(", 1)[0]
                if base in provides:
                    renames[raw_parts[2]] = RENAMED + raw_parts[2]

        found = {name for name in provides if any(
            self.run([self.tool("c++filt"), raw]).strip().split("(", 1)[0] == name for raw in renames)}
        missing = sorted(set(provides) - found)
        if missing:
            print(f"  provides, not in its objects on this chip (its feature is off here): {' '.join(missing)}")

        table = os.path.join(work, "renames.txt")
        with open(table, "w", newline=chr(10)) as f:
            for old, new in sorted(renames.items()):
                f.write(f"{old} {new}" + chr(10))
        for copy in copies:
            self.run([self.tool("objcopy"), f"--redefine-syms={table}", copy])
        return copies, sorted(renames)

    # --- the link of one module ------------------------------------------------------------------------------------

    @staticmethod
    def missing_symbols(stderr):
        return sorted(set(re.findall(r"undefined reference to `([^']+)'", stderr)))

    @staticmethod
    def pulled_members(mapfile):
        out = []
        with open(mapfile, errors="replace") as f:
            lines = f.read().split("Allocating common symbols")[0].split("Discarded input sections")[0]
        for line in lines.splitlines():
            line = line.strip()
            if line.endswith(")") and "(" in line and ".a(" in line and not line.startswith("("):
                out.append(line.replace("\\", "/").rsplit("/", 1)[-1])
        return sorted(set(out))

    def machine_flags(self):
        """the flags of the head link a module link takes too (head_link_flags)"""
        out = []
        for flag in self.env.get("LINKFLAGS", []):
            flag = self.env.subst(str(flag))
            if flag.startswith(self.head_link_flags):
                out.append(flag)
        return out

    def link_text(self, placed):
        """scripts/<link_script> with module_sections.ld in it and its placeholders replaced"""
        with open(self.script(self.link_script)) as f:
            text = f.read()
        with open(self.script("module_sections.ld")) as f:
            text = text.replace("@SECTIONS@", f.read())
        for placeholder, base in placed:
            text = text.replace(f"@{placeholder}@", f"0x{base:08X}")
        return text

    def link(self, build_dir, name, head_elf, objects, archives, tag, placed):
        """link at the placeholder bases `placed`; returns (elf, map, image)"""
        ld = os.path.join(build_dir, f"module-{name}-{tag}.ld")
        with open(ld, "w") as f:
            f.write(self.link_text(placed))

        elf = os.path.join(build_dir, f"module-{name}-{tag}.elf")
        mapfile = os.path.join(build_dir, f"module-{name}-{tag}.map")
        args = self.machine_flags() + list(LINK_FLAGS) + list(self.link_options) + [
            self.head_symbols(build_dir, head_elf),
            f"-Wl,-T,{ld}",
            f"-Wl,-Map,{mapfile}",
            "-Wl,--cref",
            # missing head symbols mangled, for custom_head_exports
            "-Wl,--no-demangle",
            "-o",
            elf,
        ]
        args += objects
        args += self.library_flags()
        args += ["-Wl,--start-group"] + archives + list(self.system_libs) + ["-Wl,--end-group"]
        # a response file: the command line of a big module (Matter) is too long for Windows. gcc splits it at white
        # space: an argument with a space (a library folder "Adafruit BusIO") goes in double quotes
        def rsp_arg(arg):
            arg = arg.replace(chr(92), "/")
            return '"' + arg.replace('"', chr(92) + '"') + '"' if any(c.isspace() for c in arg) else arg

        rsp = os.path.join(build_dir, f"module-{name}-{tag}.rsp")
        with open(rsp, "w") as f:
            f.write(chr(10).join(rsp_arg(a) for a in args))
        proc = subprocess.run([self.env.subst("$CC"), f"@{rsp}"], capture_output=True, text=True)
        if proc.returncode != 0:
            missing = self.missing_symbols(proc.stderr)
            if missing:
                raise MissingSymbols(missing, proc.stderr)
            print(proc.stdout)
            print(proc.stderr[-6000:])
            raise RuntimeError(f"module {name}: link failed")

        # only our own sections may take memory: anything else (.tbss, .rtc.*, an orphan) has no place in a module
        others = []
        for line in self.run([self.tool("objdump"), "-h", elf]).splitlines():
            parts = line.split()
            if len(parts) >= 7 and parts[0].isdigit():
                section = parts[1]
                size = int(parts[2], 16)
                if size and section not in self.own_sections and section.startswith(".") \
                        and not section.startswith(NO_MEMORY):
                    others.append(f"{section} ({size})")
        if others:
            raise RuntimeError(f"module {name}: sections without a place in a module: {', '.join(others)}")

        binary = os.path.join(build_dir, f"module-{name}-{tag}.bin")
        sections = []
        for section in self.image_sections:
            sections += ["-j", section]
        self.run([self.tool("objcopy"), "-O", "binary"] + sections + [elf, binary])
        with open(binary, "rb") as f:
            data = f.read()
        return elf, mapfile, data + b"\0" * (-len(data) % self.image_align)

    def relocations(self, name, image_a, image_b, branches):
        """the kind of every word: 0 as is, else the base it moves with; counts per kind"""
        deltas = {(b - a) & 0xFFFFFFFF: kind for kind, (_, a, b, _) in enumerate(self.bases, 1)}
        covered = {word for offset in branches for word in range(offset // 4, (offset + 3) // 4 + 1)}
        kinds = []
        counts = [0] * (len(self.bases) + 1)
        for offset in range(0, len(image_a), 4):
            a, = struct.unpack_from("<I", image_a, offset)
            b, = struct.unpack_from("<I", image_b, offset)
            kind = 0
            if a != b and (offset // 4) not in covered:
                kind = deltas.get((b - a) & 0xFFFFFFFF)
                if kind is None and self.odd_word(image_a, image_b, offset, branches):
                    kinds.append(0)
                    continue
                if kind is None:
                    raise RuntimeError(f"module {name}: word at +0x{offset:X} differs by 0x{(b - a) & 0xFFFFFFFF:X}")
            kinds.append(kind)
            counts[kind] += 1
        return kinds, counts

    def package(self, ctx, name, objects, archives):
        """link twice, write module-<name>.module; returns (RAM bytes, IRAM bytes)"""
        build_dir = ctx["build_dir"]
        head_id = ctx["head_id"]
        links = [self.link(build_dir, name, ctx["head_elf"], objects, archives, tag,
                           [(base[0], base[1 + index]) for base in self.bases])
                 for index, tag in enumerate("ab")]
        (elf_a, map_a, image_a), (_, _, image_b) = links
        if len(image_a) != len(image_b) or len(image_a) % 4:
            raise RuntimeError(f"module {name}: images differ in size: {len(image_a)} / {len(image_b)}")

        branches = self.branches(name, elf_a, image_a, image_b)
        linked = len(branches)
        kinds, counts = self.relocations(name, image_a, image_b, branches)

        flash = self.bases[0][1]
        syms = self.symbols(elf_a)
        ram_size = syms["__mod_bss_end"] - syms["__mod_data_start"]
        iram_size = syms["__mod_iram_end"] - syms["__mod_iram_start"] if self.has_iram else 0
        arenas = self.arenas(ctx)
        if syms["__mod_flash_start"] != flash or struct.unpack_from("<I", image_a, 0)[0] != HEADER_MAGIC:
            raise RuntimeError(f"module {name}: no module header at the start (KEEP .module_header)")
        if round_up(syms["__mod_flash_end"] - flash, self.image_align) != len(image_a):
            raise RuntimeError(f"module {name}: image is {len(image_a)} bytes, flash range "
                               f"{syms['__mod_flash_end'] - flash}")
        if self.ram_in_arena and arenas[0][1] and ram_size > arenas[0][1]:
            raise RuntimeError(f"module {name}: RAM {ram_size} bytes, the arena of the head has {arenas[0][1]}")
        self.check(name, syms)

        # the head id and the modules it needs (5th column of custom_modules) into the header, same in every placement
        image = bytearray(image_a)
        image[HEADER_ID: HEADER_ID + 16] = head_id
        image[HEADER_REQUIRES: HEADER_REQUIRES + REQUIRES_SIZE] = self.requires_bytes(name)

        data = package_bytes(self.package_version, name, head_id, bytes(image), kinds, ram_size, iram_size,
                             [base[1] for base in self.bases], self.chunk_extra(branches), len(branches))
        with open(os.path.join(build_dir, f"module-{name}.module"), "wb") as f:
            f.write(data)

        memory = f"RAM {ram_size} (data {syms['__mod_data_end'] - syms['__mod_data_start']}, bss " \
                 f"{syms['__mod_bss_end'] - syms['__mod_bss_start']})"
        if self.has_iram:
            memory += f", IRAM {iram_size}"
        moved = ", ".join(f"{label} {counts[kind]}" for kind, (_, _, _, label) in enumerate(self.bases, 1))
        if branches:
            moved += f", branches into the head {len(branches)} ({len(branches) - linked} in veneers)"
        print(f"Module '{name}': image {len(image)} bytes{self.image_detail(syms)}, {memory}; relocations {moved}; "
              f"package {len(data)}")
        print(f"  {name} pulled: {' '.join(self.pulled_members(map_a))}")
        self.report(name, map_a)

        final_elf = os.path.join(build_dir, f"module-{name}.elf")
        if os.path.exists(final_elf):
            os.remove(final_elf)
        os.replace(elf_a, final_elf)
        os.replace(map_a, os.path.join(build_dir, f"module-{name}.map"))
        return ram_size, iram_size

    # --- custom_modules, the modules file ----------------------------------------------------------------------------

    @staticmethod
    def parse_lines(lines):
        """module lines: <name> | <objects> | <optional objects> | <provides> | <requires>"""
        out = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            parts = [part.strip() for part in line.split("|")] + ["", "", "", ""]
            out.append({
                "name": parts[0],
                "objects": parts[1].split(),
                "optional": parts[2].split(),
                "provides": parts[3].split(),
                "requires": parts[4].split(),
            })
        return out

    def head_list(self):
        """the lines of espurna/config/head.h between the ESPURNA_MODULES markers: every module of every chip"""
        path = os.path.join(self.env.subst("$PROJECT_SRC_DIR"), "config", "head.h")
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        start = text.find("ESPURNA_MODULES:")
        end = text.find("ESPURNA_MODULES */")
        if start < 0 or end < 0:
            raise RuntimeError(f"{path}: no ESPURNA_MODULES list")
        lines = text[start:end].splitlines()[1:]
        # a module line starts with its name (the format lines above the list start with "<" or "|")
        return [line for line in lines if re.match(r"\s*[A-Za-z0-9_]+\s*\|", line)]

    def parse_modules(self):
        """head.h's list, without custom_modules_skip, with custom_modules of the chip's ini: a line of the same name
        adds its objects first (the chip's own: an arch file), the rest of its columns after; a new name is a module of
        this chip alone"""
        skip = set(self.env.GetProjectOption("custom_modules_skip", "").split())
        chip = {module["name"]: module
                for module in self.parse_lines(self.env.GetProjectOption("custom_modules", "").splitlines())}
        out = []
        for module in self.parse_lines(self.head_list()):
            if module["name"] in skip:
                continue
            extra = chip.pop(module["name"], None)
            if extra:
                module["objects"] = extra["objects"] + [o for o in module["objects"] if o not in extra["objects"]]
                for key in ("optional", "provides", "requires"):
                    module[key] = module[key] + [o for o in extra[key] if o not in module[key]]
            out.append(module)
        out += [module for module in chip.values() if module["name"] not in skip]
        return out

    def requires_of(self, name):
        for module in self.parse_modules():
            if module["name"] == name:
                return module["requires"]
        return []

    def requires_bytes(self, name):
        text = ",".join(self.requires_of(name)).encode()
        if len(text) >= REQUIRES_SIZE:
            raise RuntimeError(f"module {name}: requires list longer than {REQUIRES_SIZE - 1} bytes")
        return text.ljust(REQUIRES_SIZE, b"\0")

    def write_container(self, ctx, names):
        """the modules file: every package of this build, for the MODULES panel and the head's page (EMC2)"""
        build_dir = ctx["build_dir"]
        paths = [os.path.join(build_dir, f"module-{name}.module") for name in names]
        paths += self.extra_packages(ctx)

        packages = []
        for path in paths:
            with open(path, "rb") as f:
                packages.append(f.read())

        byte_ram = set(self.option("byte_ram").split())
        offset = 8 + (CONTAINER_ENTRY.size * len(packages))
        table = b""
        for data in packages:
            name, image, ram, iram = struct.unpack_from("<8x16s16xIII", data, 0)
            requires = self.requires_bytes(name.rstrip(b"\0").decode())
            flags = CONTAINER_BYTE_RAM if name.rstrip(b"\0").decode() in byte_ram else 0
            table += CONTAINER_ENTRY.pack(name, offset, len(data), image, ram, iram, requires, flags)
            offset += len(data)

        out = os.path.join(build_dir, self.container_name)
        with open(out, "wb") as f:
            f.write(struct.pack("<II", CONTAINER_MAGIC, len(packages)) + table + b"".join(packages))
        print(f"Modules file: {out}, {offset} bytes, {len(packages)} modules")
        return out

    # --- the module objects, compiled in this env with the module flags ----------------------------------------------

    def module_sources(self):
        """source paths (relative to the src dir) of every object custom_modules names"""
        out = []
        for module in self.parse_modules():
            for object_name in module["objects"] + module["optional"]:
                source = object_name[: -len(".o")] if object_name.endswith(".o") else object_name
                if source not in out:
                    out.append(source)
        src_dir = self.env.subst("$PROJECT_SRC_DIR")
        missing = [source for source in out if not os.path.isfile(os.path.join(src_dir, source))]
        if missing:
            raise RuntimeError(f"custom_modules: no source for {' '.join(missing)}")
        return out

    def module_env(self):
        """projenv with the module macros: custom_module_flags replace the head's macros of the same names,
        custom_module_undef drops head macros"""
        menv = self.projenv.Clone()
        parsed = menv.ParseFlagsExtended(self.env.GetProjectOption("custom_module_flags", ""))
        defines = parsed.pop("CPPDEFINES", [])
        drop = {define_name(item) for item in defines}
        drop.update(self.env.GetProjectOption("custom_module_undef", "").split())
        menv.Replace(CPPDEFINES=[item for item in menv.get("CPPDEFINES", []) if define_name(item) not in drop])
        menv.Append(CPPDEFINES=defines)
        rest = {key: value for key, value in parsed.items() if value}
        if rest:
            menv.Append(**rest)
        return menv

    def library_dir(self, name):
        """the project's lib/ first (its own copies: DNSServer, which the frameworks also have), <libdeps>/<name>, the
        lib_extra_dirs, the framework's libraries"""
        env = self.env
        folders = [env.subst("$PROJECT_LIB_DIR") or os.path.join(env.subst("$PROJECT_DIR"), "lib")]
        folders += [os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))]
        folders += [os.path.join(env.subst("$PROJECT_DIR"), env.subst(str(p)))
                    for p in env.GetProjectOption("lib_extra_dirs", [])]
        platform = env.PioPlatform()
        for package in ("framework-arduinoespressif32", "framework-arduinoespressif8266"):
            folder = platform.get_package_dir(package) if package in platform.packages else None
            if folder:
                folders.append(os.path.join(folder, "libraries"))
        for folder in folders:
            path = os.path.join(folder, name)
            if os.path.isdir(path):
                print(f"  module library {name}: {path}")
                return path
        raise RuntimeError(f"custom_module_libs: library {name} not found in {', '.join(folders)}")

    def compile_modules(self):
        """the module objects in $BUILD_DIR/mod-src, the module libraries in $BUILD_DIR/libmod"""
        menv = self.module_env()
        libraries = []
        for name in self.env.GetProjectOption("custom_module_libs", "").split():
            folder = self.library_dir(name)
            src = os.path.join(folder, "src") if os.path.isdir(os.path.join(folder, "src")) else folder
            menv.Prepend(CPPPATH=[src])
            libraries.append((name, src))

        out = []
        for name, src in libraries:
            out += menv.Flatten([menv.BuildLibrary(os.path.join("$BUILD_DIR", MODULE_LIBS, name), src)])

        src_filter = "-<*> " + " ".join(f"+<{source}>" for source in self.module_sources())
        for node in menv.CollectBuildFiles(os.path.join("$BUILD_DIR", MODULE_SRC), "$PROJECT_SRC_DIR", src_filter):
            out += menv.Flatten([menv.Object(node)])
        return out

    # --- the head link again, when the modules added head exports ---------------------------------------------------

    def keep_head_outputs(self):
        """the build actions of the head's outputs, kept: SCons drops them once a node is built"""
        for name in self.head_outputs:
            node = self.env.File(self.env.subst(os.path.join("$BUILD_DIR", name)))
            node.attributes.keep_targetinfo = 1
            self._executors.append((node, node.get_executor()))

    def relink_head(self):
        _, count = write_head_exports(self.env)
        print(f"Head exports: {count}, linking the head again")
        for node, executor in self._executors:
            print(f"  {node.get_path()}")
            executor(node)

    # --- the whole run, after the head's outputs --------------------------------------------------------------------

    def context(self):
        build_dir = self.env.subst("$BUILD_DIR")
        head_elf = os.path.join(build_dir, self.head_elf_name)
        if not os.path.exists(head_elf):
            raise RuntimeError(f"{head_elf}: no head")
        ctx = {
            "build_dir": build_dir,
            "head_env": self.env.subst("$PIOENV"),
            "head_dir": build_dir,
            "head_elf": head_elf,
            "head_bin": os.path.join(build_dir, self.head_bin_name),
        }
        ctx["head_id"] = self.head_id(ctx)
        ctx["head_syms"] = self.symbol_sizes(head_elf)
        return ctx

    def link_all(self, ctx):
        """every module of custom_modules; returns ({module: MissingSymbols}, totals, head_only)"""
        build_dir = ctx["build_dir"]
        head_dir = ctx["head_dir"]
        head_env = ctx["head_env"]
        head_map = self.read_map(head_dir)
        print(f"Head {head_env}: id {ctx['head_id'].hex()}")

        src = os.path.join(build_dir, MODULE_SRC)
        libraries, head_only = self.lib_archives(build_dir, set(self.symbols(ctx["head_elf"])))
        missing = {}
        totals = [0, 0]
        for module in self.parse_modules():
            name = module["name"]
            objects = [os.path.join(src, o) for o in module["objects"]]
            optional = []
            for object_name in module["optional"]:
                if self.head_object(head_dir, head_map, head_env, object_name):
                    continue
                if os.path.exists(os.path.join(src, object_name)):
                    optional.append(os.path.join(src, object_name))

            work = os.path.join(build_dir, f"mod-{name}")
            renamed, api = self.rename_api(work, objects + optional, set(module["provides"]))
            objects = renamed[: len(objects)]
            archives = []
            if optional:
                archive = os.path.join(build_dir, f"module-{name}-optional.a")
                if os.path.exists(archive):
                    os.remove(archive)
                self.run([self.tool("ar"), "rcs", archive] + renamed[len(objects):])
                archives.append(archive)
            archives += libraries
            if api:
                print(f"  {name} provides {len(api)} functions through the head")

            try:
                first, second = self.package(ctx, name, objects, archives)
                totals[0] += first
                totals[1] += second
            except MissingSymbols as e:
                missing[name] = e
        return missing, totals, head_only

    def modules_link(self, target, source, env):
        for attempt in range(RELINKS + 1):
            ctx = self.context()
            missing, totals, head_only = self.link_all(ctx)
            if not missing:
                break

            exported_all = []
            absent_all = []
            for name, e in missing.items():
                exported, absent = self.update_exports(ctx["head_dir"], head_only, e.names)
                exported_all += exported
                for symbol in exported:
                    print(f"  {name}: head export added: {symbol}")
                for symbol in absent:
                    print(f"  {name}: NOT IN THE HEAD: {symbol}")
                absent_all += absent
            if absent_all or not exported_all or attempt == RELINKS:
                raise RuntimeError(f"{len(set(exported_all))} head exports added, "
                                   f"{len(set(absent_all))} symbols the head does not have")
            self.relink_head()

        print("All modules: " + ", ".join(f"{label} {total}" + (f" of {size}" if size else "")
                                          for (label, size), total in zip(self.arenas(ctx), totals)))
        container = self.write_container(ctx, [module["name"] for module in self.parse_modules()])
        self.after(ctx, container)
        heads = self.ota_heads(ctx)
        if heads:
            image = self.env.subst("$PIOENV")
            module_ota.build(os.path.join(ctx["build_dir"], f"espurna-{image}.ota"), heads, container)

    def install(self):
        """the module objects and the modules link: a default target of the env, after the head's outputs"""
        from SCons.Script import Default  # pylint: disable=import-error

        env = self.env
        objects = self.compile_modules()
        self.keep_head_outputs()
        sources = [os.path.join("$BUILD_DIR", name) for name in self.head_outputs] + objects + self.extra_sources()
        node = env.Command(os.path.join("$BUILD_DIR", self.container_name), sources,
                           env.VerboseAction(self.modules_link, "Linking the modules against the head"))
        Default(node)
        return node
