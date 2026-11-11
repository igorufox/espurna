# PROTOTYPE (docs/modules.md): the ESP8266 part of linking modules against the head (the shared part: module_link.py).
#
# Last extra script of env:esp8266 of platformio_esp8266.ini (the head is the env's image). Bases of
# scripts/esp8266_module.ld: flash, RAM and IRAM (the head's IRAM arena), package version 1 (kinds 1 + flash delta,
# 2 + RAM delta, 3 + IRAM delta); the board adds the deltas as the package arrives. The SDK's libhal comes with the
# libraries. Only the ESP8266 head takes data modules: the WebUI page is one (module_link.data_package).
#
# Output next to firmware.elf: module-<name>.module (the package for /modules/upload), module-<name>.elf / .map, the
# modules file espurna-esp8266-modules.bin (with the WebUI data module), espurna-esp8266.bin(.gz) (the head, "initial")
# and espurna-esp8266-full.bin (scripts/esp8266_full_image.py).

import os
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import esp8266_full_image  # noqa: E402
import module_full_image  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name  # noqa: E402

# the flash base of a data module's link
DATA_FLASH = 0x40400000


class Esp8266Linker(module_link.Linker):
    prefix = "xtensa-lx106-elf-"
    container_name = release_name("esp8266", "modules")
    link_script = "esp8266_module.ld"
    bases = (("FLASH", DATA_FLASH, 0x40600000, "flash"),
             ("RAM", 0x50000000, 0x50100000, "RAM"),
             ("IRAM", 0x60000000, 0x60300000, "IRAM"))
    own_sections = (".mod_flash", ".mod_iram", ".mod_data", ".mod_bss")
    image_sections = (".mod_flash", ".mod_iram", ".mod_data")
    system_libs = module_link.Linker.system_libs + ("-lhal",)
    has_iram = True
    # the RAM of a module may go to the IRAM tail (ram=iram) when the DRAM arena is too small
    ram_in_arena = False

    def library_flags(self):
        return [f"-L{self.env.subst(p)}" for p in self.env["LIBPATH"]]

    def arenas(self, ctx):
        head_syms = ctx["head_syms"]
        return [("RAM", head_syms.get("espurna_modules_ram_arena", (0, 0))[1]),
                ("IRAM", head_syms.get("espurna_modules_iram_arena", (0, 0))[1])]

    def webui_page(self):
        """the WebUI of the data module: the one WebUI build (gulp build), html/build/index.html.gz"""
        page = os.path.join(self.env.subst("$PROJECT_DIR"), "html", "build", "index.html.gz")
        return page if os.path.exists(page) else None

    def extra_sources(self):
        # a new WebUI build (gulp build) has to reach the modules file too
        page = self.webui_page()
        return [page] if page else []

    def extra_packages(self, ctx):
        """the WebUI data module"""
        page = self.webui_page()
        if not page:
            return []
        with open(page, "rb") as f:
            data = module_link.data_package("webui", f.read(), ctx["head_id"], DATA_FLASH)
        out = os.path.join(ctx["build_dir"], "module-webui.module")
        with open(out, "wb") as f:
            f.write(data)
        print(f"  webui from {os.path.basename(page)}")
        return [out]

    def after(self, ctx, container):
        """initial (the head) and full (head + every module placed + directory + the modules file behind 1 MB);
        custom_full_ram_in_iram: modules whose RAM goes to the IRAM tail when switched on; the modules on out of the
        box (MODULES_ON_BY_DEFAULT of espurna/config/head.h, the captive portal), in the tail of the initial image"""
        in_iram = set(self.env.GetProjectOption("custom_full_ram_in_iram", "").split())
        on = module_full_image.modules_on(self.env.subst("$PROJECT_DIR"))
        esp8266_full_image.build(ctx["head_bin"], container, ctx["build_dir"], in_iram, on)

    def ota_heads(self, ctx):
        """the head of the install image: the initial image (head + the modules on by default in its tail)"""
        return [("esp8266", os.path.join(ctx["build_dir"], "espurna-esp8266.bin"))]


Esp8266Linker(env, globals()["projenv"]).install()
