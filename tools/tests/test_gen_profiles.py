import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import chipdb, gen_profiles

def test_resident_header_has_both_chips():
    db = chipdb.load()
    hdr = gen_profiles.render_resident(db)
    assert "PROFILES_COUNT" in hdr
    assert hdr.count("{") >= 2          # two initializers
    assert "DS35Q1GA" in hdr and "MT29F2G01ABAGD" in hdr

def test_golden_blob_is_110_bytes():
    db = chipdb.load()
    hdr = gen_profiles.render_golden(db, "DS35Q1GA")
    assert "GOLDEN_DS35Q1GA_BLOB" in hdr
    # exactly 110 comma-separated byte literals
    body = hdr[hdr.index("{") + 1: hdr.index("}")]
    assert len([b for b in body.split(",") if b.strip()]) == 110

def test_resident_header_has_id_table():
    db = chipdb.load()
    hdr = gen_profiles.render_resident(db)
    assert "PROFILE_IDS" in hdr
    # DS35 (0xE5,0x71) and MT29F (0x2C,0x24) ids present as byte literals
    assert "0xE5" in hdr and "0x71" in hdr
    assert "0x2C" in hdr and "0x24" in hdr
