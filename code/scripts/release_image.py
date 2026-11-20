# Release images, every chip: the file names that the release, the WebUI release list and the boards know
# (espurna-<name>..., ref. .github/workflows/esp32.yml), the image files written next
# to the firmware and the listing of what a build made (name, size, md5).
#
# The release files of every chip: the initial image (module_initial_image.py: the head + the modules on by default at
# the start of the module area; espurna-esp8266.bin(.gz), espurna-esp32.bin, espurna-beken-tuya.uf2 and
# espurna-beken-quectel.uf2), the modules file espurna-<chip>-modules.bin and the install image espurna-<chip>.ota
# (module_ota.py: the head(s) and every module package), all written by module_link.py and the chip's link script.
# What only a chip has stays in a script of its own:
#   esp32_merge.py           ESP32: the merged image (bootloader, partitions, boot_app0, ESPurna)
#   beken_quectel_image.py   Beken: the head for the Quectel bootloader
# The version string of the build (APP_VERSION with the git revision): pio_main.py, the post: script of every chip.
#
# No SCons here: beken_quectel_image.py imports it as a plain library.

import gzip
import hashlib
import os
import shutil

PREFIX = "espurna"


def name(*parts, ext=".bin"):
    """espurna-<part>-<part><ext>, the name of a release file"""
    return "-".join((PREFIX,) + parts) + ext


def path(folder, *parts, ext=".bin"):
    return os.path.join(folder, name(*parts, ext=ext))


def write(target, *chunks):
    with open(target, "wb") as f:
        for chunk in chunks:
            f.write(chunk)
    return target


def write_gz(target, data):
    """<target>.gz: level 9 and no time stamp, the same bytes for the same image"""
    return write(target + ".gz", gzip.compress(data, 9, mtime=0))


def copy(source, target):
    shutil.copyfile(source, target)
    return target


def quote(target):
    """a path in double quotes, spaces survive cmd.exe and the shell"""
    return '"{}"'.format(target)


def enabled(env, option):
    """`<option> = yes` in the env"""
    return env.GetProjectOption(option, "no") in ("yes", "true", "1")


def listing(paths):
    """the images of the build: name, size, md5"""
    paths = [target for target in paths if os.path.exists(target)]
    if not paths:
        return
    print("Images in {}:".format(os.path.dirname(paths[0])))
    width = max(len(os.path.basename(target)) for target in paths)
    for target in paths:
        with open(target, "rb") as f:
            md5 = hashlib.md5(f.read()).hexdigest()
        print("  {:<{}} {:>9} {}".format(os.path.basename(target), width, os.path.getsize(target), md5))
