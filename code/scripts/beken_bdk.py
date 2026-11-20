# Beken BK7231N: the SDK COMPILED by its release (framework-beken-bdk of igorufox/platform-beken, sdk/build.py: the BDK,
# FreeRTOS-Kernel V11.1.0 with its ARM968 port, lwIP 2.2.0 with the BDK port, Mbed TLS 3.6.6 and the glue of the BDK;
# owner 2026-11-20 «все по учебнику», as the ESP8266's scripts/esp8266_rtos_sdk.py). ESPurna never compiles the SDK:
#   flags.json  the flags ESPurna's code compiles with (cflags, cxxflags, asflags, defines, includes; ${PKG} = the
#               package) and the link (linkflags, libs: the SDK's archives, the closed libraries, the C libraries)
#   ld          bk7231n_bsp.ld, copied into the build folder on every build (module_head.py and beken_tcm.py patch it)
#   lib/libble_stack.a   the BLE 5.x stack for the ble module only, copied into the build folder (module_link.py takes
#               the archives of the build folder for the modules; no image links it)
# What depends on the board's flash layout is ESPurna's own and compiled here: the BDK's partitions of the layout
# (espurna/arch/beken/bdk/wraps, FLASH_<NAME>_OFFSET / _LENGTH of the board) and FlashDB with its FAL (Matter's key-value
# store on 'kvs': library-flashdb, espurna/arch/beken/bdk/port and config). The images (RBL, UF2): scripts/beken_image.py.

import json
import os
import shutil
import sys

from SCons.Script import Import

Import("env")  # noqa: F821

platform = env.PioPlatform()  # noqa: F821
config = env.GetProjectConfig()  # noqa: F821
board = env.BoardConfig()  # noqa: F821


def package_dir(name):
    path = platform.get_package_dir(name) if name in platform.packages else None
    if not path:
        path = os.path.join(config.get("platformio", "packages_dir"), name)
    if not os.path.isdir(path):
        sys.stderr.write("beken_bdk.py: package %s not found\n" % name)
        env.Exit(1)  # noqa: F821
    return path


PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
SDK_DIR = package_dir("framework-beken-bdk")
TOOLCHAIN_BIN = os.path.join(package_dir("toolchain-gccarmnoneeabi"), "bin")
FLASHDB_DIR = package_dir("library-flashdb")
GLUE = os.path.join(PROJECT_DIR, "espurna", "arch", "beken", "bdk")

FLAGS_PATH = os.path.join(SDK_DIR, "flags.json")
if not os.path.isfile(FLAGS_PATH):
    sys.stderr.write("beken_bdk.py: %s is not the compiled SDK (no flags.json)\n" % SDK_DIR)
    env.Exit(1)  # noqa: F821
with open(FLAGS_PATH, encoding="utf-8") as f:
    FLAGS = json.load(f)


def expand(items):
    return [item.replace("${PKG}", SDK_DIR.replace("\\", "/")) for item in items]


# --- the tools: arm-none-eabi ---------------------------------------------------------------------------------------

PREFIX = os.path.join(TOOLCHAIN_BIN, "arm-none-eabi-")
env.Replace(  # noqa: F821
    AR=PREFIX + "gcc-ar",
    AS=PREFIX + "gcc",
    CC=PREFIX + "gcc",
    CXX=PREFIX + "g++",
    GDB=PREFIX + "gdb",
    NM=PREFIX + "gcc-nm",
    LINK=PREFIX + "g++",
    OBJCOPY=PREFIX + "objcopy",
    OBJDUMP=PREFIX + "objdump",
    RANLIB=PREFIX + "gcc-ranlib",
    SIZETOOL=PREFIX + "size",
    PROGSUFFIX=".elf",
    SIZEPROGREGEXP=r"^(?:\.vectors|\.text|\.rodata|\.data|\.ARM\.exidx)\s+([0-9]+).*",
    SIZEDATAREGEXP=r"^(?:\.vectors|\.data|\.bss)\s+([0-9]+).*",
    SIZECHECKCMD="$SIZETOOL -A -d $SOURCES",
    SIZEPRINTCMD="$SIZETOOL -B -d $SOURCES",
)

# --- the flash layout of the board: FLASH_<NAME>_OFFSET / _LENGTH, FLASH_LENGTH (the BDK's partitions, FAL) -----------

FLASH = {}
for name, value in board.get("flash", {}).items():
    offset, length = value.split("+")
    FLASH[name] = (int(offset, 16), int(length, 16))

FLASH_DEFINES = [("FLASH_LENGTH", "0x%06X" % board.get("upload.flash_size"))]
for name, (offset, length) in FLASH.items():
    FLASH_DEFINES += [
        ("FLASH_%s_OFFSET" % name.upper(), "0x%06X" % offset),
        ("FLASH_%s_LENGTH" % name.upper(), "0x%06X" % length),
    ]

# --- the flags of the SDK for everything built against it ------------------------------------------------------------


def define(text):
    """NAME=value of flags.json as SCons wants it (a quoted header name stays quoted on the command line)"""
    if "=" not in text:
        return text
    name, value = text.split("=", 1)
    if value.startswith('"'):
        return (name, env.StringifyMacro(value.strip('"')))  # noqa: F821
    return (name, value)


cflags = FLAGS["cflags"]
cxxflags = FLAGS["cxxflags"]
common = [flag for flag in cflags if flag in cxxflags]
env.Append(  # noqa: F821
    CCFLAGS=common,
    CFLAGS=[flag for flag in cflags if flag not in common],
    CXXFLAGS=[flag for flag in cxxflags if flag not in common],
    ASFLAGS=FLAGS["asflags"],
    CPPDEFINES=[define(item) for item in expand(FLAGS["defines"])] + [("F_CPU", board.get("build.f_cpu"))]
    + FLASH_DEFINES,
    LINKFLAGS=FLAGS["linkflags"] + [
        "-Wl,--cref",
        '"-Wl,-Map=' + os.path.join("$BUILD_DIR", "${PROGNAME}.map") + '"',
    ],
)
# ESPurna's config of FlashDB / FAL and bdk.h first, then the SDK's include folders
env.Prepend(CPPPATH=[os.path.join(GLUE, "config"), GLUE])  # noqa: F821
env.Append(  # noqa: F821
    CPPPATH=expand(FLAGS["includes"]) + [
        os.path.join(FLASHDB_DIR, "inc"),
        os.path.join(FLASHDB_DIR, "port", "fal", "inc"),
    ]
)

# --- ESPurna's part of the SDK: the layout of the board, FlashDB --------------------------------------------------------

sdk = env.Clone()  # noqa: F821
sdk.Append(CCFLAGS=["-w", "-O1"])
archives = [
    sdk.BuildLibrary(os.path.join("$BUILD_DIR", "bdk_layout"), GLUE, "-<*> +<wraps/*.c> +<port/*.c>"),
    sdk.BuildLibrary(os.path.join("$BUILD_DIR", "flashdb"), FLASHDB_DIR, "-<*> +<src/*.c> +<port/fal/src/*.c>"),
]

# the archives of ESPurna's part first, then the SDK's own link (its archives, the closed libraries, the C libraries)
libs = expand(FLAGS["libs"])
env.Prepend(LIBS=archives)  # noqa: F821
env.Append(  # noqa: F821
    LIBPATH=[path[2:] for path in libs if path.startswith("-L")],
    # the SDK's archives as files (their full paths on the command line), the libraries of -L by name
    LIBS=[env.File(path) if path.endswith(".a") else path[2:] for path in libs if not path.startswith("-L")],  # noqa: F821
)

# --- what the build folder needs: the linker script, the BLE stack of the ble module --------------------------------

build_dir = env.subst("$BUILD_DIR")  # noqa: F821
os.makedirs(build_dir, exist_ok=True)
# PlatformIO takes build.ldscript of the board as the name of the script: the copy in the build folder, fresh every
# build (scripts/module_head.py and scripts/beken_tcm.py patch it)
env.Replace(LDSCRIPT_PATH=os.path.join("$BUILD_DIR", board.get("build.ldscript")))  # noqa: F821
with open(os.path.join(SDK_DIR, "ld", FLAGS["ldscript"]), encoding="utf-8") as source:
    script = source.read()
with open(os.path.join(build_dir, board.get("build.ldscript")), "w", encoding="utf-8", newline=chr(10)) as target:
    target.write(script)
os.makedirs(os.path.join(build_dir, "sdk"), exist_ok=True)
shutil.copyfile(os.path.join(SDK_DIR, "lib", "libble_stack.a"), os.path.join(build_dir, "sdk", "libble_stack.a"))
