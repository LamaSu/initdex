"""Install initdex on an init() badge from this computer, keeping the init() firmware.

Steps: find the badge -> check the conference flash layout -> back up all 16 MB ->
write initdex to app1 (0x310000) + otadata (0xe000) -> reset. Nothing else is written.
Usage:
  python flash_badge.py [--port COM5] [--skip-backup]
  python flash_badge.py --restore-init      # boot the init() firmware again (writes otadata only)
Put the badge in download mode first: USB-C data cable, hold PWR ~2 s until the green LED lights.
"""
import argparse
import datetime
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW = os.path.join(ROOT, "docs", "firmware")
BACKUPS = os.path.join(ROOT, "private", "backups")


def find_port():
    from serial.tools import list_ports  # pyserial ships with esptool

    for p in list_ports.comports():
        if p.vid == 0x303A:  # Espressif native USB
            return p.device
    return None


def esptool(port, *args, after="no-reset"):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port, "--after", after, *args]
    print(">", " ".join(cmd), flush=True)
    if subprocess.run(cmd).returncode:
        sys.exit("esptool failed; check the cable and download mode, then retry")


def partitions(blob):
    out = []
    for i in range(0, len(blob) - 31, 32):
        e = blob[i : i + 32]
        if e[:2] != b"\xaa\x50":
            break
        ptype, subtype, offset, size = struct.unpack_from("<BBII", e, 2)
        label = e[12:28].split(b"\x00")[0].decode("ascii", "replace")
        out.append((label, ptype, subtype, offset, size))
    return out


def check_layout(blob):
    table = partitions(blob)
    for row in table:
        print("  partition %-9s type %d sub 0x%02x @0x%06x size 0x%06x" % row)
    want = {
        ("ota", 1, 0x00): (0xE000, 0x2000),     # otadata
        ("app", 0, 0x10): (0x10000, 0x300000),  # ota_0 = init() firmware
        ("app", 0, 0x11): (0x310000, 0x300000), # ota_1 = initdex
    }
    for (name, ptype, subtype), (offset, size) in want.items():
        ok = any(r[1] == ptype and r[2] == subtype and r[3] == offset and r[4] == size for r in table)
        if not ok:
            sys.exit(f"layout check failed: no {name} partition at 0x{offset:x}; refusing to write")
    print("layout OK: conference partition map")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--skip-backup", action="store_true")
    ap.add_argument("--restore-init", action="store_true")
    args = ap.parse_args()
    port = args.port or find_port()
    if not port:
        sys.exit("badge not found: use a USB-C data cable and hold PWR ~2 s until the green LED lights")
    print("badge on", port)
    os.makedirs(BACKUPS, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    pt = os.path.join(BACKUPS, f"pt-{stamp}.bin")
    esptool(port, "read-flash", "0x8000", "0xC00", pt)
    with open(pt, "rb") as f:
        check_layout(f.read())
    if args.restore_init:
        esptool(port, "write-flash", "0xe000", os.path.join(FW, "otadata-ota0.bin"), after="hard-reset")
        print("done: the badge boots the init() firmware again")
        return
    if not args.skip_backup:
        backup = os.path.join(BACKUPS, f"badge-full-{stamp}.bin")
        esptool(port, "read-flash", "0", "0x1000000", backup)
        print("backup:", backup, os.path.getsize(backup), "bytes")
    esptool(
        port, "write-flash", "-fm", "keep", "-ff", "keep", "-fs", "keep",
        "0x310000", os.path.join(FW, "initdex.bin"),
        "0xe000", os.path.join(FW, "otadata-ota1.bin"),
        after="hard-reset",
    )
    print("done: initdex installed in app1; the init() firmware is untouched in app0")


if __name__ == "__main__":
    main()
