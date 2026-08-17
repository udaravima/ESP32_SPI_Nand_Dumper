import os, sys, json
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import verify_dump as vd


def test_choose_page_sources_prefers_first_crc_good():
    # 3 dumps, 4 pages. bad sets: d0 fails page 1; d1 fails pages 1&2; d2 fails page 3.
    bad = [{1}, {1, 2}, {3}]
    src = vd.choose_page_sources(4, bad)
    assert src[0] == 0          # all good -> first
    assert src[1] == 2          # d0,d1 bad -> d2
    assert src[2] == 0          # d0 good
    assert src[3] == 0          # d0 good


def test_choose_page_sources_all_bad_is_none():
    # page 0 flagged bad in every dump -> None; page 1 good everywhere -> dump 0.
    assert vd.choose_page_sources(2, [{0}, {0}, {0}]) == [None, 0]


def test_majority_bytes_clear_winner():
    a, b, c = b"\x01\x02", b"\x01\xFF", b"\x01\x02"
    assert vd.majority_bytes([a, b, c]) == b"\x01\x02"


def test_majority_bytes_tie_takes_first():
    # byte 0: 0xAA vs 0xBB, 1-1 tie -> first variant's 0xAA wins.
    assert vd.majority_bytes([b"\xAA", b"\xBB"]) == b"\xAA"


def test_read_badpages_set(tmp_path):
    p = tmp_path / "d.bin"
    (tmp_path / "d.bin.badpages.json").write_text(json.dumps({"bad_pages": [3, 7, 9]}))
    assert vd.read_badpages_set(str(p)) == {3, 7, 9}
    assert vd.read_badpages_set(str(tmp_path / "missing.bin")) == set()


def test_repair_prefers_crc_good_copy(tmp_path):
    # 2 dumps, 2 pages of 4 bytes. Page 1 is garbage in d0 but flagged bad there,
    # and good in d1 -> repaired image must take d1's page 1.
    ps = 4
    good_p0 = b"AAAA"
    good_p1 = b"BBBB"
    d0 = tmp_path / "d0.bin"; d0.write_bytes(good_p0 + b"XXXX")   # p1 corrupt
    d1 = tmp_path / "d1.bin"; d1.write_bytes(good_p0 + good_p1)
    (tmp_path / "d0.bin.badpages.json").write_text(json.dumps({"bad_pages": [1]}))
    (tmp_path / "d1.bin.badpages.json").write_text(json.dumps({"bad_pages": []}))
    out = tmp_path / "fixed.bin"
    good, maj = vd.repair([str(d0), str(d1)], str(out), ps, 2)
    assert out.read_bytes() == good_p0 + good_p1
    assert (good, maj) == (2, 0)


def test_repair_majority_when_all_flagged(tmp_path):
    ps = 4
    d0 = tmp_path / "d0.bin"; d0.write_bytes(b"AAAA")
    d1 = tmp_path / "d1.bin"; d1.write_bytes(b"AAAA")
    d2 = tmp_path / "d2.bin"; d2.write_bytes(b"ZZZZ")
    for n in ("d0", "d1", "d2"):
        (tmp_path / f"{n}.bin.badpages.json").write_text(json.dumps({"bad_pages": [0]}))
    out = tmp_path / "fixed.bin"
    good, maj = vd.repair([str(d0), str(d1), str(d2)], str(out), ps, 1)
    assert out.read_bytes() == b"AAAA"   # 2 of 3 agree
    assert (good, maj) == (0, 1)
