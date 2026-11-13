# Beken BK7231N: zero-initialized data of ESPurna and of the Matter library in the TCM, not in the main RAM
#
# The chip has 60 KiB of data TCM next to the 192 KiB main RAM. The linker script of the SDK puts only the .bss of
# the Wi-Fi driver and of the BLE stack there; with BLE off (CFG_SUPPORT_BLE = 0) about 45 KiB of it stay unused,
# while the heap (whatever is left of the main RAM) is what Matter runs out of. Moving .bss here gives the heap that
# much more. Only .bss / COMMON: the boot code zeroes the TCM (_sysboot_tcmbss_init), it copies nothing into it.
#
# Enable with `custom_tcm_bss = yes` in the env. The linker fails with "region `tcm' overflowed" when it does not fit.

import os
import sys

Import("env")  # noqa: F821
# release_image.py is in this folder: PlatformIO runs extra scripts without it on sys.path
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821
from release_image import enabled  # noqa: E402

# Objects of the Matter library that stay in the main RAM: with the Matter pools of the spec (5 fabrics, 15
# subscriptions) the library .bss is ~3 KiB more than the TCM has left. These two (Wi-Fi connectivity manager 3 KiB,
# access control entries 1.4 KiB) are not on any hot path
MAIN_RAM = "*libMatterApp.a:BK7231N.ConnectivityManagerImpl.cpp.o *libMatterApp.a:libaccess.ExampleAccessControlDelegate.cpp.o"

# input sections already placed by the first matching statement, so these go before the generic .bss rules.
# EXCLUDE_FILE applies to the section name right after it
MATTER = f"*libMatterApp.a:*(EXCLUDE_FILE({MAIN_RAM}) .bss EXCLUDE_FILE({MAIN_RAM}) .bss.* EXCLUDE_FILE({MAIN_RAM}) COMMON)"


def src_pattern(env, name="*.o"):
    """the project's objects ($BUILD_DIR/src/...) whatever the path separator: SCons passes the objects with '\' on
    Windows when the link command is short enough to go without a response file (the response file has '/'), and
    `*/src/*.o` then matched nothing (env:beken 2026-11-09: 38 KiB module arena and ESPurna's .bss in the main RAM).
    `?` matches either separator; the env's folder name keeps the module objects (mod-src) and libraries out"""
    return f"*{env.subst('$PIOENV')}?src?" + name.replace("/", "?")


def patterns(env):
    # `custom_tcm_main_ram = <object> ...` (e.g. arch/beken/system_beken.cpp.o): sources of the project whose
    # .bss stays in the main RAM, room in the TCM for something bigger (the module arena of platformio_beken.ini)
    objects = env.GetProjectOption("custom_tcm_main_ram", "").split()
    if not objects:
        return (MATTER, f"{src_pattern(env)}(.bss .bss.* COMMON)")
    keep = " ".join(src_pattern(env, name) for name in objects)
    return (
        MATTER,
        f"{src_pattern(env)}(EXCLUDE_FILE({keep}) .bss EXCLUDE_FILE({keep}) .bss.* EXCLUDE_FILE({keep}) COMMON)",
    )


def patch_ldscript(env):
    board = env.BoardConfig()
    path = os.path.join(env.subst("$BUILD_DIR"), board.get("build.ldscript"))
    with open(path, "r") as f:
        text = f.read()

    marker = "/* bss in the TCM, ref. scripts/beken_tcm.py */"
    end = "/* end of the bss in the TCM */"
    lines = "".join(f"\t\t{pattern}\n" for pattern in patterns(env))
    block = f"\t\t{marker}\n{lines}\t\t{end}\n"

    if marker in text:
        # patched before (LibreTiny writes the script fresh every build, this is for when it does not): the block is
        # replaced, changed patterns always land. Older patches have no end marker: the marker and 2 pattern lines
        start = text.index(f"\t\t{marker}\n")
        if end in text:
            stop = text.index("\n", text.index(end)) + 1
        else:
            stop = start
            for _ in range(3):
                stop = text.index("\n", stop) + 1
        text = text[:start] + block + text[stop:]
    else:
        anchor = "\t.tcm ALIGN(8) :\n\t{\n"
        if anchor not in text:
            raise ValueError(f"{path}: no .tcm section to patch")
        text = text.replace(anchor, anchor + block, 1)

    # the end of the TCM for the modules' fixed pool (modules_beken.cpp: from _tcmbss_end up to it)
    if "_espurna_tcm_end" not in text:
        text += "\nPROVIDE(_espurna_tcm_end = ORIGIN(tcm) + LENGTH(tcm));\n"

    with open(path, "w") as f:
        f.write(text)


if enabled(env, "custom_tcm_bss"):  # noqa: F821
    patch_ldscript(env)
