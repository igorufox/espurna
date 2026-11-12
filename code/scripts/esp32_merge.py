# ESP32: the merged image espurna-<env>.bin (bootloader, partitions, boot_app0, ESPurna, safeboot), flashed at 0x0
# and the release file (naming, listing: scripts/release_image.py). The partition table of the build
# (board_build.partitions) is read by scripts/esp32_partitions.py, which also writes its entries as the C header
# esp32_partition_csv.h for the firmware (the table the WebUI migration writes, arch/esp32/safeboot_esp32.cpp)

Import("env", "projenv")
import os
import sys

# release_image.py is in this folder: PlatformIO runs extra scripts without it on sys.path
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821
from release_image import listing, path, quote as _quote  # noqa: E402
import esp32_partitions  # noqa: E402


def _partitions_csv(env):
    """the partition csv of the build: the project's, else the framework's"""
    csv = env.BoardConfig().get("build.partitions", "default.csv")
    path = os.path.join(env.subst("$PROJECT_DIR"), csv)
    if not os.path.exists(path):
        framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
        path = os.path.join(framework_dir, "tools", "partitions", csv)
    return path


def _app_offsets(env):
    """Offsets of the app slot (ota_0, else the first app) and of the factory partition when there is an ota_0 too"""
    path = _partitions_csv(env)
    try:
        offset, factory = esp32_partitions.app_offsets(esp32_partitions.read_csv(path))
    except ValueError as error:
        raise Exception("[POST-BUILD] {}: {}".format(path, error))
    return hex(offset), (hex(factory) if factory is not None else None)


def _partition_header(env, projenv):
    """esp32_partition_csv.h in $BUILD_DIR/generated, on the include path of the firmware and the modules"""
    path = _partitions_csv(env)
    folder = os.path.join(env.subst("$BUILD_DIR"), "generated")
    esp32_partitions.write_header(esp32_partitions.read_csv(path), path,
                                  os.path.join(folder, "esp32_partition_csv.h"))
    projenv.Append(CPPPATH=[folder])


def _esptool(env, platform):
    """esptool with merge_bin (v4+), as "<python> <script or -m esptool>". The name tool-esptoolpy is shared: the
    ESP8266 platform's registry package (esptool 3.0, no merge_bin) takes the plain directory and pioarduino's
    v5 moves to tool-esptoolpy@src-*, so the package directory alone can be the wrong one. The esptool module of
    PlatformIO's penv (pioarduino installs it there) is v5"""
    import subprocess
    core = env.subst("$PROJECT_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")
    for python in (os.path.join(core, "penv", "Scripts", "python.exe"), os.path.join(core, "penv", "bin", "python")):
        if os.path.exists(python):
            proc = subprocess.run([python, "-c", "import esptool; print(esptool.__version__)"],
                                  capture_output=True, text=True)
            if proc.returncode == 0 and proc.stdout.strip()[:1].isdigit() and int(proc.stdout.strip()[0]) >= 4:
                return "{} -m esptool".format(_quote(python))

    script = os.path.join(platform.get_package_dir("tool-esptoolpy") or "", "esptool.py")
    if not os.path.exists(script):
        script = "esptool.py"  # fall back to PATH
    return "$PYTHONEXE {}".format(_quote(script))


def merge_bin(source, target, env):
    print("\n[POST-BUILD] Creating merged production binary...")

    platform = env.PioPlatform()
    esptool = _esptool(env, platform)

    build_dir = env.subst("$BUILD_DIR")
    board = env.get("BOARD_MCU") or env.BoardConfig().get("build.mcu", "esp32")
    pio_env = env["PIOENV"]

    # Derive flash size and mode from board config so non-default targets
    # aren't silently mis-flashed (bootloader and image must agree on mode).
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")
    flash_mode = env.BoardConfig().get("build.flash_mode", "dout")
    flash_freq = env.BoardConfig().get("build.f_flash", "40000000L")
    # f_flash is "40000000L" or "80000000L" — normalize to "40m" / "80m"
    # Refuse to silently fall back to 40 MHz: the bootloader and the image must
    # agree on speed or the chip won't boot, and a bad board config should
    # surface here rather than producing a brick.
    try:
        freq_mhz = int(str(flash_freq).rstrip("L")) // 1000000
    except (TypeError, ValueError):
        raise Exception(
            "[POST-BUILD] Could not parse build.f_flash={!r} from board config".format(flash_freq))
    bootloader_suffix = "{}_{}m".format(flash_mode, freq_mhz)

    output = path(build_dir, pio_env)

    bootloader = os.path.join(build_dir, "bootloader.bin")
    if not os.path.exists(bootloader):
        framework_dir = platform.get_package_dir("framework-arduinoespressif32")
        # e.g. bootloader_dout_40m.bin, bootloader_dio_80m.bin, …
        bootloader = os.path.join(framework_dir, "tools", "sdk", "bin",
                                  "bootloader_{}.bin".format(bootloader_suffix))

    partitions = os.path.join(build_dir, "partitions.bin")
    boot_app0 = os.path.join(build_dir, "boot_app0.bin")
    if not os.path.exists(boot_app0):
        framework_dir = platform.get_package_dir("framework-arduinoespressif32")
        boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    firmware = os.path.join(build_dir, "firmware.bin")
    app_offset, safeboot_offset = _app_offsets(env)

    missing = [f for f in (bootloader, partitions, boot_app0, firmware) if not os.path.exists(f)]
    if missing:
        # Fail the PIO build so CI doesn't publish an un-merged binary as success.
        raise Exception("[POST-BUILD] Missing components for merge: {}".format(", ".join(missing)))

    cmd = (
        "{esptool} --chip {chip} merge_bin -o {out} "
        "--flash_mode {fmode} --flash_size {fsize} "
        "0x1000 {bootloader} "
        "0x8000 {partitions} "
        "0xe000 {boot_app0} "
        "{app_offset} {firmware}"
    ).format(
        esptool=esptool,
        chip=board,
        out=_quote(output),
        fmode=flash_mode,
        fsize=flash_size,
        bootloader=_quote(bootloader),
        partitions=_quote(partitions),
        boot_app0=_quote(boot_app0),
        firmware=_quote(firmware),
        app_offset=app_offset,
    )

    # Safeboot layout: the factory partition holds the ESPurna safeboot image (code/safeboot-app, built before
    # this one: `pio run -d safeboot-app`). A merged image without it would leave the factory partition empty: the
    # build fails instead of releasing such an image
    project = env.subst("$PROJECT_DIR")
    safeboot = os.path.join(project, "safeboot-app", ".pio", "build", "esp32-safeboot", "firmware.bin")
    if safeboot_offset:
        if not os.path.exists(safeboot):
            raise Exception("[POST-BUILD] no safeboot image ({}): build code/safeboot-app first "
                            "(pio run -d safeboot-app)".format(safeboot))
        cmd += " {} {}".format(safeboot_offset, _quote(safeboot))

    print("Running: " + cmd)
    rc = env.Execute(cmd)
    if rc:
        raise Exception("[POST-BUILD] merge_bin failed with exit code {}".format(rc))
    listing([output])


_partition_header(env, projenv)  # noqa: F821
env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
