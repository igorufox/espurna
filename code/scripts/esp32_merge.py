# ESP32: the merged image espurna-<env>.bin (bootloader, partitions, boot_app0, ESPurna), flashed at 0x0, and the
# release file (naming, listing: scripts/release_image.py). The partition table of the build (board_build.partitions,
# esp32_4mb.csv) is read by scripts/esp32_partitions.py

Import("env")
import os
import sys

# release_image.py is in this folder: PlatformIO runs extra scripts without it on sys.path
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821
from release_image import listing, path, quote as _quote  # noqa: E402
import esp32_partitions  # noqa: E402


def _partitions_csv(env):
    """the partition csv of the build: the project's (board_build.partitions)"""
    csv = env.BoardConfig().get("build.partitions", "esp32_4mb.csv")
    return os.path.join(env.subst("$PROJECT_DIR"), csv)


def _app_offset(env):
    """the offset of the application partition (ota_0, else the first app)"""
    path = _partitions_csv(env)
    try:
        offset = esp32_partitions.app_offset(esp32_partitions.read_csv(path))
    except ValueError as error:
        raise Exception("[POST-BUILD] {}: {}".format(path, error))
    return hex(offset)


def _otadata_app0(path):
    """the OTA data that boots app0 (ota_0), what Arduino-ESP32's boot_app0.bin held: in the first sector the entry of
    sequence 1 (its label and state erased, CRC-32 of the sequence as esp_rom_crc32_le(UINT32_MAX, ...) computes it),
    in the second one sequence 0"""
    import struct
    import zlib

    sequence = struct.pack("<I", 1)
    erased = bytes([0xFF])
    first = sequence + erased * 24 + struct.pack("<I", zlib.crc32(sequence, 0xFFFFFFFF))
    second = struct.pack("<I", 0)
    data = first.ljust(0x1000, erased) + second.ljust(0x1000, erased)
    with open(path, "wb") as f:
        f.write(data)
    return path


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

    output = path(build_dir, pio_env)

    # ESP-IDF builds the bootloader of sdkconfig.defaults and the partition table; the OTA data that boots app0
    # (otadata at 0xE000 of esp32_4mb.csv) is made here
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    boot_app0 = _otadata_app0(os.path.join(build_dir, "boot_app0.bin"))

    firmware = os.path.join(build_dir, "firmware.bin")
    app_offset = _app_offset(env)

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

    print("Running: " + cmd)
    rc = env.Execute(cmd)
    if rc:
        raise Exception("[POST-BUILD] merge_bin failed with exit code {}".format(rc))
    listing([output])


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
