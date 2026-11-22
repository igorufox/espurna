#!/bin/sh
# Host test of the ESP8266 bootloader step: the swap of the heads and the rollback (test.c). Run in WSL from this folder:
#   sh run.sh <head.bin> [step]
# <head.bin> is the new head (code/.pio/build_esp8266/esp8266/firmware.bin); the old head is made from it - other bytes
# here and there and 6000 bytes longer, so the two differ in size and content. [step]: every how many flash operations a
# power cut is tried (7 by default, 1 - every one)
set -e
HEAD=${1:?the new head, firmware.bin}
STEP=${2:-7}
HERE=$(cd "$(dirname "$0")" && pwd)
CODE=$HERE/../../../..
OUT=$CODE/test/unit/build/rbl_swap
mkdir -p "$OUT"
python3 - "$HEAD" "$OUT" <<'EOF'
import sys, random
head = open(sys.argv[1], "rb").read()
old = bytearray(head)
rng = random.Random(20)
for _ in range(2000):
    old[rng.randrange(len(old))] = rng.randrange(256)
old += bytes(rng.randrange(256) if i % 3 else 0 for i in range(6000))
open(sys.argv[2] + "/old.bin", "wb").write(old)
open(sys.argv[2] + "/new.bin", "wb").write(head)
EOF
python3 "$CODE/scripts/esp8266_rbl.py" "$OUT/new.bin" "$OUT/new.rbl"
python3 "$CODE/scripts/esp8266_rbl.py" --flash "$OUT/new.bin" "$OUT/flash.rbl"
# the same image with two kept sectors (chunks of length 0: the bootloader leaves them, the ESP32 migration's NVS)
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import esp8266_rbl as R; open(sys.argv[3], 'wb').write(R.make(open(sys.argv[2], 'rb').read(), flash=True, keep=[(0x6000, 0x2000)]))"     "$CODE/scripts" "$OUT/new.bin" "$OUT/flash-keep.rbl"
gcc -O2 -Wall -Wextra -Wno-unused-function -I"$HERE/stub" -I"$CODE/espurna/arch/esp8266/bootloader" -I"$CODE/espurna/arch/rtos/bootloader" \
    -o "$OUT/test" "$HERE/test.c"
"$OUT/test" "$OUT/old.bin" "$OUT/new.bin" "$OUT/new.rbl" "$OUT/flash.rbl" "$STEP" "$OUT/flash-keep.rbl"
