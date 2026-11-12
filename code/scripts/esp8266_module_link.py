# PROTOTYPE (docs/modules.md): the ESP8266 part of linking modules against the head (the shared part: module_link.py).
#
# Last extra script of env:esp8266 of platformio_esp8266.ini (the head is the env's image). Bases of
# scripts/esp8266_module.ld: flash, RAM and IRAM code (kinds 1, 2, 3 of the package, the format of every chip). The
# SDK's libhal comes with the libraries.
#
# The initial image espurna-esp8266.bin(.gz): the head, and in the sector behind it (the module area starts there,
# modules_esp8266.cpp headEnd()) the modules file of the modules on by default (module_initial_image.py). One file for
# the serial flash and the OTA: eboot writes the whole file from 0x0. The install image espurna-esp8266.ota: the head
# (gzip, the Updater takes it) and every package.

import os
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import module_initial_image  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name, path as release_path, write, write_gz  # noqa: E402

# the flash base of a data module's link
DATA_FLASH = 0x40400000

# 1 MB layout: EEPROM_Rotate keeps the settings in sectors 251 and 250, the directory is the sector under them
# (modules_esp8266.cpp directoryOffset()), its second copy the one under that
DIRECTORY_OFFSET = 0xF9000


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

    def library_flags(self):
        return [f"-L{self.env.subst(p)}" for p in self.env["LIBPATH"]]

    def initial(self, ctx, container, on):
        """espurna-esp8266.bin(.gz): the head, the modules on by default from the sector behind it"""
        with open(ctx["head_bin"], "rb") as f:
            head = f.read()
        area = module_link.round_up(len(head), module_initial_image.SECTOR)
        image = module_initial_image.initial_image(head, area, container, on,
                                                   DIRECTORY_OFFSET - module_initial_image.SECTOR)
        out = write(release_path(ctx["build_dir"], "esp8266"), image)
        return [out, write_gz(out, image)]

    def ota_heads(self, ctx):
        """the head of the install image: the head alone, gzip (the Updater takes it); the modules, webui among them,
        come from the same install image by the Apply, as on every chip"""
        import gzip
        out = os.path.join(ctx["build_dir"], "esp8266-head.bin.gz")
        with open(ctx["head_bin"], "rb") as f, open(out, "wb") as g:
            g.write(gzip.compress(f.read(), 9, mtime=0))
        return [("esp8266", out)]


Esp8266Linker(env, globals()["projenv"]).install()
