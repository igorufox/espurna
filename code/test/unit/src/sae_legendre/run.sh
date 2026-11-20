#!/bin/sh
# Host test of the package's Legendre symbol (compare.c). Run in WSL from this folder: sh run.sh <package folder>
# [<mbedtls folder>]: the mbedtls is the one package of the three chips (framework-mbedtls, 3.6.6, code/libraries.ini),
# by default next to the SDK package
set -e
# the package folder (WSL path), e.g. sh run.sh /mnt/c/Users/<user>/.platformio/packages/framework-esp8266-rtos-sdk
SDK=${1:?the framework-esp8266-rtos-sdk folder}
SRC=$SDK/components/wpa_supplicant/src/crypto/crypto_mbedtls-bignum.c
MBED=${2:-$SDK/../framework-mbedtls}
OUT=$(mktemp -d)
# the two functions, from the line of the first one to the end of the second
awk '/^\/\* espurna: the Legendre symbol/{on=1} on{print} on&&/^int crypto_bignum_to_string/{exit}' "$SRC" \
    | sed '$d' > "$OUT/legendre.inc"
gcc -O1 -I"$OUT" -I"$MBED/include" -o "$OUT/compare" compare.c -I"$MBED/library" "$MBED/library/bignum.c" "$MBED/library/bignum_core.c" "$MBED/library/constant_time.c" "$MBED/library/platform_util.c"
"$OUT/compare"
