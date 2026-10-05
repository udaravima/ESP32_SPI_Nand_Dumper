"""Serial EEPROM entries in the chip DB (db/chips/eeprom/, chipdb.py)."""
import pytest

from tools import chipdb as cdb


@pytest.fixture(scope="module")
def db():
    return cdb.load_db()


def test_every_eeprom_flattens_to_a_valid_profile(db):
    flats, warnings = cdb.validate_db(db)
    ee = {n: f for n, f in flats.items() if f["family"] in cdb.EEPROM_FAMILIES}
    assert len([f for f in ee.values() if f["family"] == "i2c-eeprom"]) >= 13
    assert len([f for f in ee.values() if f["family"] == "spi-eeprom"]) >= 16
    for n, f in ee.items():
        size = f["page_size"] * f["pages_per_block"] * f["total_blocks"]
        assert size == f["size_bytes"], n
        # The address and its carried bits reach exactly the whole part.
        assert 1 << (8 * f["addr_bytes"] + f["dev_addr_bits"]) >= size, n
        assert f["id_n_bytes"] == 0 and f["read_mode"] == 0, n
        assert db["chips"][n].get("source") and db["chips"][n].get("datasheet"), n
    assert warnings == []


@pytest.mark.parametrize("name,size,ab,bits,shift", [
    ("24C01", 128, 1, 0, 0), ("24C04", 512, 1, 1, 0), ("24C16", 2048, 1, 3, 0),
    ("24C32", 4096, 2, 0, 0), ("24CM01", 131072, 2, 1, 0), ("24xx1025", 131072, 2, 1, 2),
    ("24CM02", 262144, 2, 2, 0), ("25xx040", 512, 1, 1, 3), ("25xx080", 1024, 2, 0, 0),
    ("25xx1024", 131072, 3, 0, 0), ("M95M04", 524288, 3, 0, 0),
])
def test_addressing_matches_the_parts(db, name, size, ab, bits, shift):
    f = cdb.flatten(db, name)
    assert (f["size_bytes"], f["addr_bytes"], f["dev_addr_bits"], f["dev_addr_shift"]) == \
        (size, ab, bits, shift)


def test_spi_eeprom_only_reads(db):
    f = cdb.flatten(db, "25xx256")
    assert (f["op_read_cache"], f["op_get_feat"], f["op_set_feat"], f["op_read_cache_x4"]) == \
        (0x03, 0x05, 0, 0)


def test_eeproms_never_match_an_id(db):
    # An empty bus reads 00 00 / FF FF: no EEPROM may come back from an ID lookup.
    for mfr, dev in ((0, 0), (0xFF, 0xFF)):
        assert all(c["family"] not in cdb.EEPROM_FAMILIES for c in cdb.candidates(db, mfr, dev))
    flats, _ = cdb.validate_db(db)
    assert all(k[0] not in cdb.EEPROM_FAMILIES for k in cdb.shared_ids(flats))


def test_aliases_resolve_case_insensitively(db):
    assert cdb.resolve_name(db, "AT24C256") == "24C256"
    assert cdb.resolve_name(db, "25lc040a") == "25xx040"
    assert cdb.resolve_name(db, "24C02") == "24C02"
    with pytest.raises(cdb.ChipDBError, match="unknown chip"):
        cdb.resolve_name(db, "24C9999")


def test_i2c_base_mirrors_the_firmware():
    f = {"dev_addr_bits": 0, "dev_addr_shift": 0}
    assert cdb.eeprom_i2c_base(f, 0x01) == 0x50
    assert cdb.eeprom_i2c_base(f, 0x80) == 0x57
    assert cdb.eeprom_i2c_base(f, 0) is None
    f = {"dev_addr_bits": 3, "dev_addr_shift": 0}               # 24C16
    assert cdb.eeprom_i2c_base(f, 0xFF) == 0x50
    assert cdb.eeprom_i2c_base(f, 0x7F) is None
    f = {"dev_addr_bits": 1, "dev_addr_shift": 2}               # 24xx1025
    assert cdb.eeprom_i2c_base(f, 0x22) == 0x51
    assert cdb.eeprom_i2c_base(f, 0x03) is None


def _with(db, name, **geom):
    import copy
    d = copy.deepcopy(db)
    d["chips"][name]["geometry"].update(geom)
    return cdb.flatten(d, name)


@pytest.mark.parametrize("name,geom,match", [
    ("24C16", {"dev_addr_bits": 2}, "more address bits"),
    ("24C16", {"dev_addr_shift": 1}, "within A2..A0"),
    ("24C02", {"addr_bytes": 3}, "1 or 2 bytes"),
    ("24C02", {"size_bytes": 100}, "powers? of two"),
    ("25xx256", {"dev_addr_bits": 1, "dev_addr_shift": 3}, "1-byte address"),
    ("25xx256", {"addr_bytes": 4}, "1, 2 or 3 bytes"),
    ("M95M04", {"size_bytes": 1 << 20}, "512 KiB"),
])
def test_eeprom_checks_reject(db, name, geom, match):
    with pytest.raises(cdb.ChipDBError, match=match):
        cdb.check_flat(_with(db, name, **geom))


def test_an_id_on_an_eeprom_is_refused(db):
    import copy
    d = copy.deepcopy(db)
    d["chips"]["24C02"]["id"] = {"mfr": 0x1F, "dev": 0x02}
    with pytest.raises(cdb.ChipDBError, match="no READ ID"):
        cdb.flatten(d, "24C02")


def test_eeprom_blob_roundtrip(db):
    f = cdb.flatten(db, "24xx1025")
    got = cdb.unpack_blob(cdb.pack_blob(f))
    assert got["family_code"] == 2
    assert (got["dev_addr_bits"], got["dev_addr_shift"]) == (1, 2)
    body = cdb.pack_blob(f)[6:-4]
    assert body[123:125] == bytes([1, 2]) and body[125:128] == b"\0\0\0"


def test_cli_lists_and_shows_by_alias(capsys):
    assert cdb.main(["--list", "--family", "spi-eeprom"]) == 0
    out = capsys.readouterr().out
    assert "  25xx040 " in out and "  24C02 " not in out
    assert cdb.main(["--show", "AT24C512"]) == 0
    assert "24C512" in capsys.readouterr().out
