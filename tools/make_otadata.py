"""Write an 8 KiB ESP-IDF otadata image that boots app slot ota_<n>.

Entry layout (esp_ota_select_entry_t): ota_seq u32, seq_label 20 bytes, ota_state u32,
crc u32 = crc32(ota_seq, init 0xFFFFFFFF). Boot slot = (ota_seq - 1) % 2.
Checked against Arduino's boot_app0.bin (seq 1 -> crc 0x4743989A).
Usage: python make_otadata.py OUT.bin [slot]   (slot 1 = app1, the default)
"""
import struct
import sys
import zlib


def entry(seq):
    crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    return struct.pack("<I20sII", seq, b"\xff" * 20, 0xFFFFFFFF, crc)


def image(slot):
    first = entry(slot + 1)
    return first + b"\xff" * (0x1000 - len(first)) + b"\xff" * 0x1000


if __name__ == "__main__":
    assert entry(1)[28:32] == bytes.fromhex("9a984347"), "crc convention drifted"
    slot = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    data = image(slot)
    with open(sys.argv[1], "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes, boots ota_{slot}, seq {slot + 1}")
