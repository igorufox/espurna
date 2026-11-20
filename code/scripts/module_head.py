# PROTOTYPE (docs/modules.md): the head image of the modules, every chip (extra script of the chip's env).
#
# - head functions that modules call although the head itself does not (custom_head_exports, one mangled name per
#   line, written by the module link when it finds one missing): kept in the link with EXTERN() in an implicit linker
#   script (scripts/module_link.py links the head again in the same build after a change)
# - custom_head_wraps: symbols the head links with --wrap
# - the GNU build id of the link (-Wl,--build-id=sha1), kept in the flash of the image at __build_id_start: the
#   identity of the head, every chip alike. A module carries its first 16 bytes (scripts/module_link.py head_id()),
#   modules.cpp compares them with the head's own; nothing is hashed at run time (the MD5 of the whole image was:
#   ESP.getSketchMD5() read the flash for every check). The linker computes it over the output without the note
#   itself, so the id is in the image it identifies. Where the note goes, by the linker script of the chip:
#   ESP8266 - .flash.rodata of the SDK's generated esp8266.project.ld (patched before the link);
#   ESP32 - .flash.rodata of the framework's sections.ld, after its input sections (a patched copy in the build dir,
#   first on LIBPATH);
#   Beken - .rodata of the board's linker script in the build dir

import os
import sys

Import("env")  # noqa: F821
env = globals()["env"]

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
import module_link  # noqa: E402


def exports():
    if not env.GetProjectOption("custom_head_exports", ""):
        return
    script, count = module_link.write_head_exports(env)
    env.Append(LINKFLAGS=[script])
    # the list changes without the command line changing: relink then
    env.Depends("$BUILD_DIR/${PROGNAME}.elf", module_link.head_exports_path(env))
    print(f"Head exports: {count} from {module_link.head_exports_path(env)}")


def wraps():
    names = env.GetProjectOption("custom_head_wraps", "").split()
    env.Append(LINKFLAGS=[f"-Wl,--wrap={name}" for name in names])


# the note is 4-byte aligned: the mark goes after the alignment, not before the fill the linker puts there (ESP32 on
# ESP-IDF: 2 bytes after .rodata1, head_id() of module_link.py read the fill)
BUILD_ID = "\t\t. = ALIGN(4);\n\t\t__build_id_start = .;\n\t\tKEEP(*(.note.gnu.build-id))\n"


def patch_script(path, anchor):
    with open(path, "r") as f:
        text = f.read()
    if "__build_id_start" in text:
        return
    if anchor not in text:
        raise ValueError(f"{path}: no place for the build id ({anchor.strip()!r})")
    text = text.replace(anchor, anchor + BUILD_ID, 1)
    with open(path, "w") as f:
        f.write(text)


def build_id():
    chip = module_link.export_chip(env)
    if chip == "esp8266":
        # ESP8266_RTOS_SDK: the head's script made by ldgen in the SDK's release (app/ld of the package), copied into
        # $BUILD_DIR/sdk/ld by scripts/esp8266_rtos_sdk.py on every build; the same .flash.rodata as ESP-IDF's, the mark
        # after its input sections: patched before every link
        script = os.path.join(env.subst("$BUILD_DIR"), "sdk", "ld", "esp8266.project.ld")

        def patch(target, source, env):  # noqa: ARG001
            patch_script(script, "    *(.rodata1)\n")

        env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", patch)
    elif chip == "esp32":
        # ESP-IDF generates the head's linker script (ldgen) as $BUILD_DIR/sections.ld during the build and links with
        # that very file: it is patched right after ldgen wrote it (a post action of the target, the build keeps the
        # patched one as its own)
        generated = os.path.join(env.subst("$BUILD_DIR"), "sections.ld")

        def patch(target, source, env):  # noqa: ARG001
            # after the input sections of rodata, not first: the output section takes the type of its first input,
            # a NOTE first made .flash.rodata a NOTE section, which esptool's elf2image leaves out of the image
            # ("Unknown section type 0x07"): 180 KB of constants missing, every head since the build id hung before
            # its first constructor (10.0.5.66 11-13/14, the bootloader rolled it back)
            patch_script(generated, "    *(.rodata1)\n")

        env.AddPostAction(generated, patch)
    else:
        board = env.BoardConfig()
        patch_script(os.path.join(env.subst("$BUILD_DIR"), board.get("build.ldscript")), "\t.rodata ALIGN(8) :\n\t{\n")

    env.Append(LINKFLAGS=["-Wl,--build-id=sha1"])


exports()
wraps()
build_id()
