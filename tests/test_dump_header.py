import struct, zlib
import pytest

FMT = "<6sBBHHHHIBBBBII"


def parse_header(buf):
    assert len(buf) == 32
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, page_addr_bits, _res, total_bytes, crc) = struct.unpack(FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver == 1, "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=page_addr_bits, flags=flags,
                total_bytes=total_bytes)


def build_header(**g):
    body = struct.pack(FMT[:-1], b"NANDMP", 1, g["flags"], g["page_size"],
                       g["spare_size"], g["pages_per_block"], g["total_blocks"],
                       g["total_pages"], g["mfr_id"], g["dev_id"],
                       g["page_addr_bits"], 0, g["total_bytes"])
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def test_roundtrip_micron():
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    assert parse_header(build_header(**g)) == g


def test_golden_bytes_micron():
    # Exact 32-byte wire image, verified equal to the C dump_header_pack() output.
    # Locks the format so C and Python cannot silently diverge (e.g. a wrong CRC poly).
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    assert build_header(**g).hex() == \
        "4e414e444d5001008008800040000008000002002c2406000000001143019d5d"


def test_rejects_bad_crc():
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    buf = bytearray(build_header(**g)); buf[8] ^= 0xFF
    with pytest.raises(AssertionError, match="crc"):
        parse_header(bytes(buf))
