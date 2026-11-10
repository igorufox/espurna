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
# Package version 3: PackageHeader, then per 4 KiB chunk of the image: the 256 byte map (2 bits per word), the number
# of branches in the chunk (u32), the branches (u32: offset in the chunk | kind << 16, 1 Thumb BL / BLX pair, 2 ARM
# BL / BLX), the chunk. The image is padded to 32 bytes (the block of the flash format). The head identity is the first
# 16 bytes of the head's GNU build id (scripts/module_head.py, custom_head_build_id = yes).
#
# Output next to raw_firmware.elf: module-<name>.module (the package for /modules/upload), module-<name>.elf / .map,
# espurna-beken-modules.bin (EMC2, every package: the file of the WebUI MODULES panel), espurna-beken.uf2 (the head:
# LibreTiny's firmware.uf2 under its release name, first flash over UART and OTA), espurna-beken-full.bin (the head and
# every module in flash, switched off: scripts/beken_full_image.py).

import os
import re
import struct
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import module_link  # noqa: E402
from release_image import copy, listing, name as release_name, path as release_path  # noqa: E402
import beken_full_image  # noqa: E402
from module_full_image import modules_on  # noqa: E402

FLASH_A = 0x00200000

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
    package_version = 3
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

    def chunk_extra(self, branches):
        """the branches of the chunk: their number, then offset in the chunk | kind << 16"""
        def extra(start, size):
            inside = sorted(offset for offset in branches if start <= offset < start + size)
            return struct.pack("<I", len(inside)) + b"".join(
                struct.pack("<I", (offset - start) | (branches[offset] << 16)) for offset in inside)
        return extra

    def after(self, ctx, container):
        """the head under its release name: LibreTiny's firmware.uf2 (Tuya layout), first flash and OTA; the full image
        (scripts/beken_full_image.py) with the module arena of the head's ELF"""
        folder = ctx["build_dir"]
        listing([copy(os.path.join(folder, "firmware.uf2"), release_path(folder, "beken", ext=".uf2")), container])
        syms = ctx["head_syms"]
        arena = [name for name in syms if name.startswith("_ZN7espurna7modules") and name.endswith("5arenaE")]
        if len(arena) != 1:
            raise RuntimeError(f"full image: the module arena of the head: {arena}")
        address, size = syms[arena[0]]
        beken_full_image.build(folder, container, ctx["head_id"], address, size,
                               modules_on(self.env.subst("$PROJECT_DIR")))


BekenLinker(env, globals()["projenv"]).install()
