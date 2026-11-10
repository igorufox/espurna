# One stub for every chip: PlatformIO converts a sketch (espurna/espurna.ino) to espurna/espurna.ino.cpp INSIDE the
# shared source tree and deletes that file when its build ends, so a build of another env in the same checkout lost the
# file mid-compile ("espurna.ino.cpp: No such file or directory"). espurna.ino holds only the license header (setup()
# and loop() are in main.cpp): the conversion is switched off.
# ref: platformio-core/platformio/tools/piomisc.py::ConvertInoToCpp()
#
# Used as a `pre:` extra script (platformio_esp32.ini, platformio_beken.ini) and by scripts/pio_pre.py (ESP8266)
# through install(env).


def ConvertInoToCpp(env):
    pass


def install(env):
    ino = env.Glob("$PROJECT_DIR/espurna/*.ino") + env.Glob("$PROJECT_DIR/espurna/*.pde")
    if len(ino) == 1 and ino[0].name == "espurna.ino":
        env.AddMethod(ConvertInoToCpp)


# as an extra script of its own
try:
    Import("env")  # noqa: F821
    install(env)  # noqa: F821
except NameError:
    pass
