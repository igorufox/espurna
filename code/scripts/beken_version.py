# Beken: ESPurna version with the git revision (26.11.0-git<sha>), as ESP8266 gets it from pio_main.py
# (scripts/release_image.py)
import os
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]
# release_image.py is in this folder: PlatformIO runs extra scripts without it on sys.path
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821
from release_image import inject_version  # noqa: E402

inject_version(globals()["projenv"])
