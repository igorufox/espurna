# PROTOTYPE (docs/modules.md, "ESP32"): the ESP32 part of linking modules against the head (the shared part:
# module_link.py).
#
# Last extra script of env:esp32 of platformio_esp32.ini (the head is the env's image). Bases of
# scripts/esp32_module.ld: the data view, RAM and the instruction view of the same bytes; package version 2 (kinds 1
# + data view delta, 2 + RAM delta, 3 + instruction view delta); no IRAM arena. The board keeps the maps behind the
# image to relocate the module again when its maps or arena move.
#
# What differs from the other chips, besides the link script:
# - the libraries: the prebuilt ESP-IDF archives of the framework (LIBS / LIBPATH of the env) and the object folders
#   of the PlatformIO libraries (the ESP32 platform links them unarchived), packed into one archive per library;
# - the head's symbols go in as `name = address;` assignments of its DEFINED globals, not --just-symbols: pioarduino
#   links every image with --wrap for Matter, the head without the Matter library keeps `U __wrap_esp_matter_mem_*`,
#   and --just-symbols handed those to every module link, which then pulled the whole CHIP stack in.
#
# Output next to firmware.elf: module-<name>.module (the package for /modules/upload), module-<name>.elf / .map,
# espurna-esp32-modules.bin (EMC2, every package: the file of the WebUI MODULES panel), espurna-esp32-full.bin (the
# merged image with every module in app0, switched off: scripts/esp32_full_image.py).

import os
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import esp32_full_image  # noqa: E402
from module_full_image import modules_on  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name, path as release_path  # noqa: E402

DROM_A = 0x50000000
IROM_A = 0x70000000


class Esp32Linker(module_link.Linker):
    prefix = "xtensa-esp32-elf-"
    container_name = release_name("esp32", "modules")
    link_script = "esp32_module.ld"
    bases = (("DROM", DROM_A, 0x50400000, "data view"),
             ("RAM", 0x60000000, 0x60100000, "RAM"),
             ("IROM", IROM_A, 0x70300000, "code view"))
    package_version = 2
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

    def after(self, ctx, container):
        """initial (espurna-esp32.bin, the merged head image) and full (every module in app0, switched off):
        scripts/esp32_full_image.py"""
        merged = release_path(ctx["build_dir"], "esp32")
        on = modules_on(self.env.subst("$PROJECT_DIR"))
        esp32_full_image.build(ctx["head_bin"], container, ctx["build_dir"], merged if os.path.exists(merged) else None,
                               on=on)

    def ota_heads(self, ctx):
        """the head of the install image: the merged initial image (the safeboot takes its ESPurna part)"""
        return [("esp32", release_path(ctx["build_dir"], "esp32"))]


Esp32Linker(env, globals()["projenv"]).install()
