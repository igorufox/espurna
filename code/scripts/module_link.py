# PROTOTYPE (docs/modules.md): linking modules against a head image and packaging them, every chip.
#
# One env per chip builds everything (platformio_<chip>.ini, env:<chip>): the head is the env's own image, the module
# objects are compiled in the same build with the module flags, then every module is linked against the head and the
# release images are written. A chip script (the last extra script of the env: esp8266_module_link.py, ...) subclasses
# Linker below with what only that chip has (its toolchain, the bases of its linker script, its relocation records,
# its head identity, where its module area starts in the initial image: module_initial_image.py), then calls install().
#
#   custom_module_flags = -D...      macros of the module objects: replace the head's macros of the same names
#   custom_module_undef = NAME ...   macros of the head the module objects do not get (MODULES_SUPPORT ...)
#   custom_module_libs = NAME ...    libraries only the modules use, compiled with the module flags (the head's
#                                    lib_ignore keeps them out of its LDF): <libdeps>/<NAME> or the framework's
#   custom_module_lib_filter =       <NAME> <source filter>: the part of a library the modules build
#   custom_modules =
#       <name> | <objects always linked> | <objects linked only for what the head does not have> | <API it provides>
#           | <modules it needs>
#
# The module objects go to $BUILD_DIR/mod-src (the head's are in $BUILD_DIR/src), the module libraries to
# $BUILD_DIR/libmod. Each module is linked against the head's ELF: its calls go to the head, what it needs that the
# head dropped (--gc-sections) is appended to custom_head_exports (scripts/module_head.py keeps it in the head) and the
# head is linked again in the same build (up to RELINKS times), then the modules.
#
# The modules a module needs (5th column): it is linked against them too, directly, as against the head. Each of them is
# linked before it (the modules in the order of their needs), its global symbols (without the head's) are the
# symbols of this link (provider_symbols()); its library members with state are left out of this module's libraries,
# as the head's are. A symbol it needs that the provider dropped (--gc-sections) goes to custom_module_provided
# (espurna/modules_provided.txt: <provider> <symbol> <chip>), the provider keeps it (-Wl,--undefined) and is linked
# again with everything after it. Where the words of this module hold an address of a provider: a third link with
# that provider at its link B bases, the words that move with it are the links of the package (below), the board adds
# where the Apply put that provider (or where it moved to at a boot).
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
# The package, one format on every chip (PACKAGE_VERSION, modules_core.h PackageVersion): PACKAGE, then per CHUNK of
# the image the map of 2 bits per word (0 as is, 1 flash data view, 2 RAM, 3 code: ESP8266 IRAM code, ESP32 the
# instruction view), a u32 branch count and that many u32 branches (offset in the chunk | kind << 16: Beken's BL / BLX
# into the head, kind 1 / 2; the links to the modules it needs, every chip: LINK_WORD | provider << 2 | base kind, a
# word plus where that provider's base is, LINK_BRANCH | provider << 2 | branch kind, Beken's BL / BLX into it;
# provider: its index in the needs of the module), then the chunk. The last word of PACKAGE: how many links, the
# board keeps them behind the maps of the slot (a boot relocates the module again when a provider moved). The module header (modules.h) in the image gets the head id
# (offset 24) and the modules it needs (HEADER_REQUIRES).
#
# The outputs of every chip: module-<name>.module / .elf / .map, the modules file espurna-<chip>-modules.bin (EMC2,
# every package, webui among them), the initial image (module_initial_image.py: the head + the modules on by default
# at the start of the module area) and the install image espurna-<chip>.ota (module_ota.py: the head(s) + the modules
# file). The board installs packages only by the Apply (/modules/apply).

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import threading
import zlib
from concurrent.futures import ThreadPoolExecutor

# loaded with this file: the folder of the scripts is on sys.path only while the env's scripts run
import module_ota  # noqa: E402  pylint: disable=wrong-import-position
import release_image  # noqa: E402  pylint: disable=wrong-import-position

HEADER_MAGIC = 0x31444D45   # "EMD1"
PACKAGE_MAGIC = 0x31504D45  # "EMP1"
PACKAGE_VERSION = 5         # modules_core.h PackageVersion
# magic, version, name, head id, image size, RAM, IRAM, the link A bases of kind 1, 2, 3, crc32 of the body, links
PACKAGE = struct.Struct("<II16s16sIIIIIIII")
PACKAGE_LINKS = 68
# the links to the modules a module needs, in the branch lists (modules_core.h LinkWord, LinkBranch)
LINK_WORD = 0x10
LINK_BRANCH = 0x20
LINK_PROVIDERS = 4
# the symbols a provider module keeps for the modules that need it (custom_module_provided)
PROVIDED_FILE = "espurna/modules_provided.txt"
# a provider's own symbols that are no symbols of the modules that need it
PROVIDER_PRIVATE = ("__mod_", "espurna_module_", "_GLOBAL_", "_etext", "_edata", "_end")
CHUNK = 4096
MAP_SIZE = CHUNK // 16
RENAMED = "espmod_"

# the modules file: name, offset, package bytes, image bytes, RAM, IRAM code, the modules it needs, flags (EMC2)
CONTAINER_MAGIC = 0x32434D45  # "EMC2"
# packages in one modules file: modules.cpp ModulesFileMax, the head refuses a file with more
CONTAINER_MAX = 128
CONTAINER_ENTRY = struct.Struct("<16sIIIII32sI")
# flags of a container entry: its RAM takes byte access from interrupts (custom_module_byte_ram), not into a pool that
# takes 32-bit access only (ESP8266: the IRAM tail); the upper 16 bits: its links to the modules it needs (the slot
# keeps them, the Apply plans the flash with them)
CONTAINER_BYTE_RAM = 1
# bits 8..15: the sectors of flash it keeps its data in (the data column of ESPURNA_MODULES, modulesData())
CONTAINER_DATA_SHIFT = 8
CONTAINER_LINKS_SHIFT = 16

# the module header (modules.h): the head id at 24, `needs` after the 96 bytes of the first version
HEADER = struct.Struct("<II16s16s14I32s")
HEADER_ID = 24
HEADER_REQUIRES = 96
REQUIRES_SIZE = 32

# the modules a data module needs (it has no line in ESPURNA_MODULES): the WebUI talks to the board over the
# WebSocket of the ws module
DATA_MODULE_REQUIRES = {"webui": ["ws"]}

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


def pack_body(image, kinds, branches):
    """per CHUNK: the map (2 bits per word), the branch count and the branches of the chunk (offset in the chunk |
    kind << 16, from `branches` {offset in the image: kind}), the bytes"""
    body = bytearray()
    for start in range(0, len(image), CHUNK):
        chunk = image[start: start + CHUNK]
        bitmap = bytearray(MAP_SIZE)
        for index, kind in enumerate(kinds[start // 4: start // 4 + CHUNK // 4]):
            bitmap[index >> 2] |= kind << ((index & 3) * 2)
        body += bitmap
        inside = sorted(offset for offset in branches if start <= offset < start + len(chunk))
        body += struct.pack("<I", len(inside))
        for offset in inside:
            body += struct.pack("<I", (offset - start) | (branches[offset] << 16))
        body += chunk
    return bytes(body)


def package_bytes(name, head_id, image, kinds, ram, iram, bases, branches=None):
    """PACKAGE + body; bases: the link A bases of kind 1, 2, 3 (0 for what the chip has not); branches: {offset in the
    image: kind} of the words the board moves other than by the map (Beken), none elsewhere"""
    branches = branches or {}
    body = pack_body(image, kinds, branches)
    bases = list(bases) + [0] * (3 - len(bases))
    links = sum(1 for kind in branches.values() if kind >= LINK_WORD)
    header = PACKAGE.pack(PACKAGE_MAGIC, PACKAGE_VERSION, module_name(name), head_id, len(image), ram, iram, *bases,
                          zlib.crc32(body) & 0xFFFFFFFF, links)
    return header + body


def data_package(name, data, head_id, bases, align=4, requires=b""):
    """a data module (no code, entry 0, e.g. the WebUI page): the header + the data, padded to the chip's image
    alignment; only flash_start / flash_end hold addresses, they are its two flash relocations (words 10 and 11, kind
    1). bases: the chip's link A bases (the first one, flash, is the data module's link address)"""
    link_flash = bases[0]
    size = round_up(HEADER.size + len(data), align)
    fields = [0] * 14  # the last one: priority 0
    fields[0] = link_flash         # flash_start
    fields[1] = link_flash + size  # flash_end
    fields[4] = len(data)          # data_end: the data size
    image = HEADER.pack(HEADER_MAGIC, 1, module_name(name), head_id, *fields, requires) + data
    image += b"\0" * (size - len(image))
    kinds = [0] * (size // 4)
    kinds[10] = kinds[11] = 1
    out = package_bytes(name, head_id, image, kinds, 0, 0, bases)
    print(f"data module '{name}': {len(data)} bytes of data, image {len(image)}, package {len(out)}, "
          f"head md5 {head_id.hex()}")
    return out


def head_exports_path(env):
    path = env.GetProjectOption("custom_head_exports", "")
    return os.path.join(env.subst("$PROJECT_DIR"), path) if path else ""


def export_chip(env):
    """the chip key of the export list: esp8266, esp32, beken"""
    # the platform option of the env: a name or a URL (pioarduino), with a version
    platform = (env.GetProjectOption("platform", "") or env.subst("$PIOPLATFORM")).lower()
    for key, chip in (("espressif8266", "esp8266"), ("espressif32", "esp32"), ("libretiny", "beken")):
        if key in platform:
            return chip
    raise RuntimeError(f"head exports: unknown platform {platform!r}")


def read_export_lines(path):
    """(name, chips) per line of the export list; chips empty - every chip"""
    if not path or not os.path.exists(path):
        return []
    out = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if parts and not parts[0].startswith("#"):
                out.append((parts[0], set(parts[1:])))
    return out


def read_head_exports(env):
    chip = export_chip(env)
    seen = set()
    out = []
    for name, chips in read_export_lines(head_exports_path(env)):
        if (not chips or chip in chips) and name not in seen:
            seen.add(name)
            out.append(name)
    return out


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


def provided_path(env):
    return os.path.join(env.subst("$PROJECT_DIR"), PROVIDED_FILE)


def read_provided(env):
    """{provider: [symbol]} of this chip"""
    out = {}
    path = provided_path(env)
    if not os.path.exists(path):
        return out
    chip = export_chip(env)
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and not line.startswith("#") and (len(parts) == 2 or chip in parts[2:]):
                out.setdefault(parts[0], []).append(parts[1])
    return out


def add_provided(env, provider, symbols):
    path = provided_path(env)
    known = set(read_provided(env).get(provider, []))
    chip = export_chip(env)
    new = [symbol for symbol in symbols if symbol not in known]
    if new:
        with open(path, "a", newline=chr(10)) as f:
            for symbol in new:
                f.write(f"{provider} {symbol} {chip}" + chr(10))
    return new


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
    # the module has IRAM code of its own (ESP8266: copied into the fixed pool at its start), its size in the package
    has_iram = False

    def __init__(self, env, projenv=None):
        self.env = env
        # the sources of the project are built with projenv (build_src_flags, library include paths)
        self.projenv = projenv if projenv is not None else env
        self._executors = []
        self._members_cache = None
        # the modules of one wave link in parallel (link_all): the caches and the shared files behind locks
        self._members_lock = threading.Lock()
        self._libs_lock = threading.Lock()
        self._libs = {}
        assert len({b - a for _, a, b, _ in self.bases}) == len(self.bases), "every base needs its own delta"

    # --- chip hooks ------------------------------------------------------------------------------------------------

    def head_id(self, ctx):
        """the id the modules carry of the head they are linked against: the first 16 bytes of the GNU build id the
        head keeps in its flash at __build_id_start (scripts/module_head.py), every chip"""
        elf = ctx["head_elf"]
        at = self.symbols(elf)["__build_id_start"]
        dump = self.run([self.tool("objdump"), "-s", f"--start-address=0x{at:x}", f"--stop-address=0x{at + 36:x}", elf])
        data = b""
        for line in dump.splitlines():
            match = re.match(r"^\s*([0-9a-f]{4,8})\s+((?:[0-9a-f]{2,8}\s){1,4})", line)
            if match and int(match.group(1), 16) >= at:
                data += bytes.fromhex("".join(match.group(2).split()))
        namesz, descsz, kind = struct.unpack_from("<III", data, 0)
        if namesz != 4 or kind != 3 or data[12:16] != b"GNU\0" or descsz < 16:
            raise RuntimeError("no GNU build id in the head (scripts/module_head.py)")
        return data[16:32]

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

    def provider_branches(self, name, elf, image_a, image_p):
        """{offset: branch kind} of the branches into a provider (Beken: BL / BLX whose bytes changed when only the
        provider moved), none on the other chips"""
        return {}

    def provider_odd_word(self, image_a, image_p, offset):
        """the branch kind of a word that moved with a provider but by no base delta (Beken: a veneer), else None"""
        return None

    def symbols_arg(self, build_dir, elf, names, out):
        """the link argument that gives a module these global symbols of `elf` with their addresses and types:
        a copy of the ELF with only them (-Wl,--just-symbols), as the head's on most chips"""
        keep = out + ".keep"
        with open(keep, "w", newline=chr(10)) as f:
            f.write(chr(10).join(sorted(names)) + chr(10))
        path = out + ".elf"
        self.run([self.tool("objcopy"), "--extract-symbol", "--strip-all", f"--keep-symbols={keep}", elf, path])
        return f"-Wl,--just-symbols={path}"

    def check(self, name, syms):
        """more checks of the linked module (its symbols)"""

    def image_detail(self, syms):
        """the image line of the summary, after its size"""
        return ""

    def report(self, name, mapfile):
        """warnings about the linked module"""

    def webui_page(self):
        """the WebUI of the webui data module, every chip (decision 11-11): the one WebUI build (gulp build),
        html/build/index.html.gz"""
        page = os.path.join(self.env.subst("$PROJECT_DIR"), "html", "build", "index.html.gz")
        return page if os.path.exists(page) else None

    def extra_packages(self, ctx):
        """the webui data module"""
        page = self.webui_page()
        if not page:
            # never a release without the WebUI (W10: the CI build job had no WebUI build, the module went missing
            # without a word)
            raise RuntimeError("webui: html/build/index.html.gz is missing, run `gulp build` in code/ first")
        with open(page, "rb") as f:
            data = data_package("webui", f.read(), ctx["head_id"], [base[1] for base in self.bases], self.image_align,
                                self.requires_bytes("webui"))
        out = os.path.join(ctx["build_dir"], "module-webui.module")
        with open(out, "wb") as f:
            f.write(data)
        print(f"  webui from {os.path.basename(page)}")
        return [out]

    def extra_sources(self):
        """a new WebUI build (gulp build) has to reach the modules file too"""
        page = self.webui_page()
        return [page] if page else []

    def initial(self, ctx, container, on):
        """the initial image(s) of the chip: the head + the modules file of `on` at the start of the module area
        (module_initial_image.py); returns the paths written"""
        raise NotImplementedError

    def ota_heads(self, ctx):
        """[(name, path)] of the heads in the install image espurna-<chip>.ota (scripts/module_ota.py)"""
        return []

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
        """the prebuilt libraries the head links (LIBS of the env, found on LIBPATH), but those of
        custom_module_lib_exclude (ESP32: espressif__esp_matter, the connectedhomeip of esp_matter, another build of
        the stack than the one of the matter module's libMatterApp.a)"""
        paths = [self.env.subst(str(p)) for p in self.env.get("LIBPATH", [])]
        exclude = set(self.env.GetProjectOption("custom_module_lib_exclude", "").split())
        out = {}
        for lib in self.env.get("LIBS", []):
            name = self.env.subst(str(lib))
            if name.startswith("-l"):
                name = name[2:]
            if name in exclude:
                continue
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
        with self._members_lock:
            return self._archive_members(archive)

    def _archive_members(self, archive):
        cache_path = os.path.join(self.env.subst("$BUILD_DIR"), "mod-libs", "members.json")
        if self._members_cache is None:
            self._members_cache = {}
            if os.path.exists(cache_path):
                # a cache cut by a stopped build is only a cache: read anew
                try:
                    with open(cache_path) as f:
                        self._members_cache = json.load(f)
                except (OSError, ValueError):
                    self._members_cache = {}
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
        # whole or not at all: a stopped build leaves the old file, never a cut one
        part = cache_path + ".part"
        with open(part, "w") as f:
            json.dump(self._members_cache, f)
        os.replace(part, cache_path)
        return out

    def lib_archives(self, build_dir, head_names, work_name="mod-libs"):
        """the libraries for a module link, without the members the head (and the providers it is linked against)
        have state of. Returns (archives, symbols of the members left out)."""
        work = os.path.join(build_dir, work_name)
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
            known = set(read_head_exports(self.env))
            chip = export_chip(self.env)
            with open(path, "a", newline=chr(10)) as f:
                for name in exported:
                    if name not in known:
                        f.write(f"{name} {chip}" + chr(10))
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

    def link(self, build_dir, name, head_elf, objects, archives, tag, placed, providers=(), keep=()):
        """link at the placeholder bases `placed`, with the symbols of its providers (link arguments) and the symbols
        it keeps for the modules that need it; returns (elf, map, image)"""
        ld = os.path.join(build_dir, f"module-{name}-{tag}.ld")
        with open(ld, "w") as f:
            f.write(self.link_text(placed))

        elf = os.path.join(build_dir, f"module-{name}-{tag}.elf")
        mapfile = os.path.join(build_dir, f"module-{name}-{tag}.map")
        args = self.machine_flags() + list(LINK_FLAGS) + list(self.link_options) + [
            self.head_symbols(build_dir, head_elf),
        ] + list(providers) + [f"-Wl,--undefined={symbol}" for symbol in keep] + [
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

    def provider_symbols(self, ctx, name, provider, tag):
        """the link argument with the global symbols of a provider module linked at its bases A or B, without the
        head's and its own private ones; written into the folder of the module `name` (the modules that need the same
        provider link in parallel)"""
        build_dir = ctx["build_dir"]
        elf = os.path.join(build_dir, f"module-{provider}.elf" if tag == "a" else f"module-{provider}-b.elf")
        head = ctx["head_syms"]
        names = []
        for line in self.run([self.tool("nm"), "-g", "--defined-only", elf]).splitlines():
            parts = line.split()
            if len(parts) != 3 or parts[1] not in "TDBRVWAGSuiN":
                continue
            symbol = parts[2]
            if symbol in head or symbol.startswith(PROVIDER_PRIVATE):
                continue
            names.append(symbol)
        return self.symbols_arg(build_dir, elf, names, os.path.join(build_dir, f"mod-{name}", f"provided-{provider}-{tag}"))

    def provider_links(self, ctx, name, objects, archives, providers, extra_a, image_a):
        """{offset: kind} of the words (and Beken's branches) that hold an address of a provider: linked once more
        per provider with that one at its B bases"""
        build_dir = ctx["build_dir"]
        deltas = {(b - a) & 0xFFFFFFFF: kind for kind, (_, a, b, _) in enumerate(self.bases, 1)}
        out = {}
        for position, (index, provider) in enumerate(providers):
            extra = list(extra_a)
            extra[position] = self.provider_symbols(ctx, name, provider, "b")
            elf_p, map_p, image_p = self.link(build_dir, name, ctx["head_elf"], objects, archives, f"p{index}",
                                              [(base[0], base[1]) for base in self.bases], extra)
            if len(image_p) != len(image_a):
                raise RuntimeError(f"module {name}: image changes size with {provider}")
            branches = self.provider_branches(name, elf_p, image_a, image_p)
            for offset, kind in branches.items():
                out[offset] = LINK_BRANCH | (index << 2) | kind
            covered = {word for offset in branches for word in range(offset // 4, (offset + 3) // 4 + 1)}
            for offset in range(0, len(image_a), 4):
                a, = struct.unpack_from("<I", image_a, offset)
                p, = struct.unpack_from("<I", image_p, offset)
                if a == p or (offset // 4) in covered:
                    continue
                kind = deltas.get((p - a) & 0xFFFFFFFF)
                if kind is not None:
                    out[offset] = LINK_WORD | (index << 2) | kind
                    continue
                branch = self.provider_odd_word(image_a, image_p, offset)
                if branch is None:
                    raise RuntimeError(f"module {name}: word at +0x{offset:X} moves with {provider} by "
                                       f"0x{(p - a) & 0xFFFFFFFF:X}")
                out[offset] = LINK_BRANCH | (index << 2) | branch
            for path in (elf_p, map_p):
                if os.path.exists(path):
                    os.remove(path)
        return out

    def package(self, ctx, name, objects, archives, providers=(), keep=()):
        """link twice (and once more per provider), write module-<name>.module; returns (RAM bytes, IRAM bytes).
        providers: (index in its needs, name) of the modules it is linked against; keep: the symbols the modules that
        need it want of it"""
        build_dir = ctx["build_dir"]
        head_id = ctx["head_id"]
        extra_a = [self.provider_symbols(ctx, name, provider, "a") for _, provider in providers]
        links = [self.link(build_dir, name, ctx["head_elf"], objects, archives, tag,
                           [(base[0], base[1 + index]) for base in self.bases], extra_a, keep)
                 for index, tag in enumerate("ab")]
        (elf_a, map_a, image_a), (_, _, image_b) = links
        if len(image_a) != len(image_b) or len(image_a) % 4:
            raise RuntimeError(f"module {name}: images differ in size: {len(image_a)} / {len(image_b)}")

        branches = self.branches(name, elf_a, image_a, image_b)
        linked = len(branches)
        kinds, counts = self.relocations(name, image_a, image_b, branches)
        provided = self.provider_links(ctx, name, objects, archives, providers, extra_a, image_a) if providers else {}
        # a branch into a provider is one into the head too for the links A and B (the provider stays where it is in
        # both): the provider's link moves it by both deltas, the head's entry goes
        for offset in provided:
            if offset in branches:
                del branches[offset]
                linked -= 1
        overlap = [offset for offset in provided
                   if kinds[offset // 4] or any(abs(offset - other) < 4 for other in branches)]
        if overlap:
            raise RuntimeError(f"module {name}: a word moves with the module and a provider: +0x{overlap[0]:X}")
        branches.update(provided)

        flash = self.bases[0][1]
        syms = self.symbols(elf_a)
        ram_size = syms["__mod_bss_end"] - syms["__mod_data_start"]
        iram_size = syms["__mod_iram_end"] - syms["__mod_iram_start"] if self.has_iram else 0
        if syms["__mod_flash_start"] != flash or struct.unpack_from("<I", image_a, 0)[0] != HEADER_MAGIC:
            raise RuntimeError(f"module {name}: no module header at the start (KEEP .module_header)")
        if round_up(syms["__mod_flash_end"] - flash, self.image_align) != len(image_a):
            raise RuntimeError(f"module {name}: image is {len(image_a)} bytes, flash range "
                               f"{syms['__mod_flash_end'] - flash}")
        self.check(name, syms)

        # the head id and the modules it needs (5th column of custom_modules) into the header, same in every placement
        image = bytearray(image_a)
        image[HEADER_ID: HEADER_ID + 16] = head_id
        image[HEADER_REQUIRES: HEADER_REQUIRES + REQUIRES_SIZE] = self.requires_bytes(name)

        data = package_bytes(name, head_id, bytes(image), kinds, ram_size, iram_size, [base[1] for base in self.bases],
                             branches)
        with open(os.path.join(build_dir, f"module-{name}.module"), "wb") as f:
            f.write(data)

        memory = f"RAM {ram_size} (data {syms['__mod_data_end'] - syms['__mod_data_start']}, bss " \
                 f"{syms['__mod_bss_end'] - syms['__mod_bss_start']})"
        if self.has_iram:
            memory += f", IRAM {iram_size}"
        moved = ", ".join(f"{label} {counts[kind]}" for kind, (_, _, _, label) in enumerate(self.bases, 1))
        head_branches = len(branches) - len(provided)
        if head_branches:
            moved += f", branches into the head {head_branches} ({head_branches - linked} in veneers)"
        if provided:
            moved += f", links to {' '.join(p for _, p in providers)} {len(provided)}"
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
        """module lines: <name> | <objects> | <optional objects> | <provides> | <requires> | <data bytes>"""
        out = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            parts = [part.strip() for part in line.split("|")] + ["", "", "", "", ""]
            out.append({
                "name": parts[0],
                "objects": parts[1].split(),
                "optional": parts[2].split(),
                "provides": parts[3].split(),
                "requires": parts[4].split(),
                # bytes of flash it keeps its data in (modulesData(), placed by the Apply), whole sectors
                "data": int(parts[5]) if parts[5] else 0,
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
                module["data"] = module["data"] or extra["data"]
            out.append(module)
        out += [module for module in chip.values() if module["name"] not in skip]
        return out

    def ordered_modules(self):
        """parse_modules() with every module after the modules of this build it needs: they are linked first, the
        modules file has them first (the Apply writes them in its order)"""
        modules = self.parse_modules()
        by_name = {module["name"]: module for module in modules}
        out = []
        state = {}

        def visit(module, path):
            name = module["name"]
            if state.get(name) == "done":
                return
            if state.get(name) == "busy":
                raise RuntimeError(f"modules: circular needs {' -> '.join(path + [name])}")
            state[name] = "busy"
            for need in module["requires"]:
                if need in by_name:
                    visit(by_name[need], path + [name])
            state[name] = "done"
            out.append(module)

        for module in modules:
            visit(module, [])
        return out

    def data_sectors(self, name):
        """sectors of flash the module keeps its data in (the data column), at most 255"""
        for module in self.parse_modules():
            if module["name"] == name and module["data"]:
                sectors = (module["data"] + 4095) // 4096
                if sectors > 255:
                    raise RuntimeError(f"module {name}: more than 255 sectors of data")
                return sectors
        return 0

    def requires_of(self, name):
        for module in self.parse_modules():
            if module["name"] == name:
                return module["requires"]
        return DATA_MODULE_REQUIRES.get(name, [])

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

        if len(packages) > CONTAINER_MAX:
            raise RuntimeError(f"{len(packages)} packages, the head takes at most {CONTAINER_MAX} in one modules file "
                               f"(modules.cpp ModulesFileMax)")

        byte_ram = set(self.option("byte_ram").split())
        offset = 8 + (CONTAINER_ENTRY.size * len(packages))
        table = b""
        for data in packages:
            name, image, ram, iram = struct.unpack_from("<8x16s16xIII", data, 0)
            requires = self.requires_bytes(name.rstrip(b"\0").decode())
            flags = CONTAINER_BYTE_RAM if name.rstrip(b"\0").decode() in byte_ram else 0
            links, = struct.unpack_from("<I", data, PACKAGE_LINKS)
            flags |= links << CONTAINER_LINKS_SHIFT
            flags |= self.data_sectors(name.rstrip(b"\0").decode()) << CONTAINER_DATA_SHIFT
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
        # what a pre: script of the chip gives the modules alone (scripts/matter_lib.py: the include folders and
        # defines of the Matter library's API for matter.cpp, matter_app.cpp, matter_identity.cpp), not the head. First
        # on the include path: the ESP32 framework has the headers of esp_matter's own connectedhomeip build too
        menv.Prepend(CPPPATH=list(self.env.get("ESPURNA_MODULE_CPPPATH", [])))
        menv.Append(CPPDEFINES=list(self.env.get("ESPURNA_MODULE_CPPDEFINES", [])))
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
            if not os.path.isdir(path) and os.path.isdir(folder):
                # the names of the ini are the lib_ignore ones (hlw8012), the folders the libraries' own (HLW8012): a
                # case-sensitive file system (CI) finds them by the name in any case. A folder with spaces ("SparkFun
                # VEML6075 Arduino Library") has '_' for them in the ini, the list is split at the spaces
                def same(entry):
                    return entry.lower().replace(" ", "_") == name.lower().replace(" ", "_")

                path = next((os.path.join(folder, entry) for entry in sorted(os.listdir(folder))
                             if same(entry) and os.path.isdir(os.path.join(folder, entry))), path)
            if os.path.isdir(path):
                print(f"  module library {name}: {path}")
                return path
        raise RuntimeError(f"custom_module_libs: library {name} not found in {', '.join(folders)}")

    def precompiled(self, name, src):
        """a library that comes with its code prebuilt (library.properties: precompiled=true, the BSEC of the bme680
        driver): its archive of this chip, <src>/<chip>/lib*.a, goes next to the libraries of the modules (every module
        link takes the archives of the build dir)"""
        properties = os.path.join(os.path.dirname(src) if os.path.basename(src) == "src" else src, "library.properties")
        if not os.path.isfile(properties):
            return
        with open(properties, "r", encoding="utf-8", errors="replace") as f:
            if "precompiled=true" not in f.read().replace(" ", ""):
                return
        chip = export_chip(self.env)
        folder = os.path.join(src, chip)
        archives = [entry for entry in os.listdir(folder) if entry.endswith(".a")] if os.path.isdir(folder) else []
        if not archives:
            raise RuntimeError(f"custom_module_libs: {name} has no prebuilt archive for {chip} ({folder})")
        # in the build dir itself: every chip's archives_in() takes the archives there (ESP32 packs the library
        # folders' objects and skips the archives inside them)
        out = self.env.subst("$BUILD_DIR")
        for entry in archives:
            shutil.copyfile(os.path.join(folder, entry), os.path.join(out, f"libmodpre_{entry[3:] if entry.startswith('lib') else entry}"))
            print(f"  module library {name}: prebuilt {entry} ({chip})")

    def compile_modules(self):
        """the module objects in $BUILD_DIR/mod-src, the module libraries in $BUILD_DIR/libmod"""
        menv = self.module_env()
        libraries = []
        for name in self.env.GetProjectOption("custom_module_libs", "").split():
            folder = self.library_dir(name)
            src = os.path.join(folder, "src") if os.path.isdir(os.path.join(folder, "src")) else folder
            menv.Prepend(CPPPATH=[src])
            libraries.append((name, src))

        # custom_module_lib_filter: <library> <source filter>, the part of a library the modules use (Adafruit BusIO:
        # the I2C device of the si1145 driver, its SPI device does not build on every chip)
        filters = {}
        for line in self.env.GetProjectOption("custom_module_lib_filter", "").splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2:
                filters[parts[0]] = parts[1]

        # The libraries themselves without ESPurna's src/ on the include path, as PlatformIO builds a library:
        # IRremoteESP8266 includes the core's <gpio.h>, src/gpio.h of ESPurna would be found first. A library without
        # src/ (Adafruit NeoPixel) leaves out its examples and tests, as PlatformIO does
        lenv = menv.Clone()
        project_src = os.path.normcase(os.path.abspath(self.env.subst("$PROJECT_SRC_DIR")))
        lenv.Replace(CPPPATH=[path for path in menv.get("CPPPATH", [])
                              if os.path.normcase(os.path.abspath(lenv.subst(str(path)))) != project_src])
        default_filter = "+<*> -<.git/> -<.svn/> -<examples/> -<example/> -<test/> -<tests/> -<extras/>"

        out = []
        for name, src in libraries:
            out += lenv.Flatten([lenv.BuildLibrary(os.path.join("$BUILD_DIR", MODULE_LIBS, name), src,
                                                   filters.get(name, default_filter))])
            self.precompiled(name, src)

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
        head_names = set(self.symbols(ctx["head_elf"]))
        libraries, head_only = self.lib_archives(build_dir, head_names)
        provided = read_provided(self.env)
        missing = {}
        totals = [0, 0]
        linked = set()
        ctx["provider_inputs"] = {}
        ordered = self.ordered_modules()
        in_build = {module["name"] for module in ordered}
        # the head's symbols file of the chip (ESP32: one head-symbols.ld) before the threads
        self.head_symbols(build_dir, ctx["head_elf"])
        self._libs = {}

        # waves: a module links once every module of this build it needs did; the modules of a wave link in
        # parallel, as many as the jobs of the build (-j)
        depth = {}
        for module in ordered:
            needs = [depth[need] for need in module["requires"] if need in in_build]
            depth[module["name"]] = 1 + max(needs) if needs else 0
        jobs = max(1, int(self.env.GetOption("num_jobs") or 1))
        for wave in range(max(depth.values(), default=-1) + 1):
            todo = []
            for module in ordered:
                if depth[module["name"]] != wave:
                    continue
                waiting = [need for need in module["requires"] if need in in_build and need not in linked]
                if waiting:
                    # a module it needs did not link in this pass: after that one is fixed (the next pass)
                    print(f"  {module['name']}: waits for {' '.join(waiting)}")
                    continue
                todo.append(module)
            with ThreadPoolExecutor(max_workers=jobs) as pool:
                results = list(pool.map(lambda module: self.link_one(ctx, module, head_dir, head_map, head_env, src,
                                                                        head_names, libraries, provided, linked),
                                        todo))
            for module, (sizes, error) in zip(todo, results):
                if error is not None:
                    missing[module["name"]] = error
                    continue
                totals[0] += sizes[0]
                totals[1] += sizes[1]
                linked.add(module["name"])
        return missing, totals, head_only

    def provider_libs(self, build_dir, names, tag):
        """lib_archives() for the modules linked against the providers `tag`: once per pass, the modules with the same
        providers share it"""
        with self._libs_lock:
            if tag not in self._libs:
                self._libs[tag] = self.lib_archives(build_dir, names, f"mod-libs-{tag}")[0]
            return self._libs[tag]

    def link_one(self, ctx, module, head_dir, head_map, head_env, src, head_names, libraries, provided, linked):
        """one module of link_all(); returns ((RAM, IRAM), None) or (None, MissingSymbols)"""
        build_dir = ctx["build_dir"]
        name = module["name"]
        providers = [(index, need) for index, need in enumerate(module["requires"]) if need in linked]
        if len(module["requires"]) > LINK_PROVIDERS:
            raise RuntimeError(f"module {name}: more than {LINK_PROVIDERS} modules it needs")
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
        if providers:
            # the library members with state the providers have stay theirs (Wire of the i2c module)
            names = set(head_names)
            for _, provider in providers:
                names.update(self.symbols(os.path.join(build_dir, f"module-{provider}.elf")))
            tag = "-".join(sorted(provider for _, provider in providers))
            archives += self.provider_libs(build_dir, names, tag)
        else:
            archives += libraries
        if api:
            print(f"  {name} provides {len(api)} functions through the head")

        ctx["provider_inputs"][name] = objects + archives
        try:
            return self.package(ctx, name, objects, archives, providers, provided.get(name, [])), None
        except MissingSymbols as e:
            e.providers = [provider for _, provider in providers]
            return None, e

    def provider_defined(self, ctx, provider):
        """the global symbols the inputs of a provider's link define (before --gc-sections): its objects and the
        members of the libraries its link pulled"""
        mapfile = os.path.join(ctx["build_dir"], f"module-{provider}.map")
        pulled = set(self.pulled_members(mapfile)) if os.path.exists(mapfile) else set()
        out = set()
        for path in ctx["provider_inputs"].get(provider, []):
            if path.endswith(".a"):
                base = os.path.basename(path)
                for member, defined in self.archive_members(path).items():
                    if f"{base}({member})" in pulled:
                        out.update(symbol for kind, symbol in defined if kind.isupper())
            else:
                for line in self.run([self.tool("nm"), "-g", "--defined-only", path]).splitlines():
                    parts = line.split()
                    if len(parts) == 3:
                        out.add(parts[2])
        return out

    def modules_link(self, target, source, env):
        for attempt in range(RELINKS + 1):
            ctx = self.context()
            missing, totals, head_only = self.link_all(ctx)
            if not missing:
                break

            exported_all = []
            absent_all = []
            kept_all = []
            for name, e in missing.items():
                # what a provider has: it keeps it for this module (custom_module_provided), linked again
                names = list(e.names)
                for provider in getattr(e, "providers", []):
                    defined = self.provider_defined(ctx, provider)
                    mine = [symbol for symbol in names if symbol in defined]
                    if mine:
                        kept = add_provided(self.env, provider, mine)
                        kept_all += kept
                        for symbol in mine:
                            print(f"  {name}: kept by {provider}: {symbol}")
                        names = [symbol for symbol in names if symbol not in mine]
                if not names:
                    continue
                exported, absent = self.update_exports(ctx["head_dir"], head_only, names)
                exported_all += exported
                for symbol in exported:
                    print(f"  {name}: head export added: {symbol}")
                for symbol in absent:
                    print(f"  {name}: NOT IN THE HEAD: {symbol}")
                absent_all += absent
            if absent_all or not (exported_all or kept_all) or attempt == RELINKS:
                raise RuntimeError(f"{len(set(exported_all))} head exports added, "
                                   f"{len(set(absent_all))} symbols the head does not have")
            if exported_all:
                self.relink_head()

        print(f"All modules: RAM {totals[0]}" + (f", IRAM code {totals[1]}" if self.has_iram else ""))
        container = self.write_container(ctx, [module["name"] for module in self.ordered_modules()])
        import module_initial_image  # it imports this file
        on = module_initial_image.modules_on(self.env.subst("$PROJECT_DIR"))
        images = self.initial(ctx, container, on)
        heads = self.ota_heads(ctx)
        ota = os.path.join(ctx["build_dir"], f"espurna-{self.env.subst('$PIOENV')}.ota")
        module_ota.build(ota, heads, container)
        release_image.listing(list(images) + [container, ota])

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
