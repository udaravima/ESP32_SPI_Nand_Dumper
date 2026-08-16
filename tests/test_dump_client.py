import io, json, os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.test_dump_header import build_header
import dump


class FakeSocket:
    """Serves a fixed byte buffer through recv(), like a socket draining."""
    def __init__(self, data):
        self.buf = data
        self.pos = 0

    def recv(self, n):
        chunk = self.buf[self.pos:self.pos + n]
        self.pos += len(chunk)
        return chunk


def _frame(data):
    """One proto-v2 page record: data followed by its little-endian CRC32."""
    return data + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)


def test_parse_header_reads_geometry():
    buf = build_header(page_size=2176, spare_size=128, pages_per_block=64,
                       total_blocks=2048, total_pages=2048*64,
                       total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
                       page_addr_bits=6, flags=0)
    g = dump.parse_header(buf)
    assert g["total_bytes"] == 2048 * 64 * 2176
    assert g["page_size"] == 2176


def test_receive_pages_v2_all_good():
    page_size = 8
    p0 = bytes(range(0, 8))
    p1 = bytes(range(8, 16))
    stream = _frame(p0) + _frame(p1)
    geom = dict(page_size=page_size, total_pages=2, total_bytes=16, flags=dump.FLAG_PAGECRC)
    fout = io.BytesIO()
    got = dump.receive_pages(FakeSocket(stream), geom, fout)
    assert got.bad_pages == []
    assert got.pages_received == 2
    assert got.bytes_received == 16
    assert fout.getvalue() == p0 + p1        # CRC tails stripped, only data written


def test_receive_pages_v2_flags_corrupt_page():
    page_size = 8
    p0 = bytes(range(0, 8))
    p1 = bytes(range(8, 16))
    stream = bytearray(_frame(p0) + _frame(p1))
    stream[page_size + 4] ^= 0xFF            # flip first data byte of page 1 (after its CRC)
    geom = dict(page_size=page_size, total_pages=2, total_bytes=16, flags=dump.FLAG_PAGECRC)
    fout = io.BytesIO()
    got = dump.receive_pages(FakeSocket(bytes(stream)), geom, fout)
    assert got.bad_pages == [1]              # page 1 fails its seal
    assert got.pages_received == 2
    out = fout.getvalue()
    assert out[:8] == p0                      # page 0 intact
    assert out[8] == (8 ^ 0xFF)               # corrupted byte still written for majority-vote


def test_receive_pages_v2_truncated_stream():
    page_size = 8
    p0 = bytes(range(0, 8))
    stream = _frame(p0) + _frame(bytes(range(8, 16)))[:5]   # page 1 cut off mid-record
    geom = dict(page_size=page_size, total_pages=2, total_bytes=16, flags=dump.FLAG_PAGECRC)
    fout = io.BytesIO()
    got = dump.receive_pages(FakeSocket(stream), geom, fout)
    assert got.pages_received == 1            # only the whole page landed
    assert got.truncated is True
    assert fout.getvalue() == p0


def test_write_metadata_roundtrip(tmp_path):
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    p = tmp_path / "d.bin"
    dump.write_metadata(str(p), g, byte_count=g["total_bytes"])
    meta = json.load(open(str(p) + ".meta.json"))
    assert meta["geometry"]["page_size"] == 2176
    assert meta["bytes_received"] == g["total_bytes"]
