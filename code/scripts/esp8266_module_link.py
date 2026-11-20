# PROTOTYPE (docs/modules.md): the ESP8266 part of linking modules against the head (the shared part: module_link.py),
# on ESP8266_RTOS_SDK (owner 2026-11-17: NONOS removed).
#
# Last extra script of env:esp8266 of platformio_esp8266.ini (the head is the env's image). Bases of
# scripts/esp8266_module.ld: flash (code and read-only data in the one view the cache maps at 0x40200000 + offset) and
# RAM (the heap block), kinds 1 and 2 of the package, the format of every chip. No IRAM pool: as on ESP32.
#
# The libraries of the SDK come from the SDK's own link (scripts/esp8266_rtos_sdk.py, SDK_LINK_TAIL: the component
# archives and the prebuilt Wi-Fi / PHY libraries), the PlatformIO libraries from the build dir.
#
# The initial image espurna-esp8266.bin: the head as the SDK writes it to 'factory' (0x10000, arch/esp8266/partitions.csv)
# and, in the sector behind it, the modules file of the modules on by default (module_initial_image.py), written at
# 0x10000. The bootloader and the partition table come with the one flash scheme (bootloader-architecture.md).
# The install image espurna-esp8266.ota: the head and every package.

import os
import subprocess
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

# GCC 14.2: Espressif's xtensa-esp-elf for the ESP8266 too (scripts/esp8266_rtos_sdk.py sets the prefix and the core
# configuration of the chip); here, after the platform named its xtensa-lx106-elf-* tools, for ESPurna's code and the
# libraries (projenv too: the platform's names reached it before)
TOOL_PREFIX = env.get("ESPURNA_TOOL_PREFIX", "xtensa-esp-elf-")
# and the env of every library builder (cloned from env before the post: scripts)
for target_env in [env, globals()["projenv"]] + [builder.env for builder in env.GetLibBuilders()]:
    target_env.Replace(
        AR=TOOL_PREFIX + "gcc-ar", AS=TOOL_PREFIX + "as", CC=TOOL_PREFIX + "gcc", CXX=TOOL_PREFIX + "g++",
        GDB=TOOL_PREFIX + "gdb", OBJCOPY=TOOL_PREFIX + "objcopy", RANLIB=TOOL_PREFIX + "gcc-ranlib",
        SIZETOOL=TOOL_PREFIX + "size",
    )

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import esp8266_rbl  # noqa: E402
import module_initial_image  # noqa: E402
import module_link  # noqa: E402
from release_image import name as release_name, path as release_path, write  # noqa: E402

# the flash base of a data module's link
DATA_FLASH = 0x40400000

# arch/esp8266/partitions.csv: 'factory', the head at its start, the modules behind it
APP_OFFSET = 0x10000
# the directory (arch/esp8266/modules_esp8266.cpp DirectoryOffset), its second copy the sector under it
DIRECTORY_OFFSET = 0xF9000


class Esp8266Linker(module_link.Linker):
    # GCC 14.2: Espressif's xtensa-esp-elf with the core configuration of the ESP8266 (scripts/esp8266_rtos_sdk.py)
    prefix = "xtensa-esp-elf-"
    container_name = release_name("esp8266", "modules")
    link_script = "esp8266_module.ld"
    bases = (("FLASH", DATA_FLASH, 0x40600000, "flash"),
             ("RAM", 0x50000000, 0x50100000, "RAM"))
    own_sections = (".mod_flash", ".mod_data", ".mod_bss")
    image_sections = (".mod_flash", ".mod_data")
    # the newlib of the SDK's toolchain is c_nano (the SDK's own link); vsnprintf and its family are the head's
    # (arch/esp8266/stdio_esp8266.c)
    system_libs = ("-lstdc++", "-lm", "-lc_nano", "-lgcc")

    # library members a module must never carry (regress-todo.md items 24, 30): the C++ exception runtime (one
    # std::__throw_* resolved from libstdc++ instead of the head's arch/esp8266/cxx_esp8266.cpp pulled 41 KB into each
    # of led, relay, ws - owner 18.11: «исключения слишком дорого для нашей 8266») and newlib's own malloc on sbrk
    # (there is no sbrk: abort() at the module's start, 18.11)
    # (newlib 4.3 of GCC 14.2 names its members libc_a-*, 3.x lib_a-*)
    forbidden_members = ("eh_", "unwind-", "functexcept.o", "cow-stdexcept.o",
                         "lib_a-nano-mallocr.o", "lib_a-nano-reallocr.o", "lib_a-sbrkr.o",
                         "libc_a-nano-mallocr.o", "libc_a-nano-reallocr.o", "libc_a-sbrkr.o")

    def report(self, name, mapfile):
        bad = [member for member in self.pulled_members(mapfile)
               if any(member.split("(", 1)[-1].startswith(prefix) for prefix in self.forbidden_members)]
        if bad:
            raise RuntimeError(f"{name}: forbidden library members in the module: {', '.join(bad)} - export the "
                               f"head's symbol they were pulled for (espurna/modules_exports.txt)")

    def sdk_tail(self):
        return [self.env.subst(str(token)) for token in self.env.get("SDK_LINK_TAIL", [])]

    def framework_archives(self):
        """the archives of the SDK's own link (its components and prebuilt libraries)"""
        out = {}
        for token in self.sdk_tail():
            if token.endswith(".a") and os.path.isfile(token):
                out.setdefault(os.path.basename(token), token)
        return out

    def archives_in(self, build_dir):
        """the libraries a module links its own members from: the SDK's archives (in the SDK package, not in the build
        dir since the SDK comes compiled: without them the tls module's mbedtls went to the head's exports, 20.11) and
        the PlatformIO libraries of the build dir"""
        out = self.framework_archives()
        out.update(module_link.Linker.archives_in(build_dir))
        return out

    def head_defined(self, head_dir, head_only):
        """the shared part's symbols of the head and those of the SDK's archives: the SDK's objects (.c.obj, in its own
        build dir) and its prebuilt libraries are the head's too, a head export keeps a member of them in the head"""
        out = super().head_defined(head_dir, head_only)
        for path in self.framework_archives().values():
            proc = subprocess.run([self.tool("nm"), "-g", "--defined-only", path], capture_output=True, text=True)
            for line in proc.stdout.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[1] in "TDRBVWtdrb":
                    out.add(parts[2])
        return out

    def library_flags(self):
        """the library folders of the SDK's own link (-L), then the env's"""
        tail = self.sdk_tail()
        out = [f"-L{tail[index + 1]}" for index, token in enumerate(tail[:-1]) if token == "-L"]
        # and the joined ones (-L<folder>: the lx106 libraries of the SDK's toolchain file)
        out += [token for token in tail if token.startswith("-L") and len(token) > 2]
        return out + [f"-L{self.env.subst(str(p))}" for p in self.env.get("LIBPATH", [])]

    def initial(self, ctx, container, on):
        """espurna-esp8266.bin: the head, the modules on by default from the sector behind it; written at 0x10000"""
        with open(ctx["head_bin"], "rb") as f:
            head = f.read()
        area = module_link.round_up(len(head), module_initial_image.SECTOR)
        image = module_initial_image.initial_image(head, area, container, on,
                                                   DIRECTORY_OFFSET - APP_OFFSET - module_initial_image.SECTOR)
        return [write(release_path(ctx["build_dir"], "esp8266"), image)]

    def ota_heads(self, ctx):
        """the head of the install image: the head as an RBL package (esp8266_rbl.py) the head writes for the SDK's
        bootloader to install (arch/esp8266/bootloader/espurna_rbl.c); the modules, webui among them, come from the
        same install image by the Apply, as on every chip"""
        out = os.path.join(ctx["build_dir"], "espurna-esp8266.rbl")
        esp8266_rbl.write(ctx["head_bin"], out)
        return [("esp8266", out)]


Esp8266Linker(env, globals()["projenv"]).install()
