import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "tools"))
import binary_compare_fix as bcf


def test_majority_vote_fixes_minority(tmp_path):
    a = tmp_path / "a.bin"; b = tmp_path / "b.bin"; c = tmp_path / "c.bin"
    a.write_bytes(b"\x01\x02\x03")
    b.write_bytes(b"\x01\xFF\x03")   # middle byte corrupted in one file
    c.write_bytes(b"\x01\x02\x03")
    out = tmp_path / "fixed.bin"; rep = tmp_path / "rep.txt"
    bcf.compare_and_fix([str(a), str(b), str(c)], str(out), str(rep))
    assert out.read_bytes() == b"\x01\x02\x03"
