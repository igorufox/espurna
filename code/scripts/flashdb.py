# FlashDB 1.2.0 with its FAL (library-flashdb, libraries.ini `flashdb`): the key-value store of Matter on the 'kvs'
# partition (espurna/matter_port.cpp), the same build on every chip. ESPurna compiles it, the FAL is the chip's flash:
#   espurna/libs/flashdb/fdb_cfg.h       the config of FlashDB (KVDB, FAL mode), every chip
#   espurna/arch/<chip>/flashdb/         fal_cfg.h (the flash device and the 'kvs' partition) and the device's C file
# libflashdb.a in the build folder: the matter module links it (scripts/module_link.py takes the archives of the build
# folder), its include folders go to the module objects only (ESPURNA_MODULE_CPPPATH).
#
#   extra_scripts = post:scripts/flashdb.py      (after the chip's SDK script: its flags and flash layout defines)
#   custom_flashdb_chip = beken | esp32

import os
import sys

from SCons.Script import Import

Import("env")  # noqa: F821

platform = env.PioPlatform()  # noqa: F821
config = env.GetProjectConfig()  # noqa: F821


def package_dir(name):
    path = platform.get_package_dir(name) if name in platform.packages else None
    if not path:
        path = os.path.join(config.get("platformio", "packages_dir"), name)
    if not os.path.isdir(path):
        sys.stderr.write("flashdb.py: package %s not found\n" % name)
        env.Exit(1)  # noqa: F821
    return path


PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
FLASHDB_DIR = package_dir("library-flashdb")
CHIP = env.GetProjectOption("custom_flashdb_chip")  # noqa: F821
PORT = os.path.join(PROJECT_DIR, "espurna", "arch", CHIP, "flashdb")
COMMON = os.path.join(PROJECT_DIR, "espurna", "libs", "flashdb")

includes = [
    PORT,
    COMMON,
    os.path.join(FLASHDB_DIR, "inc"),
    os.path.join(FLASHDB_DIR, "port", "fal", "inc"),
]

lib = env.Clone()  # noqa: F821
lib.Append(CPPPATH=includes, CCFLAGS=["-w"])
archive = lib.BuildLibrary(os.path.join("$BUILD_DIR", "flashdb"), FLASHDB_DIR,
                           "-<*> +<src/*.c> +<port/fal/src/*.c>")
# the device's C file as objects of its own (the source filter of BuildLibrary found nothing in the ESP-IDF build)
objects = [lib.Object(os.path.join("$BUILD_DIR", "flashdb_" + CHIP, name[:-2] + ".o"), os.path.join(PORT, name))
           for name in sorted(os.listdir(PORT)) if name.endswith(".c")]
device = lib.StaticLibrary(os.path.join("$BUILD_DIR", "flashdb_" + CHIP), objects)

env.Append(ESPURNA_MODULE_CPPPATH=includes)  # noqa: F821
# built before the head's link (nothing of the head uses them): the modules take them from the build folder
env.Prepend(LIBS=[archive, device])  # noqa: F821
