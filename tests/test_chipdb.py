import pytest
import chipdb
import struct

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

def test_scheme_expands_to_16_entries_uncor_default():
    shift, mask, m = chipdb.expand_scheme("generic2")
    assert (shift, mask) == (4, 0x3)
    assert len(m) == 16
    assert m[2] == chipdb.UNCOR          # field 2 = uncorrectable
    assert all(v == chipdb.UNCOR for v in m[4:])  # unnamed => UNCOR

def test_validate_accepts_both_shipped_chips():
    db = chipdb.load()
    for name in db.chips:
        chipdb.validate(chipdb.get(db, name))  # must not raise

def test_validate_rejects_spare_ge_page():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["spare_size"] = c["geometry"]["page_size"]
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_non_power_of_two_ppb():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["pages_per_block"] = 63
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_capacity_out_of_window():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["total_blocks"] = 1        # ~2 Mb, below 512 Mb floor
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_overlong_name():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["name"] = "X" * 24                      # 24 chars, no room for NUL in name[24]
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_too_many_oob_regions():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["profile"]["oob_layout"]["free_regions"] = [[0, 1]] * 5   # > 4 would truncate
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_zero_total_blocks():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["total_blocks"] = 0
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_bbm_beyond_spare():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["profile"]["oob_layout"]["bbm"]["offset"] = c["geometry"]["spare_size"]
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_too_many_ecc_regions():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["profile"]["oob_layout"]["ecc_regions"] = [[0, 1]] * 5
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_pack_is_110_bytes():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "DS35Q1GA"))
    blob = chipdb.pack(flat)
    assert len(blob) == 110 == chipdb.PROFILE_SIZE

def test_pack_ds35_known_fields():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "DS35Q1GA"))
    assert flat["name"] == "DS35Q1GA"
    assert flat["page_size"] == 2112
    assert flat["ecc_shift"] == 4 and flat["ecc_mask"] == 0x3
    assert flat["ecc_map"][2] == chipdb.UNCOR
    assert flat["qe_addr"] == 0xB0 and flat["qe_bit"] == 0x01
    assert flat["bbm_off"] == 0 and flat["bbm_good"] == 0xFF
    # name round-trips through the fixed 24-byte field, NUL-padded
    blob = chipdb.pack(flat)
    assert blob[:24] == b"DS35Q1GA".ljust(24, b"\x00")

def test_micron_has_no_qe():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "MT29F2G01ABAGD"))
    assert flat["qe_addr"] == 0 and flat["qe_bit"] == 0
