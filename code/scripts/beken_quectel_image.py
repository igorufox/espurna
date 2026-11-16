# Beken: the second initial image of the head, for the Quectel bootloader (XH-CB2S / BK7231M, 4 MiB, no flash
# encryption). The same head as espurna-beken-tuya.uf2 (Tuya layout): the application partition starts at 0x11000 on both,
# only the OTA 'download' partition and the RBL format differ (ref. arch/beken/layout_beken.cpp).
#
#   <ltchiptool python> beken_quectel_image.py <build dir> <board json of the build> <board json of the Quectel layout>
#       [<file>+<offset>=flasher:download]
#
# Writes espurna-beken-quectel.uf2: the whole application partition (image, CRC of every 32 bytes, RBL header at its
# end) for the first flash over UART, and the RBL of the Quectel layout for the 'download' partition, the OTA. Its
# BOARD tag names the Quectel layout: the board refuses a UF2 of the other bootloader (arch/beken/ota_beken.cpp).
# The optional last input is the modules file of the modules on by default at the start of the module area ('download'
# + 0x1000, module_initial_image.py), for the flasher only, the same run as in espurna-beken-tuya.uf2.
# Run by scripts/beken_module_link.py after the head's link, under ltchiptool's python (LibreTiny's venv).

import json
import os
import subprocess
import sys
from datetime import datetime
from io import BytesIO

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_image import listing, path as release_path, write  # noqa: E402

# the UF2 BOARD tag of this image: the board checks it before writing anything
BOARD_TAG = "bk7231n-quectel"


def merged(base, extra):
    out = dict(base)
    for key, value in extra.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = merged(out[key], value)
        else:
            out[key] = value
    return out


def build(folder, board_json, quectel_json, extra=()):
    from ltchiptool import Board
    from ltchiptool.soc.bk72xx.binary import to_offset
    from ltchiptool.soc.bk72xx.util import RBL, BekenBinary, DataType, OTACompression, OTAEncryption

    with open(os.path.join(folder, "raw_firmware.bin"), "rb") as f:
        raw = f.read()

    with open(board_json, "r") as f:
        board = json.load(f)
    with open(quectel_json, "r") as f:
        board = merged(board, json.load(f))
    board["name"] = BOARD_TAG
    board_path = os.path.join(folder, f"board-{BOARD_TAG}.json")
    with open(board_path, "w") as f:
        json.dump(board, f, indent=1)
    qboard = Board(board_path)

    app_size = int(qboard["build.bkrbl_size_app"], 16)
    if len(raw) > app_size:
        raise ValueError(f"the head ({len(raw)} bytes) does not fit the Quectel layout ({app_size})")

    version = f"{datetime.now():%y.%m.%d}-{qboard['build.mcu']}"
    coeffs = qboard["build.bkcrypt_coeffs"] or ("0" * 32)
    bk = BekenBinary(coeffs)

    # the OTA: the RBL the bootloader installs from 'download'
    ota_rbl = RBL(name="app", version=version, encryption=OTAEncryption.AES256, compression=OTACompression.GZIP)
    ota = b"".join(bk.ota_package(BytesIO(raw), ota_rbl, key=qboard["build.bkota.key"], iv=qboard["build.bkota.iv"]))
    download = qboard.region("download")[1]
    if len(ota) > download:
        raise ValueError(f"the OTA image ({len(ota)} bytes) does not fit the Quectel 'download' partition ({download})")
    ota_path = write(os.path.join(folder, "image_quectel_app.ota.rbl"), ota)

    # the first flash over UART: the image with the CRC of every 32 bytes and the RBL header at the end of the
    # application partition, in one aligned run (the RBL header alone at its unaligned offset fails to write there)
    app_rbl = RBL(name="app", version=version, container_size=app_size)
    crc_data = b""
    rblh_data = b""
    for data_type, data in bk.package(BytesIO(raw), 0x10000, len(raw), app_rbl, partial=True):
        if data_type == DataType.BINARY:
            crc_data += data
        elif data_type == DataType.RBL:
            rblh_data += data

    rbl_offs = to_offset(app_size) - 102
    if rbl_offs < len(crc_data):
        raise ValueError("the image overlaps the RBL header of the Quectel layout")
    # the RBL header in its own aligned run, the last sector of the partition (0xFF before the header in it): not the
    # whole gap from the image to it, ~1 MB of 0xFF in the UF2 (W62). An image reaching into that sector: one run
    tail_at = rbl_offs & ~0xFFF
    runs = []
    if len(crc_data) <= tail_at:
        app_path = write(os.path.join(folder, "image_quectel_app.0x011000.rbl"), crc_data)
        tail_path = write(os.path.join(folder, "image_quectel_app.rblh.bin"), bytes([0xFF]) * (rbl_offs - tail_at),
                          rblh_data)
        runs = [f"{app_path}=flasher:app", f"{tail_path}+0x{tail_at:X}=flasher:app"]
    else:
        app_path = write(os.path.join(folder, "image_quectel_app.0x011000.rbl"), crc_data,
                         bytes([0xFF]) * (rbl_offs - len(crc_data)), rblh_data)
        runs = [f"{app_path}=flasher:app"]

    uf2_path = release_path(folder, "beken", "quectel", ext=".uf2")
    subprocess.run(
        [
            sys.executable, "-m", "ltchiptool", "uf2", "write",
            "--output", uf2_path,
            "--board", board_path,
            "--fw", f"espurna:{datetime.now():%y.%m.%d}",
            "--date", str(int(datetime.now().timestamp())),
            "--legacy",
        ] + runs + [
            f"{ota_path}=device:download",
        ] + list(extra),
        check=True,
    )
    listing([uf2_path])
    return uf2_path


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit("usage: beken_quectel_image.py <build dir> <board json> <quectel json> [<file>+<offset>=flasher:download]")
    build(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
