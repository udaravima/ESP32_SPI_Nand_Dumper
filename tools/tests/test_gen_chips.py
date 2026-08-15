import pytest
from tools.gen_chips import validate, render_header

BASE = {
    "mfr_id": 0x2C, "dev_id": 0x24, "page_size": 2176, "spare_size": 128,
    "pages_per_block": 64, "total_blocks": 2048, "bad_block_mark": 0x00,
    "has_qe_bit": False, "ecc_default": "off", "vcc_mv": 3300,
}


def test_validate_accepts_good_chip():
    validate({"MT29F2G01ABAGD": dict(BASE)})  # no raise


def test_validate_rejects_spare_ge_page():
    bad = dict(BASE); bad["spare_size"] = 2176
    with pytest.raises(ValueError, match="spare_size"):
        validate({"X": bad})


def test_validate_rejects_non_power_of_two_ppb():
    bad = dict(BASE); bad["pages_per_block"] = 60
    with pytest.raises(ValueError, match="pages_per_block"):
        validate({"X": bad})


def test_validate_rejects_duplicate_ids():
    with pytest.raises(ValueError, match="duplicate"):
        validate({"A": dict(BASE), "B": dict(BASE)})


def test_validate_requires_qe_fields_when_has_qe_bit():
    q = dict(BASE); q["has_qe_bit"] = True
    with pytest.raises(ValueError, match="qe_feature_addr"):
        validate({"X": q})


def test_render_emits_row_with_derived_page_addr_bits():
    out = render_header({"MT29F2G01ABAGD": dict(BASE)})
    # page_addr_bits = log2(64) = 6, ecc off -> false
    assert '{ "MT29F2G01ABAGD", 0x2C, 0x24, 2176, 128, 64, 2048, 6,' in out
    assert "false" in out and "3300" in out
    assert "CHIPS_COUNT" in out
