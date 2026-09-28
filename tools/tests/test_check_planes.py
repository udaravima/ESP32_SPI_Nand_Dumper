import json
from tools.check_planes import classify_odd_blocks, main

PS, PPB = 16, 4


def _page(tag):
    return bytes([tag]) * PS


def _block(tags):
    return b"".join(_page(t) for t in tags)


def test_mirrored_odd_block_is_affected():
    # block 0 ends in page 0x13; buggy block 1 repeats it for every page.
    data = _block([0x10, 0x11, 0x12, 0x13]) + _block([0x13] * PPB)
    assert classify_odd_blocks(data, PS, PPB) == ([1], [], [])


def test_independent_odd_block_is_clean():
    data = _block([0x10, 0x11, 0x12, 0x13]) + _block([0x20, 0x21, 0x22, 0x23])
    assert classify_odd_blocks(data, PS, PPB) == ([], [1], [])


def test_erased_pair_is_inconclusive():
    data = _block([0x10, 0x11, 0x12, 0xFF]) + _block([0xFF] * PPB)
    assert classify_odd_blocks(data, PS, PPB) == ([], [], [1])


def test_main_reads_meta_and_flags_affected(tmp_path):
    dump = tmp_path / "d.bin"
    dump.write_bytes(_block([1, 2, 3, 4]) + _block([4] * PPB)
                     + _block([5, 6, 7, 8]) + _block([9, 10, 11, 12]))
    (tmp_path / "d.bin.meta.json").write_text(
        json.dumps({"geometry": {"page_size": PS, "pages_per_block": PPB}}))
    assert main([str(dump)]) == 1


def test_main_without_geometry_is_usage_error(tmp_path):
    dump = tmp_path / "d.bin"
    dump.write_bytes(b"\x00" * PS)
    assert main([str(dump)]) == 2
