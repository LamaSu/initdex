#!/usr/bin/env python3
"""Check that an init() badge's partition table is safe for an initdex install.

initdex is written to exactly two places on the badge:
  firmware/initdex.bin       -> 0x310000 (the ota_1 / "app1" slot)
  firmware/otadata-ota1.bin  -> 0xe000   (the otadata partition; selects app1)
The init() conference firmware (app0 at 0x10000) and the profile storage
(ffat at 0x610000) are never written.

Usage (esptool >= 4, stdlib-only Python):
  python -m esptool --chip esp32s3 --port PORT read_flash 0x8000 0xC00 pt.bin
  python check_layout.py pt.bin
  python check_layout.py --self-test

Exit codes:
  0  layout is safe: otadata at 0xe000 (size 0x2000), an ota_1 app at
     0x310000 (size 0x300000), nothing else overlapping those two ranges,
     and the table checksum (if present) matches.
  1  the layout differs; DO NOT write anything.
  2  the file could not be read or is not an ESP-IDF partition table.
"""
import hashlib
import struct
import sys

ENTRY_SIZE = 32
ENTRY_MAGIC = b"\xaa\x50"
MD5_MAGIC = b"\xeb\xeb"

TYPE_APP, TYPE_DATA = 0x00, 0x01
SUBTYPE_OTADATA = 0x00          # data/ota
SUBTYPE_OTA_1 = 0x11            # app/ota_1

OTADATA_RANGE = (0xE000, 0x2000)
APP1_RANGE = (0x310000, 0x300000)

TYPE_NAMES = {TYPE_APP: "app", TYPE_DATA: "data"}
DATA_SUBTYPES = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs"}


class LayoutError(Exception):
    """The input is not a readable ESP-IDF partition table."""


def parse_table(blob):
    """Return (entries, md5_ok). md5_ok is None when the table has no MD5 entry."""
    entries = []
    for off in range(0, len(blob) - ENTRY_SIZE + 1, ENTRY_SIZE):
        rec = blob[off:off + ENTRY_SIZE]
        if rec == b"\xff" * ENTRY_SIZE:
            return entries, None
        if rec[:2] == MD5_MAGIC:
            return entries, rec[16:32] == hashlib.md5(blob[:off]).digest()
        if rec[:2] != ENTRY_MAGIC:
            raise LayoutError("entry %d at +0x%x has magic %s, not aa50" % (len(entries) + 1, off, rec[:2].hex()))
        ptype, subtype, offset, size = struct.unpack_from("<BBII", rec, 2)
        (flags,) = struct.unpack_from("<I", rec, 28)
        label = rec[12:28].split(b"\x00", 1)[0].decode("ascii", "replace")
        entries.append({"label": label, "type": ptype, "subtype": subtype,
                        "offset": offset, "size": size, "flags": flags})
    raise LayoutError("no end-of-table marker found in %d bytes (truncated dump?)" % len(blob))


def describe(entry):
    if entry["type"] == TYPE_APP:
        sub = "factory" if entry["subtype"] == 0 else ("ota_%d" % (entry["subtype"] - 0x10) if 0x10 <= entry["subtype"] <= 0x1F else "0x%02x" % entry["subtype"])
    else:
        sub = DATA_SUBTYPES.get(entry["subtype"], "0x%02x" % entry["subtype"])
    kind = TYPE_NAMES.get(entry["type"], "0x%02x" % entry["type"])
    return "%-10s %-4s %-8s offset 0x%06x size 0x%06x" % (entry["label"], kind, sub, entry["offset"], entry["size"])


def overlaps(entry, rng):
    start, size = rng
    return entry["offset"] < start + size and start < entry["offset"] + entry["size"]


def check(entries, md5_ok):
    """Return a list of problems; an empty list means the layout is safe."""
    problems = []
    if md5_ok is False:
        problems.append("partition table MD5 does not match (corrupt read or modified table)")
    otadata = [e for e in entries if e["type"] == TYPE_DATA and e["subtype"] == SUBTYPE_OTADATA]
    app1 = [e for e in entries if e["type"] == TYPE_APP and e["subtype"] == SUBTYPE_OTA_1]
    if len(otadata) != 1:
        problems.append("expected exactly one otadata partition, found %d" % len(otadata))
    elif (otadata[0]["offset"], otadata[0]["size"]) != OTADATA_RANGE:
        problems.append("otadata is at 0x%x size 0x%x, expected 0xe000 size 0x2000" % (otadata[0]["offset"], otadata[0]["size"]))
    if len(app1) != 1:
        problems.append("expected exactly one ota_1 app partition, found %d" % len(app1))
    elif (app1[0]["offset"], app1[0]["size"]) != APP1_RANGE:
        problems.append("ota_1 is at 0x%x size 0x%x, expected 0x310000 size 0x300000" % (app1[0]["offset"], app1[0]["size"]))
    for e in entries:
        if e in otadata or e in app1:
            continue
        for name, rng in (("otadata", OTADATA_RANGE), ("ota_1", APP1_RANGE)):
            if overlaps(e, rng):
                problems.append("partition '%s' overlaps the %s range" % (e["label"], name))
    return problems


def check_file(path):
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        print("check_layout: cannot read %s: %s" % (path, exc))
        return 2
    try:
        entries, md5_ok = parse_table(blob)
    except LayoutError as exc:
        print("check_layout: not a valid partition table: %s" % exc)
        print("RESULT: UNREADABLE - do not write anything.")
        return 2
    print("Partition table (%d entries, md5 %s):" % (len(entries), {None: "absent", True: "ok", False: "MISMATCH"}[md5_ok]))
    for e in entries:
        print("  " + describe(e))
    problems = check(entries, md5_ok)
    if problems:
        for p in problems:
            print("PROBLEM: " + p)
        print("RESULT: DIFFERENT LAYOUT - do not write anything.")
        return 1
    print("RESULT: OK - safe to write initdex.bin at 0x310000 and otadata-ota1.bin at 0xe000.")
    return 0


# ---- self-test -------------------------------------------------------------

def _entry(label, ptype, subtype, offset, size, flags=0):
    return (ENTRY_MAGIC + struct.pack("<BBII", ptype, subtype, offset, size)
            + label.encode("ascii").ljust(16, b"\x00") + struct.pack("<I", flags))


def _table(rows, with_md5=True, pad_to=0xC00):
    body = b"".join(_entry(*r) for r in rows)
    if with_md5:
        body += MD5_MAGIC + b"\xff" * 14 + hashlib.md5(body).digest()
    return body + b"\xff" * (pad_to - len(body))


CONFERENCE = [
    ("nvs", TYPE_DATA, 0x02, 0x9000, 0x5000),
    ("otadata", TYPE_DATA, 0x00, 0xE000, 0x2000),
    ("app0", TYPE_APP, 0x10, 0x10000, 0x300000),
    ("app1", TYPE_APP, 0x11, 0x310000, 0x300000),
    ("ffat", TYPE_DATA, 0x81, 0x610000, 0x9E0000),
    ("coredump", TYPE_DATA, 0x03, 0xFF0000, 0x10000),
]
FACTORY = [  # the M5Stack factory layout (never converted by initdex)
    ("nvs", TYPE_DATA, 0x02, 0x9000, 0x4000),
    ("otadata", TYPE_DATA, 0x00, 0xD000, 0x2000),
    ("phy_init", TYPE_DATA, 0x01, 0xF000, 0x1000),
    ("ota_0", TYPE_APP, 0x10, 0x20000, 0x4F0000),
    ("ota_1", TYPE_APP, 0x11, 0x510000, 0x4F0000),
    ("storage", TYPE_DATA, 0x81, 0xA00000, 0x400000),
    ("coredump", TYPE_DATA, 0x03, 0xE00000, 0x10000),
]


def self_test():
    def result(blob):
        try:
            entries, md5_ok = parse_table(blob)
        except LayoutError:
            return "unreadable"
        return "ok" if not check(entries, md5_ok) else "different"

    corrupt = bytearray(_table(CONFERENCE))
    corrupt[3 * ENTRY_SIZE + 4] ^= 0x01  # flip a bit in app1's offset; MD5 entry no longer matches
    small_app1 = [r if r[0] != "app1" else ("app1", TYPE_APP, 0x11, 0x310000, 0x200000) for r in CONFERENCE]
    no_otadata = [r for r in CONFERENCE if r[0] != "otadata"]
    overlap = CONFERENCE[:4] + [("spill", TYPE_DATA, 0x81, 0x600000, 0x20000)] + CONFERENCE[5:]
    cases = [
        ("conference layout with MD5", _table(CONFERENCE), "ok"),
        ("conference layout without MD5", _table(CONFERENCE, with_md5=False), "ok"),
        ("conference layout, full 4 KB sector read", _table(CONFERENCE, pad_to=0x1000), "ok"),
        ("M5Stack factory layout", _table(FACTORY), "different"),
        ("app1 too small", _table(small_app1), "different"),
        ("otadata missing", _table(no_otadata), "different"),
        ("extra partition overlapping app1", _table(overlap), "different"),
        ("MD5 mismatch (corrupt read)", bytes(corrupt), "different"),
        ("garbage bytes", b"\x00" * 0xC00, "unreadable"),
        ("truncated (no end marker)", _table(CONFERENCE, with_md5=False, pad_to=0)[:2 * ENTRY_SIZE], "unreadable"),
    ]
    failures = 0
    for name, blob, want in cases:
        got = result(blob)
        ok = got == want
        failures += not ok
        print("%s  %-42s expected %-10s got %s" % ("PASS" if ok else "FAIL", name, want, got))
    print("self-test: %d/%d passed" % (len(cases) - failures, len(cases)))
    return 0 if failures == 0 else 1


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2 or argv[1].startswith("-"):
        print(__doc__.strip())
        return 2
    return check_file(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
