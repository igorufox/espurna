# ESP8266 on ESP8266_RTOS_SDK 3.4: the SDK COMPILED by its own release (igorufox/ESP8266_RTOS_SDK, tools/espurna/build.py
# of the espurna branch, as Espressif's lib-builder for Arduino-ESP32; owner 2026-11-20 «все по учебнику»). ESPurna
# never compiles the SDK, it links the package (framework-esp8266-rtos-sdk of libraries.ini):
#   flags.json  per image ("app", "bootloader"): the include folders, defines and flags the SDK was compiled with -
#               ESPurna's code takes the same - and the SDK's own link (archives, linker scripts, the closed libraries)
#               after ESPurna's objects; ${PKG} = the package, ${LX106} = the lx106 target libraries (esp8266_libs)
#   app/ld      copied into $BUILD_DIR/sdk/ld on every build: module_head.py patches esp8266.project.ld there
#   bootloader  ESPurna links its own: espurna_rbl.c (bootloader_after_init(), the install step of a new head) before
#               the bootloader's archives -> $BUILD_DIR/bootloader.bin (the wire writes it once at 0x0,
#               /upgrade/bootloader takes it)
#   tools       esptool elf2image (the app and the bootloader images), gen_esp32part.py (arch/esp8266/partitions.csv ->
#               $BUILD_DIR/partition-table.bin, the wire writes it once at 0x8000)
# The compiler is Espressif's xtensa-esp-elf 14.2 with the core configuration of the ESP8266 (xtensa_esp8266.so).

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys

from SCons.Script import Builder, Import

Import("env")  # noqa: F821

platform = env.PioPlatform()  # noqa: F821
config = env.GetProjectConfig()  # noqa: F821

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
SDK_OUT = os.path.join(BUILD_DIR, "sdk")
# the 1 MB layout: one 936 KB app partition (the head and the modules), the settings in sectors 250/251
PARTITIONS = os.path.join(PROJECT_DIR, "espurna", "arch", "esp8266", "partitions.csv")
# the ESPurna step of the bootloader: a new head from its RBL package (owner 2026-11-18, the Tuya scheme)
BOOTLOADER_STEP = os.path.join(PROJECT_DIR, "espurna", "arch", "esp8266", "bootloader", "espurna_rbl.c")


def package_dir(name):
    path = platform.get_package_dir(name)
    if not path:
        packages = config.get("platformio", "packages_dir")
        path = os.path.join(packages, name)
    if not os.path.isdir(path):
        sys.stderr.write("esp8266_rtos_sdk.py: package %s not found\n" % name)
        env.Exit(1)  # noqa: F821
    return path


SDK_DIR = package_dir("framework-esp8266-rtos-sdk")
# the platform's toolchain-xtensa slot: the lx106 target libraries of GCC 14.2 (libraries.ini esp8266_libs)
LX106_LIBS = package_dir("toolchain-xtensa")
TOOL_PREFIX = "xtensa-esp-elf-"

FLAGS_PATH = os.path.join(SDK_DIR, "flags.json")
if not os.path.isfile(FLAGS_PATH):
    sys.stderr.write("esp8266_rtos_sdk.py: %s is not the compiled SDK (no flags.json)\n" % SDK_DIR)
    env.Exit(1)  # noqa: F821
with open(FLAGS_PATH, encoding="utf-8") as f:
    FLAGS = json.load(f)


def tool_platform():
    machine = __import__("platform").machine().lower()
    arm = machine in ("arm64", "aarch64")
    if sys.platform.startswith("win"):
        return "win64"
    if sys.platform == "darwin":
        return "macos-arm64" if arm else "macos"
    return "linux-arm64" if arm else "linux-amd64"


def idf_tool(key):
    """The compiler ([platforms] esp8266_toolchain of libraries.ini) is the ESP32 platform's package (a pioarduino
    registry zip: a tools.json naming the program; the pioarduino installer puts the program in <core>/tools/<name>).
    It is no platform_package of the ESP8266: PlatformIO put the bare zip over the ESP32's installed package (19.11
    13:22: the ESP32 build lost its compiler). Here: the program in <core>/tools/<name> (installed by the ESP32 build,
    or by this script from the same zip and its tools.json when missing), nothing written into <core>/packages"""
    name, _, url = (part.strip() for part in config.get("platforms", key).partition("@"))
    core = os.path.dirname(config.get("platformio", "packages_dir"))
    target = os.path.join(core, "tools", name)
    if os.path.isfile(os.path.join(target, "package.json")) or os.path.isfile(os.path.join(target, ".espurna")):
        return target

    import tarfile, tempfile, urllib.request, zipfile
    with tempfile.TemporaryDirectory() as temp:
        meta = os.path.join(temp, "meta.zip")
        urllib.request.urlretrieve(url, meta)
        with zipfile.ZipFile(meta) as archive:
            spec = json.loads(next(archive.read(item) for item in archive.namelist()
                                   if os.path.basename(item) == "tools.json"))["tools"][0]
        version = spec["versions"][0]
        dist = version[tool_platform()]
        print("esp8266_rtos_sdk.py: installing %s %s from %s" % (name, version["name"], dist["url"]))
        archive = os.path.join(temp, os.path.basename(dist["url"]))
        urllib.request.urlretrieve(dist["url"], archive)
        if hashlib.sha256(open(archive, "rb").read()).hexdigest() != dist["sha256"]:
            raise RuntimeError("%s: sha256 mismatch" % dist["url"])
        unpacked = os.path.join(temp, "unpacked")
        if archive.endswith(".zip"):
            zipfile.ZipFile(archive).extractall(unpacked)
        else:
            tarfile.open(archive).extractall(unpacked)
        root = unpacked
        for _ in range(spec.get("strip_container_dirs", 0)):
            root = os.path.join(root, os.listdir(root)[0])
        if os.path.isdir(target):
            shutil.rmtree(target)
        shutil.copytree(root, target, symlinks=True)
    # zip archives keep no modes: the programs executable on Linux / macOS
    for folder in (target, os.path.join(target, "bin")):
        for entry in (os.listdir(folder) if os.path.isdir(folder) else []):
            path = os.path.join(folder, entry)
            if os.path.isfile(path):
                os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    open(os.path.join(target, ".espurna"), "w").close()
    return target


TOOLCHAIN_DIR = idf_tool("esp8266_toolchain")
TOOLCHAIN_BIN = os.path.join(TOOLCHAIN_DIR, "bin")
XTENSA_CONFIG = os.path.join(TOOLCHAIN_DIR, "lib", "xtensa_esp8266.so")
LD_DIR = os.path.join(SDK_OUT, "ld")


def posix(path):
    return path.replace("\\", "/")


def expand(tokens, ld_dir=None):
    out = []
    for token in tokens:
        if ld_dir and token == "-L${PKG}/app/ld":
            token = "-L" + posix(ld_dir)
        out.append(token.replace("${PKG}", posix(SDK_DIR)).replace("${LX106}", posix(LX106_LIBS)))
    return out


def copy_scripts():
    """the linker scripts of the app, fresh from the package on every build (module_head.py patches the project one)"""
    source = os.path.join(SDK_DIR, "app", "ld")
    os.makedirs(LD_DIR, exist_ok=True)
    for name in os.listdir(source):
        shutil.copyfile(os.path.join(source, name), os.path.join(LD_DIR, name))


def tool_env():
    out = dict(os.environ)
    out["XTENSA_GNU_CONFIG"] = XTENSA_CONFIG
    out["PATH"] = os.pathsep.join([TOOLCHAIN_BIN, out.get("PATH", "")])
    return out


def run(args, what):
    result = subprocess.run(args, env=tool_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout[-20000:])
        sys.stderr.write("esp8266_rtos_sdk.py: %s failed\n" % what)
        env.Exit(1)  # noqa: F821


ESPTOOL = os.path.join(SDK_DIR, "tools", "esptool", "esptool.py")
GEN_PART = os.path.join(SDK_DIR, "tools", "gen_esp32part.py")


def stamp_changed(path, inputs):
    digest = hashlib.sha1()
    for item in inputs:
        if os.path.isfile(item):
            with open(item, "rb") as f:
                digest.update(f.read())
        else:
            digest.update(item.encode())
    current = digest.hexdigest()
    if os.path.isfile(path) and open(path).read() == current:
        return False, current
    return True, current


def build_bootloader():
    """ESPurna's bootloader: its step (espurna_rbl.c) with the bootloader's flags, then the bootloader's own link, and
    the partition table of arch/esp8266/partitions.csv"""
    image = FLAGS["bootloader"]
    os.makedirs(SDK_OUT, exist_ok=True)
    bootloader_bin = os.path.join(BUILD_DIR, "bootloader.bin")
    table_bin = os.path.join(BUILD_DIR, "partition-table.bin")
    stamp_path = os.path.join(SDK_OUT, "bootloader.stamp")
    changed, current = stamp_changed(stamp_path, [BOOTLOADER_STEP, FLAGS_PATH, PARTITIONS,
        os.path.join(PROJECT_DIR, "espurna", "arch", "esp8266", "rbl_esp8266.h"),
        os.path.join(PROJECT_DIR, "scripts", "esp8266_rtos_sdk.py")])
    if not changed and os.path.isfile(bootloader_bin) and os.path.isfile(table_bin):
        return

    gcc = os.path.join(TOOLCHAIN_BIN, TOOL_PREFIX + "gcc")
    obj = os.path.join(SDK_OUT, "espurna_rbl.o")
    elf = os.path.join(SDK_OUT, "bootloader.elf")
    run([gcc] + expand(image["cflags"]) + ["-D" + define for define in image["defines"]]
        + ["-I" + path for path in expand(image["includes"])] + ["-c", BOOTLOADER_STEP, "-o", obj],
        "compile the bootloader step")
    # the XMC chips of the XM25QH..A series pass the SDK's startup check (espurna_rbl.c)
    run([gcc, "-nostdlib", "-Wl,--gc-sections", "-mlongcalls", "-Wl,--wrap=bootloader_flash_xmc_startup", "-o", elf, obj,
         "-Wl,-Map=" + posix(os.path.join(SDK_OUT, "bootloader.map"))] + expand(image["link"]),
        "link the bootloader")
    run([sys.executable, ESPTOOL, "--chip", "esp8266", "elf2image", "--flash_mode", "dout", "--flash_freq", "40m",
         "--flash_size", "1MB", "-o", bootloader_bin, elf], "the bootloader image")
    run([sys.executable, GEN_PART, "-q", "--offset", "0x8000", "--disable-md5sum", "--flash-size", "1MB",
         PARTITIONS, table_bin], "the partition table")
    with open(stamp_path, "w") as f:
        f.write(current)
    print("esp8266_rtos_sdk.py: %s, %s" % (bootloader_bin, table_bin))


copy_scripts()
build_bootloader()

# ESPurna's own code with the same compiler: the core configuration of the ESP8266 in its environment (every compile, the
# module links); the tool names xtensa-esp-elf-* replace the platform's xtensa-lx106-elf-* in scripts/esp8266_module_link.py
# (a post: script - the platform sets its names after this pre: script)
env["ESPURNA_TOOL_PREFIX"] = TOOL_PREFIX  # noqa: F821
env["ENV"]["XTENSA_GNU_CONFIG"] = XTENSA_CONFIG  # noqa: F821
# the module links run the tools from this process (scripts/module_link.py run(): os.environ)
os.environ["XTENSA_GNU_CONFIG"] = XTENSA_CONFIG
env["ENV"]["PATH"] = os.pathsep.join([TOOLCHAIN_BIN, env["ENV"].get("PATH", "")])  # noqa: F821

APP = FLAGS["app"]


def ours(flag):
    """the SDK's flags for ESPurna's code, but its language standard (ours: -std of build_flags) and its -Werror (the
    SDK's own code is built warning-free that way, ESPurna's warnings are shown by pio_main.py, as on every chip)"""
    return not flag.startswith("-std=") and not flag.startswith("-Werror")


# flags of both compilers to CCFLAGS, the rest to its own
cflags = [f for f in expand(APP["cflags"]) if ours(f)]
cxxflags = [f for f in expand(APP["cxxflags"]) if ours(f)]
cflags_common = [f for f in cflags if f in cxxflags]
cflags_only = [f for f in cflags if f not in cxxflags]
cxxflags_only = [f for f in cxxflags if f not in cflags]

env.Prepend(CPPPATH=expand(APP["includes"]))  # noqa: F821
env.Append(  # noqa: F821
    # F_CPU: the CPU clock of the board (a framework would define it), espurna::time::CpuClock counts by it
    # a header name (MBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\") comes out of the split of the command line as
    # /mbedtls/esp_config.h/ (its escaped quotes): the name quoted again
    CPPDEFINES=[(lambda name, value: (name, env.StringifyMacro(value.strip('/"'))) if value.strip('/"').endswith(".h")  # noqa: F821
                 else (name, value))(*d.split("=", 1)) if "=" in d else d for d in APP["defines"]]
        + [("F_CPU", "$BOARD_F_CPU")],
    CCFLAGS=cflags_common,
    CFLAGS=["-std=gnu99"] + cflags_only,
    CXXFLAGS=cxxflags_only,
    ASFLAGS=["-mlongcalls"],
    SDK_LINK_TAIL=expand(APP["link"], LD_DIR),
)

# the SDK's libraries and scripts after ESPurna's objects and libraries, in the order of its own link
env.Replace(  # noqa: F821
    # the map: the module link reads the head's symbols and sections from it (scripts/module_link.py)
    LINKFLAGS=["-nostdlib", "-Wl,--gc-sections", "-mlongcalls", "-Wl,-Map=$BUILD_DIR/${PROGNAME}.map"],
    # through a response file, as PlatformIO's own: the line is far too long for Windows
    LINKCOM="${TEMPFILE('$LINK -o $TARGET $LINKFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS $SDK_LINK_TAIL', '$LINKCOMSTR')}",
)

# the head links again when the SDK package changes: SCons sees only the command line, which names the same files
# (the program's name literally: this pre: script runs before the platform sets PROGNAME)
env.Depends(  # noqa: F821
    os.path.join(BUILD_DIR, "firmware.elf"),
    [token for token in env["SDK_LINK_TAIL"] if token.endswith(".a") and os.path.isfile(token)]  # noqa: F821
    + [FLAGS_PATH]
    + [os.path.join(LD_DIR, name) for name in os.listdir(LD_DIR)]
    # the head exports (module_head.py: its Depends on "${PROGNAME}.elf" names no file in this pre: script): an
    # export added did not link the head nor the modules again, "up to date" (11-18)
    + ([os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("custom_head_exports", ""))]  # noqa: F821
       if env.GetProjectOption("custom_head_exports", "") else []),  # noqa: F821
)

# the app image of the SDK (esptool.py elf2image, image version 3 as the SDK's own build makes it)
env.Append(  # noqa: F821
    BUILDERS=dict(
        ElfToBin=Builder(
            action=env.VerboseAction(" ".join([  # noqa: F821
                '"$PYTHONEXE"', '"%s"' % ESPTOOL,
                "--chip", "esp8266", "elf2image",
                "--version=3",
                "--flash_mode", "dout",
                "--flash_freq", "40m",
                "--flash_size", "1MB",
                "-o", "$TARGET", "$SOURCES",
            ]), "Building $TARGET"),
            suffix=".bin",
        )
    )
)


# The size check of the platform (checkprogsize) reads the app's size from a linker script's irom0_0_seg, as the NONOS
# scripts had it; the SDK's scripts have no such line: the size of 'factory' of arch/esp8266/partitions.csv instead, the
# head and its modules together
def factory_size():
    with open(PARTITIONS, encoding="utf-8") as f:
        for line in f:
            fields = [field.strip() for field in line.split("#", 1)[0].split(",")]
            if len(fields) >= 5 and fields[0] == "factory":
                return int(fields[4], 0)
    raise ValueError(f"{PARTITIONS}: no 'factory' partition")


def size_script(env):
    path = os.path.join(env.subst("$BUILD_DIR"), "sdk", "app_size.ld")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = f"MEMORY\n{{\n  irom0_0_seg : org = 0x40210000, len = 0x{factory_size():X}\n}}\n"
    if not os.path.isfile(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)
    return path


env.AddMethod(size_script, "GetActualLDScript")  # noqa: F821
