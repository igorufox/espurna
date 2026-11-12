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
# The initial image espurna-esp32.bin: the merged image of scripts/esp32_merge.py (bootloader, partitions, safeboot,
# the head in app0) and the modules file of the modules on by default at the start of the module area, the first 64 KB
# page behind the app image in app0 (modules_esp32.cpp findArea(); module_initial_image.py). The install image
# espurna-esp32.ota: the head (the app image, the safeboot writes it to app0) and every package.

import os
import struct
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import module_initial_image  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name, path as release_path, write  # noqa: E402

DROM_A = 0x50000000
IROM_A = 0x70000000

# esp32_4mb_safeboot.csv: app0 (ota_0)
APP_OFFSET = 0xE0000
APP_SIZE = 0x2D0000
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
    own_sections = (".mod_rodata", ".mod_text", ".mod_data", ".mod_bss")
    image_sections = (".mod_rodata", ".mod_text", ".mod_data")
    # the core config of the xtensa toolchain
    head_link_flags = ("-m",)

    def __init__(self, env, projenv=None):
        super().__init__(env, projenv)
        self._head_symbols = {}

    def archives_in(self, build_dir):
        """the framework libraries, this env's archives and its library object folders packed into archives"""
        out = dict(self.framework_archives())
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
        """the head's DEFINED global symbols as `"name" = address;` (an implicit linker script)"""
        if head_elf in self._head_symbols:
            return self._head_symbols[head_elf]
        path = os.path.join(build_dir, "head-symbols.ld")
        lines = []
        for line in self.run([self.tool("nm"), "-g", "--defined-only", head_elf]).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TDBRVWAGSuiN":
                lines.append(f'"{parts[2]}" = 0x{parts[0]};')
        with open(path, "w", newline=chr(10)) as f:
            f.write(chr(10).join(lines) + chr(10))
        self._head_symbols[head_elf] = path.replace(chr(92), "/")
        return self._head_symbols[head_elf]

    def check(self, name, syms):
        if syms["__mod_iram_start"] - IROM_A != syms["__mod_iram_load"] - DROM_A:
            raise RuntimeError(f"module {name}: the code is not at the same offset in both views")

    def image_detail(self, syms):
        return f" (rodata {syms['__mod_iram_load'] - DROM_A}, code {syms['__mod_iram_end'] - syms['__mod_iram_start']})"

    def report(self, name, mapfile):
        """IRAM_ATTR functions that came with the module: they run from flash here"""
        iram = []
        with open(mapfile, errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith(".iram1") and len(line.split()) >= 3:
                    iram.append(line.split()[0])
        if iram:
            print(f"  {name}: WARNING {len(iram)} IRAM_ATTR functions run from flash: {' '.join(iram[:12])}")

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
        """the head of the install image: the head alone (the application image, the safeboot writes it to app0), as on
        every chip; the modules, webui among them, come from the same install image by the Apply"""
        return [("esp32", ctx["head_bin"])]


Esp32Linker(env, globals()["projenv"]).install()
