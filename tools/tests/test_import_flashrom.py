"""tools/import_flashrom.py against a tiny synthetic flashrom tree.

The fixture imitates the shape of flashrom's include/flashchips.h and
flashchips/*.c (field names, macros, the .tested struct) with made-up chips;
no flashrom source is vendored here."""
import shutil

import pytest
import yaml

from tools import chipdb as cdb
from tools import import_flashrom as imp

HEADER = """
#define ACME_ID         0xEF   /* pretend vendor */
#define ACME_A16        0x4015
#define ACME_A32        0x4016
#define ACME_B256       0x4019
#define ACME_C256       0x4119
#define ACME_E256       0x4219
"""

CHIPS = """
{
    .vendor = "Acme",
    .name = "AC25A16",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_A16,
    .total_size = 2048,
    .feature_bits = FEATURE_WRSR_WREN,
    .tested = TEST_OK_PREW,
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
    .voltage = {2700, 3600},
},
{   // same ID and size under another name: merged, listed as an alias
    .vendor = "Acme",
    .name = "AC25A16X/AC25A16Y",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_A16,
    .total_size = 2 * 1024,
    .tested = {.probe = OK, .read = NT, .erase = NT, .write = NT},
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
    .voltage = {1650, 1950},
},
{   /* 1.8 V only and the read is untested: not resident */
    .vendor = "Acme",
    .name = "AC25A32W",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_A32,
    .total_size = 4096,
    .tested = {.probe = OK, .read = OK, .erase = NT, .write = NT},
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
    .voltage = {1650, 1950},
},
{
    .vendor = "Acme",
    .name = "AC25B256",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_B256,
    .total_size = 32768,
    .feature_bits = FEATURE_WRSR_WREN | FEATURE_4BA,
    .tested = TEST_OK_PREW,
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
    .voltage = {2700, 3600},
},
{
    .vendor = "Acme",
    .name = "AC25C256",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_C256,
    .total_size = 32768,
    .feature_bits = FEATURE_4BA_ENTER,
    .tested = TEST_OK_PREW,
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
    .voltage = {2700, 3600},
},
{   /* above 16 MiB with only an extended address register: skipped */
    .vendor = "Acme",
    .name = "AC25E256",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = ACME_E256,
    .total_size = 32768,
    .feature_bits = FEATURE_4BA_EAR_ANY,
    .tested = TEST_OK_PREW,
    .probe = PROBE_SPI_RDID,
    .read = SPI_CHIP_READ,
},
{
    .vendor = "Acme",
    .name = "AC25OLD",
    .bustype = BUS_SPI,
    .manufacture_id = ACME_ID,
    .model_id = 0x12,
    .total_size = 512,
    .tested = TEST_OK_PREW,
    .probe = PROBE_SPI_RES1,
    .read = SPI_CHIP_READ,
},
{
    .vendor = "Acme",
    .name = "AC29PAR",
    .bustype = BUS_PARALLEL,
    .manufacture_id = ACME_ID,
    .model_id = 0x13,
    .total_size = 512,
    .probe = PROBE_JEDEC,
    .read = READ_MEMMAPPED,
},
"""


@pytest.fixture
def tree(tmp_path):
    fr = tmp_path / "flashrom"
    (fr / "include").mkdir(parents=True)
    (fr / "flashchips").mkdir()
    (fr / "include" / "flashchips.h").write_text(HEADER)
    (fr / "flashchips" / "acme_chips.c").write_text(CHIPS)
    db = tmp_path / "db"
    shutil.copytree(cdb.DB_DIR, db)
    shutil.rmtree(db / "chips" / "spi-nor")
    return fr, db


def _chips(by_file):
    return {c["name"]: c for cs in by_file.values() for c in cs}


def test_collect_keeps_readable_rdid_chips(tree):
    fr, db = tree
    by_file, skipped = imp.collect(str(fr), str(db))
    chips = _chips(by_file)
    assert sorted(chips) == ["AC25A16", "AC25A32W", "AC25B256", "AC25C256"]
    why = dict(skipped)
    assert why["AC25E256"].startswith("over 16 MiB")
    assert why["AC25OLD"] == "probe PROBE_SPI_RES1"
    assert "AC29PAR" not in why                      # not SPI: not even listed


def test_merge_sizes_and_test_status(tree):
    fr, db = tree
    chips = _chips(imp.collect(str(fr), str(db))[0])
    a16 = chips["AC25A16"]
    assert a16["aliases"] == ["AC25A16X", "AC25A16Y"]
    assert a16["size_kib"] == 2048 and a16["vcc_mv"] == 1800 and a16["read_ok"]
    # The .tested literal's ".read = OK" counts; its ".probe" is not the probe.
    assert chips["AC25A32W"]["read_ok"] and chips["AC25A32W"]["vcc_mv"] == 1800
    assert chips["AC25B256"]["addr4"] == "native"
    assert chips["AC25C256"]["addr4"] == "enter"


def test_hand_written_chip_wins(tree):
    fr, db = tree
    (db / "chips" / "AC25B256.yml").write_text(
        "AC25B256-HAND:\n"
        "  id: {mfr: 0xEF, dev: 0x40, dev2: 0x19}\n"
        "  family: spi-nor\n  profile: nor-winbond\n  geometry: {size_kib: 32768}\n"
        "  addr4: native\n  resident: false\n  datasheet: \"local\"\n")
    by_file, skipped = imp.collect(str(fr), str(db))
    assert "AC25B256" not in _chips(by_file)
    assert ("AC25B256", "defined by hand in db/chips/") in skipped


def test_rendered_yaml_loads_into_chipdb(tree):
    fr, db = tree
    by_file, _ = imp.collect(str(fr), str(db))
    out = db / "chips" / "spi-nor"
    out.mkdir()
    for src, chips in by_file.items():
        (out / imp.out_name(src)).write_text(imp.render_file(src, chips, "deadbeef"))
    assert [p.name for p in out.iterdir()] == ["flashrom-acme-chips.yml"]
    raw = yaml.safe_load((out / "flashrom-acme-chips.yml").read_text())
    assert raw["AC25A16"]["resident"] is False        # 1.8 V alias pulls vcc down
    assert raw["AC25B256"]["resident"] is True
    assert "deadbeef/flashchips/acme_chips.c" in raw["AC25A16"]["source"]
    loaded = cdb.load_db(str(db))
    flat = cdb.flatten(loaded, "AC25C256")
    cdb.check_flat(flat)
    assert flat["family"] == "spi-nor" and flat["addr4_mode"] == cdb.ADDR4_MODES["enter"]
    assert flat["addr_bytes"] == 4
