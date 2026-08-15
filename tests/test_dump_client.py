import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.test_dump_header import build_header
import dump


def test_parse_header_reads_geometry():
    buf = build_header(page_size=2176, spare_size=128, pages_per_block=64,
                       total_blocks=2048, total_pages=2048*64,
                       total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
                       page_addr_bits=6, flags=0)
    g = dump.parse_header(buf)
    assert g["total_bytes"] == 2048 * 64 * 2176
    assert g["page_size"] == 2176


def test_write_metadata_roundtrip(tmp_path):
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    p = tmp_path / "d.bin"
    dump.write_metadata(str(p), g, byte_count=g["total_bytes"])
    meta = json.load(open(str(p) + ".meta.json"))
    assert meta["geometry"]["page_size"] == 2176
    assert meta["bytes_received"] == g["total_bytes"]
