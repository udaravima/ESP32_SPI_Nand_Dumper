import copy
import os

import pytest
import yaml

from tools import chipdb as cdb
from tools.chipdb import OK, CORR, CORR_REFRESH, UNCOR

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


@pytest.fixture(scope="module")
def db():
    return cdb.load_db()


def _decode(scheme, status):
    shift, mask, ecc_map = cdb.expand_scheme({"scheme": scheme})
    return ecc_map[(status >> shift) & mask]


# ---- the shipped DB -------------------------------------------------------------
def test_shipped_db_validates_without_warnings(db):
    flats, warnings = cdb.validate_db(db)
    assert set(flats) == {"MT29F2G01ABAGD", "DS35Q1GA"}
    assert warnings == []


def test_db_agrees_with_chips_yml_until_stage2(db):
    # The firmware still builds from chips.yml; the two must not drift.
    with open(os.path.join(REPO, "chips.yml")) as fh:
        legacy = yaml.safe_load(fh)["chips"]
    for name, old in legacy.items():
        f = cdb.flatten(db, name)
        assert (f["id_mfr"], f["id_dev"]) == (old["mfr_id"], old["dev_id"])
        for k in ("page_size", "spare_size", "pages_per_block", "total_blocks"):
            assert f[k] == old[k], (name, k)
        assert f["planes"] == old.get("planes", 1)
        assert f["vcc_mv"] == old["vcc_mv"]


def test_dosilicon_fixes_the_three_ds35_defects(db):
    f = cdb.flatten(db, "DS35Q1GA")
    assert (f["ecc_shift"], f["ecc_mask"]) == (4, 0x3)          # SR[5:4], not [6:4]
    assert (f["qe_addr"], f["qe_bit"]) == (0xB0, 0x01)           # QE at B0[0]
    assert (f["bbm_off"], f["bbm_len"]) == (0, 2)                # profile-driven BBM
    assert f["oob_free_n"] == 4 and f["oob_ecc_n"] == 4


def test_micron_resolves_as_today(db):
    f = cdb.flatten(db, "MT29F2G01ABAGD")
    assert (f["ecc_shift"], f["ecc_mask"]) == (4, 0x7)
    assert f["qe_addr"] == 0 and f["planes"] == 2
    assert f["op_read_cache"] == 0x0B and f["op_read_cache_x4"] == 0x6B
    assert f["ecc_en_bit"] == 4


# ---- ECC decoder matrix (design section 9) --------------------------------------
@pytest.mark.parametrize("scheme,status,want", [
    ("generic2", 0x00, OK), ("generic2", 0x10, CORR), ("generic2", 0x20, UNCOR),
    ("generic2", 0x30, UNCOR),                        # reserved -> conservative
    ("micron3", 0x10, CORR), ("micron3", 0x20, UNCOR), ("micron3", 0x30, CORR_REFRESH),
    ("micron3", 0x50, CORR_REFRESH), ("micron3", 0x40, UNCOR),
    ("gd_uc", 0x60, CORR), ("gd_uc", 0x70, UNCOR),
    ("xtx4", 0x80, CORR), ("xtx4", 0xF0, UNCOR),      # index 15 of ecc_map
    ("xtx_g0xa", 0x20, UNCOR), ("xtx_g0xa", 0x30, CORR_REFRESH), ("xtx_g0xa", 0x1C, CORR),
    ("generic2", 0x31, UNCOR),                        # OIP bit ignored by the mask
])
def test_ecc_scheme_matrix(scheme, status, want):
    assert _decode(scheme, status) == want


def test_every_scheme_fills_all_16_entries():
    for name in cdb.SCHEMES:
        _, _, m = cdb.expand_scheme({"scheme": name})
        assert len(m) == 16 and set(m) <= {OK, CORR, CORR_REFRESH, UNCOR}


def test_explicit_value_map_unnamed_values_are_uncorrectable():
    _, _, m = cdb.expand_scheme({"status_shift": 4, "status_mask": 0x3,
                                 "value_map": {0: OK, 1: CORR}})
    assert m[:4] == [OK, CORR, UNCOR, UNCOR] and m[15] == UNCOR


# ---- validation rejects what the device would --------------------------------------
def _mutate(db, name, fn):
    d = copy.deepcopy(db)
    fn(d["chips"][name], d)
    return d


@pytest.mark.parametrize("mutate,match", [
    (lambda c, d: c.update(profile="nope"), "unknown profile"),
    (lambda c, d: c.update(family="nope"), "unknown family"),
    (lambda c, d: c.pop("datasheet"), "datasheet"),
    (lambda c, d: c["geometry"].update(page_size=9000), "MAX_PAGE_BUFFER"),
    (lambda c, d: c["geometry"].update(pages_per_block=48), "power of two"),
    (lambda c, d: c["geometry"].update(spare_size=4000), "spare_size"),
    (lambda c, d: c["geometry"].update(planes=3), "planes"),
    (lambda c, d: d["profiles"]["micron"]["oob_layout"]["ecc_regions"].append([120, 16]),
     "outside"),
    (lambda c, d: d["profiles"]["micron"]["oob_layout"]["bbm"].update(offset=127, length=2),
     "marker"),
    (lambda c, d: d["profiles"]["micron"]["ecc"].update(scheme="bogus"), "scheme"),
])
def test_validation_rejects(db, mutate, match):
    bad = _mutate(db, "MT29F2G01ABAGD", mutate)
    with pytest.raises(cdb.ChipDBError, match=match):
        cdb.validate_db(bad)


def test_overlong_name_rejected(db):
    d = copy.deepcopy(db)
    d["chips"]["X" * 24] = d["chips"].pop("DS35Q1GA")
    with pytest.raises(cdb.ChipDBError, match="name longer"):
        cdb.validate_db(d)


def test_implausible_capacity_is_a_warning_not_an_error(db):
    d = _mutate(db, "DS35Q1GA", lambda c, _: c["geometry"].update(total_blocks=64))
    _, warnings = cdb.validate_db(d)
    assert any("capacity" in w for w in warnings)


# ---- ID disambiguation (design section 5) ------------------------------------------
@pytest.fixture
def twin_db(db):
    d = copy.deepcopy(db)
    a = copy.deepcopy(d["chips"]["DS35Q1GA"]); a["id"]["dev2"] = 0x01
    b = copy.deepcopy(d["chips"]["DS35Q1GA"]); b["id"]["dev2"] = 0x02
    d["chips"]["TWIN_A"], d["chips"]["TWIN_B"] = a, b
    del d["chips"]["DS35Q1GA"]
    return d


def test_identify_unique_id(db):
    assert cdb.identify(db, 0x2C, 0x24)["name"] == "MT29F2G01ABAGD"


def test_identify_dev2_breaks_tie(twin_db):
    assert cdb.identify(twin_db, 0xE5, 0x71, dev2=0x02)["name"] == "TWIN_B"


def test_identify_never_guesses(twin_db):
    with pytest.raises(cdb.AmbiguousId) as e:
        cdb.identify(twin_db, 0xE5, 0x71)
    assert {c["name"] for c in e.value.candidates} == {"TWIN_A", "TWIN_B"}


def test_identify_honors_cached_choice(twin_db):
    assert cdb.identify(twin_db, 0xE5, 0x71, cached="TWIN_A")["name"] == "TWIN_A"


def test_identify_unknown_id(db):
    with pytest.raises(cdb.ChipDBError, match="no chip"):
        cdb.identify(db, 0x00, 0x00)


# ---- flat struct + push blob --------------------------------------------------------
def test_layout_is_naturally_aligned_for_c():
    # Each field at a multiple of its element size => a plain C struct with the
    # same field order has the same layout (no __attribute__((packed)) needed).
    for field, off, size in cdb.field_offsets():
        elem = {"name": 1, "ecc_map": 1, "oob_free": 2, "oob_ecc": 2}.get(field, size)
        assert off % elem == 0, (field, off)
    assert cdb.STRUCT_SIZE % 4 == 0


def test_blob_roundtrip(db):
    f = cdb.flatten(db, "DS35Q1GA")
    got = cdb.unpack_blob(cdb.pack_blob(f))
    for k, v in got.items():
        assert v == f[k], k


def test_blob_golden_ds35(db):
    # Locks the wire layout; stage 2's native C test unpacks these same bytes.
    blob = cdb.pack_blob(cdb.flatten(db, "DS35Q1GA"))
    assert len(blob) == 6 + cdb.STRUCT_SIZE + 4
    assert blob[:6] == b"PRF\x01" + cdb.STRUCT_SIZE.to_bytes(2, "little")
    assert blob[6:14] == b"DS35Q1GA"
    body = blob[6:]
    assert body[24:28] == bytes([0xE5, 0x71, 0x00, 0x00])        # id, no dev2
    assert int.from_bytes(body[28:32], "little") == 2112          # page_size


@pytest.mark.parametrize("corrupt,code", [
    (lambda b: b"XXX" + b[3:], "E_BAD_MAGIC"),
    (lambda b: b[:3] + b"\x09" + b[4:], "E_SCHEMA_VER"),
    (lambda b: b[:10] + bytes([b[10] ^ 0xFF]) + b[11:], "E_BAD_CRC"),   # flip a body byte
    (lambda b: b[:-1], "E_BAD_LEN"),
])
def test_blob_transport_checks_fail_closed(db, corrupt, code):
    blob = cdb.pack_blob(cdb.flatten(db, "MT29F2G01ABAGD"))
    with pytest.raises(cdb.ChipDBError, match=code):
        cdb.unpack_blob(corrupt(blob))


def test_cli_lists_db(capsys):
    assert cdb.main([]) == 0
    assert "2 chips OK" in capsys.readouterr().out
