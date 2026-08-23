import pytest
import chipdb

def test_load_resolves_two_chips():
    db = chipdb.load()
    assert set(db.chips) == {"MT29F2G01ABAGD", "DS35Q1GA"}
    ds = chipdb.get(db, "DS35Q1GA")
    assert ds["id"]["mfr"] == 0xE5 and ds["id"]["dev"] == 0x71
    assert ds["profile"]["ecc"]["scheme"] == "generic2"
    assert ds["family"]["feature_addrs"]["config"] == 0xB0

def test_dangling_profile_ref_raises(tmp_path):
    (tmp_path / "families").mkdir(); (tmp_path / "profiles").mkdir(); (tmp_path / "chips").mkdir()
    (tmp_path / "families" / "spi-nand.yml").write_text("name: spi-nand\n")
    (tmp_path / "chips" / "X.yml").write_text(
        "name: X\nfamily: spi-nand\nprofile: nope\nid: {mfr: 1, dev: 2}\n")
    with pytest.raises(chipdb.RefError):
        chipdb.resolve_refs(chipdb.load(root=str(tmp_path)))
