# Release images, every chip: the file names that the release, the WebUI release list and the boards know
# (espurna-<name>..., ref. .github/workflows/esp32.yml), the version string of the build, the image files written next
# to the firmware and the listing of what a build made (name, size, md5).
#
# The chip parts stay extra scripts of their own (the ini files name them), each with what only that chip has:
#   esp32_merge.py         ESP32: the merged image (bootloader, partitions, boot_app0, ESPurna, safeboot)
#   beken_matter_tail.py   Beken: the link through beken_link_tail.py, the Tuya .uf2 under its release name
#   beken_link_tail.py     Beken: the Matter tail, the OTA file of both bootloaders, the Quectel .uf2
#   beken_version.py       Beken: the version string (ESP8266: pio_main.py)
#   esp8266_full_image.py  ESP8266: the initial (.bin + .gz) and full images of the head + modules build
#
# No SCons here: beken_link_tail.py runs under ltchiptool's python, esp8266_full_image.py also alone.

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


def inject_version(projenv):
    """APP_VERSION with the git revision (26.11.0-git<sha>): the WebUI release list (html/src/fwinstall.mjs) finds the
    release the board runs by that revision"""
    from espurna_utils import app_inject_version

    app_inject_version(projenv)


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
