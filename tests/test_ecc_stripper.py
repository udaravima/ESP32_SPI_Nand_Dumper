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


def test_resolve_bbm_from_meta_ds35():
    """Test that resolve_bbm fetches DS35 profile from chipdb via meta."""
    meta = {"geometry": {"mfr_id": 0xE5, "dev_id": 0x71,
                         "page_size": 2112, "spare_size": 64, "pages_per_block": 64}}
    bbm = ecc_stripper.resolve_bbm(meta)
    assert bbm is not None
    assert bbm["offset"] == 0 and bbm["good"] == 0xFF


def test_resolve_bbm_unknown_chip_is_none():
    """Test that resolve_bbm returns None for unknown chip."""
    meta = {"geometry": {"mfr_id": 0x00, "dev_id": 0x00}}
    assert ecc_stripper.resolve_bbm(meta) is None


def test_strip_flags_bad_block_at_profile_offset(tmp_path):
    """Test that strip() uses custom bbm offset to detect bad blocks."""
    # 4 bytes main + 4 bytes spare, 2 pages/block, 2 blocks. bbm offset 1 in spare.
    ps, ss, ppb = 8, 4, 2
    good, offbyte = 0xFF, 1
    page_ok  = b"\xAA\xAA\xAA\xAA" + b"\xFF\xFF\xFF\xFF"
    page_bad = b"\xBB\xBB\xBB\xBB" + b"\xFF\x00\xFF\xFF"  # spare[offset=1] != good
    raw = tmp_path / "raw.bin"
    raw.write_bytes(page_ok + page_ok + page_bad + page_ok)     # block1 page0 marks bad
    out = tmp_path / "clean.bin"
    bad = ecc_stripper.strip(str(raw), str(out), ps, ss, ppb,
                             bbm={"offset": offbyte, "len": 1, "good": good, "pages": ["first"]})
    assert bad == [1]                       # block 1 flagged bad
    assert out.read_bytes()[8:12] == b"\xFF\xFF\xFF\xFF"   # block1 main replaced with 0xFF pad
