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


# ---- profile-aware bad-block markers -----------------------------------------------
import json  # noqa: E402

import pytest  # noqa: E402


def _dump_with_marker(path, page_size, spare_size, ppb, blocks, marks):
    """marks: {(block, page): {spare_offset: byte}} written into the spare."""
    main = page_size - spare_size
    with open(path, "wb") as f:
        for b in range(blocks):
            for p in range(ppb):
                page = bytearray(bytes([b & 0x7F]) * main + b"\xFF" * spare_size)
                for off, v in marks.get((b, p), {}).items():
                    page[main + off] = v
                f.write(page)


def test_two_byte_marker_second_byte_counts(tmp_path):
    # DS35: 2-byte marker. A block whose spare[1] alone is 0x00 is bad; the
    # classic 1-byte check at spare[0] would miss it.
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 4, 3, {(1, 0): {1: 0x00}})
    assert ecc_stripper.strip(inp, out, 2112, 64, 4) == []
    bbm = ecc_stripper.bbm_from_flat(ecc_stripper.load_profile("DS35Q1GA"))
    assert bbm == {"offset": 0, "length": 2, "good": 0xFF, "pages": ("first",)}
    assert ecc_stripper.strip(inp, out, 2112, 64, 4, bbm) == [1]


def test_marker_on_last_page_pads_the_whole_block(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 4, 3, {(2, 3): {5: 0x12}})
    bbm = {"offset": 5, "length": 1, "good": 0xFF, "pages": ("last",)}
    assert ecc_stripper.strip(inp, out, 2112, 64, 4, bbm) == [2]
    data = open(out, "rb").read()
    blk = 4 * 2048
    assert data[:blk] == b"\x00" * blk                  # block 0 kept
    assert data[2 * blk:3 * blk] == b"\xFF" * blk        # block 2, every page padded


def test_inverted_polarity_marker(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    # Every spare byte is 0xFF; a profile whose good value is 0x00 calls all bad.
    _dump_with_marker(inp, 2112, 64, 4, 2, {(1, 0): {0: 0x00}})
    bbm = {"offset": 0, "length": 1, "good": 0x00, "pages": ("first",)}
    assert ecc_stripper.strip(inp, out, 2112, 64, 4, bbm) == [0]


def test_marker_outside_spare_is_rejected(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 4, 1, {})
    with pytest.raises(ValueError):
        ecc_stripper.strip(inp, out, 2112, 64, 4,
                           {"offset": 63, "length": 2, "good": 0xFF, "pages": ("first",)})


def test_partial_last_block_is_kept(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 4, 2, {})
    with open(inp, "r+b") as f:
        f.truncate(6 * 2112)                             # a truncated dump
    assert ecc_stripper.strip(inp, out, 2112, 64, 4) == []
    assert os.path.getsize(out) == 6 * 2048


def _write_meta(path, profile, ecc_on=False):
    g = dict(page_size=2112, spare_size=64, pages_per_block=4, total_blocks=2,
             total_pages=8, total_bytes=8 * 2112, mfr_id=0xE5, dev_id=0x71,
             page_addr_bits=2, flags=0)
    meta = {"geometry": g, "ecc_on": ecc_on}
    if profile:
        meta["profile"] = profile
    with open(path, "w") as f:
        json.dump(meta, f)


def test_main_uses_profile_recorded_in_meta(tmp_path, capsys):
    inp, out, meta = (str(tmp_path / n) for n in ("raw.bin", "clean.bin", "raw.meta.json"))
    _dump_with_marker(inp, 2112, 64, 4, 2, {(1, 0): {1: 0x00}})
    _write_meta(meta, {"name": "DS35Q1GA", "source": "resident"})
    ecc_stripper.main([inp, out, "--meta", meta])
    printed = capsys.readouterr().out
    assert "profile DS35Q1GA" in printed and "1 bad block(s): [1]" in printed


def test_main_unknown_profile_in_meta_falls_back(tmp_path, capsys):
    inp, out, meta = (str(tmp_path / n) for n in ("raw.bin", "clean.bin", "raw.meta.json"))
    _dump_with_marker(inp, 2112, 64, 4, 2, {})
    _write_meta(meta, {"name": "MANUAL", "source": "unknown"}, ecc_on=True)
    ecc_stripper.main([inp, out, "--meta", meta])
    printed = capsys.readouterr().out
    assert "not in db/" in printed and "(default)" in printed and "ECC on" in printed


def test_main_profile_supplies_geometry(tmp_path, capsys):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 64, 1, {})
    ecc_stripper.main([inp, out, "--profile", "DS35Q1GA"])
    assert os.path.getsize(out) == 64 * 2048


def test_main_unknown_profile_flag_fails(tmp_path):
    inp, out = str(tmp_path / "raw.bin"), str(tmp_path / "clean.bin")
    _dump_with_marker(inp, 2112, 64, 4, 1, {})
    with pytest.raises(SystemExit):
        ecc_stripper.main([inp, out, "--profile", "NOPE"])
