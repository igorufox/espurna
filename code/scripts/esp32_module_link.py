# PROTOTYPE (docs/modules.md, "ESP32"): the ESP32 part of linking modules against the head (the shared part:
# module_link.py).
#
# Last extra script of env:esp32 of platformio_esp32.ini (the head is the env's image). Bases of
# scripts/esp32_module.ld: the data view, RAM and the instruction view of the same bytes (kinds 1, 2, 3 of the package,
# the format of every chip). The board keeps the maps behind the image to relocate the module again when its bases
# move.
#
# What differs from the other chips, besides the link script:
# - the libraries: the prebuilt ESP-IDF archives of the framework (LIBS / LIBPATH of the env) and the object folders
#   of the PlatformIO libraries (the ESP32 platform links them unarchived), packed into one archive per library;
# - the head's symbols go in as `name = address;` assignments of its DEFINED globals, not --just-symbols: pioarduino
#   links every image with --wrap for Matter, the head without the Matter library keeps `U __wrap_esp_matter_mem_*`,
#   and --just-symbols handed those to every module link, which then pulled the whole CHIP stack in.
#
# The initial image espurna-esp32.bin: the merged image of scripts/esp32_merge.py (bootloader, partitions, the head in
# app0) and the modules file of the modules on by default at the start of the module area, the first 64 KB page behind
# the app image in app0 (modules_esp32.cpp findArea(); module_initial_image.py). The install image espurna-esp32.ota:
# the head (the app image) and every package; the ESP32 head is not replaced over the network (no safeboot, owner
# 2026-11-18), the modules are.

import hashlib
import os
import struct
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import esp8266_rbl  # noqa: E402
import esp32_migrate  # noqa: E402
import module_initial_image  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name, path as release_path, write  # noqa: E402

DROM_A = 0x50000000
IROM_A = 0x70000000
IRAM_A = 0x40000000

# esp32_4mb.csv: app0 (the factory app)
APP_OFFSET = 0x10000
APP_SIZE = 0x3A0000
# the MMU page: the module area starts on one (modules_esp32.cpp findArea())
PAGE = 0x10000


def app_size(app):
    """the bytes of an app image as the board counts them (modules_esp32.cpp headSize()): the header, the segments,
    the checksum at the end of a 16 byte block, the SHA-256 when appended"""
    if len(app) < 24 or app[0] != 0xE9:
        raise ValueError("not an ESP32 app image")
    pos = 24
    for _ in range(app[1]):
        pos += 8 + struct.unpack_from("<I", app, pos + 4)[0]
    pos = module_link.round_up(pos + 1, 16)
    if app[23]:
        pos += 32
    if pos > len(app):
        raise ValueError("the app image is shorter than its segments")
    return pos


class Esp32Linker(module_link.Linker):
    prefix = "xtensa-esp32-elf-"
    container_name = release_name("esp32", "modules")
    link_script = "esp32_module.ld"
    bases = (("DROM", DROM_A, 0x50400000, "data view"),
             ("RAM", 0x60000000, 0x60100000, "RAM"),
             ("IROM", IROM_A, 0x70300000, "code view"))
    # the pool code (IRAM_ATTR: the BT controller's ISR, what runs with the flash cache off), into the IRAM pool of
    # the head (modules_esp32.cpp pools()); package version 7
    pool_base = ("IRAM", IRAM_A, 0x40020000, "IRAM pool")
    own_sections = (".mod_rodata", ".mod_text", ".mod_iram", ".mod_data", ".mod_bss")
    image_sections = (".mod_rodata", ".mod_text", ".mod_iram", ".mod_data")
    # the core config of the xtensa toolchain
    head_link_flags = ("-m",)

    def __init__(self, env, projenv=None):
        super().__init__(env, projenv)
        self._head_symbols = {}

    def archives_in(self, build_dir):
        """the framework libraries, this env's archives and its library object folders packed into archives"""
        out = dict(self.framework_archives())
        # the bt component of ESP-IDF (NimBLE, the controller's glue): the ble module's, not the head's (the other
        # component archives of esp-idf/ are the head's)
        bt = os.path.join(build_dir, "esp-idf", "bt", "libbt.a")
        if os.path.isfile(bt):
            out["libbt.a"] = bt
        work = os.path.join(build_dir, "mod-pio")
        os.makedirs(work, exist_ok=True)
        for entry in sorted(os.listdir(build_dir)):
            path = os.path.join(build_dir, entry)
            if entry.endswith(".a") and not entry.startswith("module-"):
                out[entry] = path
            elif os.path.isdir(path) and entry.startswith("lib"):
                for library in sorted(os.listdir(path)):
                    folder = os.path.join(path, library)
                    if not os.path.isdir(folder):
                        continue
                    objects = [os.path.join(root, name) for root, _, names in os.walk(folder)
                               for name in names if name.endswith(".o")]
                    if not objects:
                        continue
                    archive = os.path.join(work, f"libpio_{entry}_{library}.a")
                    newest = max(os.path.getmtime(o) for o in objects)
                    if not os.path.exists(archive) or os.path.getmtime(archive) < newest:
                        if os.path.exists(archive):
                            os.remove(archive)
                        rsp = archive + ".rsp"
                        with open(rsp, "w") as f:
                            f.write(chr(10).join(o.replace(chr(92), "/") for o in objects))
                        self.run([self.tool("ar"), "rcs", archive, f"@{rsp}"])
                    out[os.path.basename(archive)] = archive
        return out

    def head_symbols(self, build_dir, head_elf):
        """the head's DEFINED global symbols as `"name" = address;` (an implicit linker script); per head image: the
        relinked head (new exports) is the same path, a cache by path kept the old symbols and every relink exported the
        same symbols again"""
        stat = os.stat(head_elf)
        key = (head_elf, stat.st_mtime_ns, stat.st_size)
        if key in self._head_symbols:
            return self._head_symbols[key]
        path = os.path.join(build_dir, "head-symbols.ld")
        lines = []
        for line in self.run([self.tool("nm"), "-g", "--defined-only", head_elf]).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TDBRVWAGSuiN":
                lines.append(f'"{parts[2]}" = 0x{parts[0]};')
        with open(path, "w", newline=chr(10)) as f:
            f.write(chr(10).join(lines) + chr(10))
        self._head_symbols[key] = path.replace(chr(92), "/")
        return self._head_symbols[key]

    def symbols_arg(self, build_dir, elf, names, out):
        """the symbols of a provider module as the head's: `"name" = address;` (an implicit linker script)"""
        wanted = set(names)
        lines = []
        for line in self.run([self.tool("nm"), "-g", "--defined-only", elf]).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in wanted:
                lines.append(f'"{parts[2]}" = 0x{parts[0]};')
        path = out + ".ld"
        with open(path, "w", newline=chr(10)) as f:
            f.write(chr(10).join(lines) + chr(10))
        return path.replace(chr(92), "/")

    def link(self, build_dir, name, head_elf, objects, archives, tag, placed, providers=(), keep=()):
        """the module's objects with the literals of their IRAM_ATTR functions next to them in the pool code"""
        objects = [self.iram_literals(build_dir, obj) for obj in objects]
        return super().link(build_dir, name, head_elf, objects, archives, tag, placed, providers, keep)

    def iram_literals(self, build_dir, obj):
        """an IRAM_ATTR function defined in a header (an inline, a template: a COMDAT) keeps its literals in
        .literal.<function>, not .iram1.<n>.literal: the link script would put them in the code view (flash), and an
        l32r of the pool code reaches only back into the pool. A copy of the object with those sections renamed
        .iram1.literal.<function> (esp32_module.ld: .mod_iram, literals first); the object itself when it has none"""
        with self._libs_lock:
            cache = self.__dict__.setdefault("_iram_literal_cache", {})
            stamp = (obj, os.path.getmtime(obj))
            if stamp in cache:
                return cache[stamp]

        functions = set()
        for line in self.run([self.tool("objdump"), "-t", obj]).splitlines():
            parts = line.split()
            if len(parts) >= 6 and parts[-3].startswith(".iram1") and "F" in parts[1:-3]:
                functions.add(parts[-1])
        renames = []
        if functions:
            for line in self.run([self.tool("objdump"), "-h", obj]).splitlines():
                parts = line.split()
                if len(parts) >= 2 and parts[0].isdigit() and parts[1].startswith(".literal."):
                    if parts[1][len(".literal."):] in functions:
                        renames.append(parts[1])

        out = obj
        if renames:
            folder = os.path.join(build_dir, "mod-iram-literals")
            os.makedirs(folder, exist_ok=True)
            digest = hashlib.md5(os.path.abspath(obj).encode()).hexdigest()[:8]
            out = os.path.join(folder, f"{digest}-{os.path.basename(obj)}")
            args = [self.tool("objcopy")]
            for section in renames:
                args.append(f"--rename-section={section}=.iram1.literal.{section[len('.literal.'):]}")
            self.run(args + [obj, out])

        with self._libs_lock:
            self.__dict__["_iram_literal_cache"][stamp] = out
        return out

    def library_flags(self):
        """the ROM's address table (esp_rom esp32.rom.ld: PROVIDE only, a symbol is defined when a module calls it): the
        BT controller in ROM (ke_*, llc*, r_*, nvds_*, _data_start_btdm ...) the head never calls, so its ELF has none"""
        package = self.env.PioPlatform().get_package_dir("framework-espidf")
        rom = os.path.join(package, "components", "esp_rom", "esp32", "ld", "esp32.rom.ld") if package else ""
        return [rom.replace(chr(92), "/")] if rom and os.path.isfile(rom) else []

    def check(self, name, syms):
        if syms["__mod_text_start"] - IROM_A != syms["__mod_text_lma"] - DROM_A:
            raise RuntimeError(f"module {name}: the code is not at the same offset in both views")

    def image_detail(self, syms):
        return f" (rodata {syms['__mod_text_lma'] - DROM_A}, code {syms['__mod_text_end'] - syms['__mod_text_start']})"

    def initial(self, ctx, container, on):
        """espurna-esp32.bin: the merged image (esp32_merge.py, run after firmware.bin), the modules on by default at
        the start of the module area in app0"""
        with open(ctx["head_bin"], "rb") as f:
            head = f.read()
        merged = release_path(ctx["build_dir"], "esp32")
        with open(merged, "rb") as f:
            image = f.read()
        if image[APP_OFFSET: APP_OFFSET + len(head)] != head:
            raise ValueError(f"{merged} does not hold this head at 0x{APP_OFFSET:X}")
        # the merged image up to the head's end: a modules file of an earlier run of this script goes
        area = module_link.round_up(APP_OFFSET + app_size(head), PAGE)
        image = module_initial_image.initial_image(image[: APP_OFFSET + len(head)], area, container, on,
                                                   APP_OFFSET + APP_SIZE - 2 * module_initial_image.SECTOR)
        return [write(merged, image)]

    def ota_heads(self, ctx):
        """the head of the install image: the head as an RBL package (esp8266_rbl.py, the name "esp32"), the ESP8266's
        scheme (owner 22.11): the head writes it under the modules' directory, the bootloader step installs it
        (arch/rtos/bootloader/espurna_rbl.c); the modules, webui among them, come from the same install image by the
        Apply, as on every chip"""
        out = os.path.join(ctx["build_dir"], "espurna-esp32.rbl")
        esp8266_rbl.write(ctx["head_bin"], out, name=b"esp32")
        # the migration of the safeboot layout's boards (esp32_migrate.py): this build's flash from 0x0 for the /upgrade
        # of this head started from their app0
        bootloader = os.path.join(ctx["build_dir"], "bootloader.bin")
        table = os.path.join(ctx["build_dir"], "partitions.bin")
        if os.path.isfile(bootloader) and os.path.isfile(table):
            with open(bootloader, "rb") as f_boot, open(table, "rb") as f_table, open(ctx["head_bin"], "rb") as f_head:
                esp32_migrate.save(esp32_migrate.over(f_boot.read(), f_table.read(), f_head.read()),
                                   os.path.join(ctx["build_dir"], "espurna-esp32-migrate.rbl"))
        return [("esp32", out)]


Esp32Linker(env, globals()["projenv"]).install()
