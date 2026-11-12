# PROTOTYPE (docs/modules.md, "Beken"): the BK7231N part of linking modules against the head (the shared part:
# module_link.py).
#
# Last extra script of env:beken of platformio_beken.ini (the head is the env's image). Each module is linked twice with
# scripts/beken_module.ld at two sets of placeholder bases (flash, RAM), with -q (the relocation records stay in the
# ELF). The BL / BLX whose bytes differ between the two links are calls into the head (a call inside the module does
# not change): the board moves them by the flash delta (the branch list), the linker's interworking veneers too
# (`bx pc; b <head function>`, no relocation record). Every other word that differs must differ by one of the two
# deltas (kind 1 flash, 2 RAM); anything else stops the build.
#
# The package is the format of every chip (module_link.py): its branch list per chunk holds these branches (offset in
# the chunk | kind << 16, 1 Thumb BL / BLX pair, 2 ARM BL / BLX). The image is padded to 32 bytes (the block of the
# flash format). The head identity is the first 16 bytes of the head's GNU build id (scripts/module_head.py,
# custom_head_build_id = yes).
#
# The initial images: espurna-beken-tuya.uf2 (Tuya bootloader: LibreTiny's UF2 inputs of the build, written again
# here) and espurna-beken-quectel.uf2 (scripts/beken_quectel_image.py: Quectel bootloader), first flash over UART and
# OTA. The module area is the OTA 'download' partition from its second sector (modules_beken.cpp findArea()), not
# behind the head: the modules file of the modules on by default (module_initial_image.py) is a run of its own in both
# UF2s, `<file>+0x1000=flasher:download`. The flasher (ltchiptool) writes it there; an OTA with the same UF2 skips it
# (a block of the flasher scheme only, uf2ota ignores it for the device scheme) and puts its RBL into 'download'
# as always. The install image espurna-beken.ota: both heads and every package (scripts/module_ota.py).

import os
import re
import struct
import subprocess
import sys
from datetime import datetime

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import module_initial_image  # noqa: E402
import module_link  # noqa: E402
from release_image import copy, name as release_name, path as release_path, write  # noqa: E402

FLASH_A = 0x00200000

# the module area: the 'download' partition from its second sector (modules_beken.cpp findArea())
AREA_IN_DOWNLOAD = 0x1000

BRANCH_THUMB = 1
BRANCH_ARM = 2
THUMB_TYPES = {"R_ARM_THM_CALL", "R_ARM_THM_PC22"}
ARM_TYPES = {"R_ARM_CALL", "R_ARM_JUMP24", "R_ARM_PC24", "R_ARM_PLT32"}


def thumb_offset(data, at):
    h1, h2 = struct.unpack_from("<HH", data, at)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xE800) != 0xE800:
        return None
    value = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    return value - (1 << 23) if value & (1 << 22) else value


def arm_offset(data, at):
    insn, = struct.unpack_from("<I", data, at)
    if (insn & 0x0E000000) != 0x0A000000:
        return None
    value = insn & 0xFFFFFF
    return (value - (1 << 24) if value & (1 << 23) else value) * 4


class BekenLinker(module_link.Linker):
    prefix = "arm-none-eabi-"
    container_name = release_name("beken", "modules")
    # LibreTiny's names of the head's link; its link (ltchiptool link2bin) writes the ELF and the images, then the UF2
    head_elf_name = "raw_firmware.elf"
    head_bin_name = "raw_firmware.bin"
    head_map_name = "raw_firmware.map"
    head_outputs = ("${PROGNAME}.elf", "firmware.uf2")
    link_script = "beken_module.ld"
    bases = (("FLASH", FLASH_A, 0x00280000, "flash"),
             ("RAM", 0x10000000, 0x10100000, "RAM"))
    own_sections = (".mod_text", ".mod_data", ".mod_bss")
    image_sections = (".mod_text", ".mod_data")
    # the block of the flash format
    image_align = 32
    # the same multilib as the head (ARM mode libc / libgcc) and its --wrap options: malloc & co. of a module go to the
    # head's wrappers (FreeRTOS heap), never to a newlib malloc of its own
    head_link_flags = ("-m", "--specs", "-Wl,-wrap", "-Wl,--wrap")
    # the relocation records stay in the ELF: the branch list
    link_options = ("-Wl,-q",)

    def archives_in(self, build_dir):
        """the env's libraries, and the prebuilt ones on its LIBPATH (libMatterApp.a of scripts/beken_matter_lib.py)"""
        out = self.framework_archives()
        out.update(module_link.Linker.archives_in(build_dir))
        return out

    def head_id(self, ctx):
        """first 16 bytes of the GNU build id the head has in .rodata (scripts/module_head.py)"""
        elf = ctx["head_elf"]
        at = self.symbols(elf)["__build_id_start"]
        dump = self.run([self.tool("objdump"), "-s", f"--start-address=0x{at:x}", f"--stop-address=0x{at + 36:x}", elf])
        data = b""
        for line in dump.splitlines():
            match = re.match(r"^\s*([0-9a-f]{4,8})\s+((?:[0-9a-f]{2,8}\s){1,4})", line)
            if match and int(match.group(1), 16) >= at:
                data += bytes.fromhex("".join(match.group(2).split()))
        namesz, descsz, kind = struct.unpack_from("<III", data, 0)
        if namesz != 4 or kind != 3 or data[12:16] != b"GNU\0":
            raise RuntimeError("no GNU build id in the head (custom_head_build_id = yes, scripts/module_head.py)")
        return data[16:32]

    def branch_sites(self, elf, base):
        """(offset in the image, relocation type) of every relocation record of the code left by -q"""
        out = []
        section = None
        for line in self.run([self.tool("readelf"), "-r", "-W", elf]).splitlines():
            match = re.match(r"Relocation section '([^']+)'", line)
            if match:
                section = match.group(1)
                continue
            parts = line.split()
            if section not in (".rel.mod_text", ".rela.mod_text") or len(parts) < 3:
                continue
            try:
                address = int(parts[0], 16)
            except ValueError:
                continue
            out.append((address - base, parts[2]))
        return out

    def branches(self, name, elf, image_a, image_b):
        """branches into the head: the BL / BLX with a relocation record whose bytes changed between the two links"""
        delta = self.bases[0][2] - self.bases[0][1]
        out = {}
        for offset, kind in self.branch_sites(elf, FLASH_A):
            if offset < 0 or offset + 4 > len(image_a):
                continue
            if image_a[offset : offset + 4] == image_b[offset : offset + 4]:
                continue
            if kind in THUMB_TYPES:
                a, b = thumb_offset(image_a, offset), thumb_offset(image_b, offset)
                code = BRANCH_THUMB
            elif kind in ARM_TYPES:
                a, b = arm_offset(image_a, offset), arm_offset(image_b, offset)
                code = BRANCH_ARM
            else:
                # absolute words (R_ARM_ABS32, R_ARM_TARGET1): the word comparison; anything PC relative into the head
                # that is not a branch shows up there as a word that differs by neither delta
                continue
            if a is None or b is None or (a - b) != delta:
                raise RuntimeError(f"module {name}: branch at +0x{offset:X} ({kind}) does not move with the module")
            out[offset] = code
        return out

    def odd_word(self, image_a, image_b, offset, branches):
        """interworking veneers of the linker (`bx pc; b <head function>` for a Thumb caller of an ARM function in the
        head) carry no relocation record: an ARM branch that moves with the module"""
        a, = struct.unpack_from("<I", image_a, offset)
        b, = struct.unpack_from("<I", image_b, offset)
        if (a >> 24) != (b >> 24):
            return False
        off_a, off_b = arm_offset(image_a, offset), arm_offset(image_b, offset)
        if off_a is None or off_b is None or (off_a - off_b) != self.bases[0][2] - self.bases[0][1]:
            return False
        branches[offset] = BRANCH_ARM
        return True

    def initial(self, ctx, container, on):
        """the two initial UF2s of the head, each with the modules on by default at the start of the module area"""
        folder = ctx["build_dir"]
        tuya = release_path(folder, "beken", "tuya", ext=".uf2")
        modules = module_initial_image.read_modules_on(container, on)
        extra = []
        if modules:
            on_file = write(os.path.join(folder, "modules-on.bin"),
                            modules + b"\xff" *(-len(modules) % module_initial_image.SECTOR))
            extra = [f"{on_file}+0x{AREA_IN_DOWNLOAD:X}=flasher:download"]
            print(f"  initial: the head + {', '.join(sorted(on))} at 'download' + 0x{AREA_IN_DOWNLOAD:X} "
                  f"({len(modules)} bytes)")

        # Tuya: LibreTiny's UF2 inputs of this build (its firmware.uf2) and the modules file
        if extra:
            env = self.env
            inputs = [env.subst(arg) for arg in env["UF2OTA"] if arg]
            now = datetime.now()
            subprocess.run([env.subst("$LTPYTHONEXE"), "-m", "ltchiptool", "uf2", "write", "--output", tuya,
                            "--board", env.subst("$BOARD_JSON"), "--lt-version", env.PioPlatform().version,
                            "--fw", f"espurna:{now:%y.%m.%d}", "--date", str(int(now.timestamp())), "--legacy"]
                           + inputs + extra, check=True)
        else:
            copy(os.path.join(folder, "firmware.uf2"), tuya)

        # Quectel: the same head for the Quectel bootloader, ltchiptool's python (LibreTiny's venv)
        project = self.env.subst("$PROJECT_DIR")
        quectel = os.path.join(project, self.env.GetProjectOption("custom_quectel_board", "boards_beken/xh-cb2s-bk7231m.json"))
        subprocess.run([self.env.subst("$LTPYTHONEXE"), os.path.join(project, "scripts", "beken_quectel_image.py"),
                        folder, os.path.join(folder, "board.json"), quectel] + extra, check=True)
        return [tuya, release_path(folder, "beken", "quectel", ext=".uf2")]

    def ota_heads(self, ctx):
        """both heads in the install image, the board takes the one of its bootloader"""
        folder = ctx["build_dir"]
        return [("tuya", release_path(folder, "beken", "tuya", ext=".uf2")),
                ("quectel", release_path(folder, "beken", "quectel", ext=".uf2"))]


BekenLinker(env, globals()["projenv"]).install()
