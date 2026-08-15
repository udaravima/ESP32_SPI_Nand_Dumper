import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import ecc_stripper


def _make_dump(path, page_size, spare_size, pages_per_block, blocks, bad_blocks):
    main = page_size - spare_size
    with open(path, "wb") as f:
        for b in range(blocks):
            for p in range(pages_per_block):
                page = bytearray(b"\xAA" * main + b"\xFF" * spare_size)
                if p == 0 and b in bad_blocks:
                    page[main] = 0x00  # bad-block marker at spare[0]
                f.write(page)


def test_strip_sizes_and_detects_bad(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _make_dump(inp, 2176, 128, 64, blocks=4, bad_blocks={2})
    bad = ecc_stripper.strip(inp, out, 2176, 128, 64)
    assert bad == [2]
    assert os.path.getsize(out) == 4 * 64 * 2048       # main-only
    data = open(out, "rb").read()
    blk2 = data[2*64*2048:(2*64+1)*2048]               # block 2 first page
    assert blk2 == b"\xFF" * 2048                       # padded


def test_strip_2112_geometry(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _make_dump(inp, 2112, 64, 64, blocks=2, bad_blocks=set())
    ecc_stripper.strip(inp, out, 2112, 64, 64)
    assert os.path.getsize(out) == 2 * 64 * 2048
