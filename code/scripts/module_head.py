# PROTOTYPE (docs/modules.md): the head image of the modules, every chip (extra script of the chip's env).
#
# - head functions that modules call although the head itself does not (custom_head_exports, one mangled name per
#   line, written by the module link when it finds one missing): kept in the link with EXTERN() in an implicit linker
#   script (scripts/module_link.py links the head again in the same build after a change)
# - custom_head_wraps: symbols the head links with --wrap (ESP8266: the mDNS stubs ArduinoOTA references, the heap
#   protection of heap_reserve_esp8266.cpp)
# - custom_head_build_id = yes: the GNU build id of the link in .rodata (__build_id_start), the identity a module is
#   made for where the image has no MD5 the board can take (Beken: arch/beken/modules_beken.cpp compares its first 16
#   bytes with the module header)

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


def build_id():
    if env.GetProjectOption("custom_head_build_id", "no").lower() not in ("yes", "true", "1"):
        return

    board = env.BoardConfig()
    path = os.path.join(env.subst("$BUILD_DIR"), board.get("build.ldscript"))
    with open(path, "r") as f:
        text = f.read()

    if "__build_id_start" not in text:
        anchor = "\t.rodata ALIGN(8) :\n\t{\n"
        if anchor not in text:
            raise ValueError(f"{path}: no .rodata section to patch")
        text = text.replace(anchor, anchor + "\t\t__build_id_start = .;\n\t\tKEEP(*(.note.gnu.build-id))\n", 1)
        with open(path, "w") as f:
            f.write(text)

    env.Append(LINKFLAGS=["-Wl,--build-id=sha1"])


exports()
wraps()
build_id()
