import yaml

from tools import chipdb
from tools import chips_yml_to_db as conv

LEGACY = """
chips:
  MT29F2G01ABAGD:
    mfr_id:  0x2C
    dev_id:  0x24
    page_size:       2176
    spare_size:      128
    pages_per_block: 64
    total_blocks:    2048
    planes:          2
    bad_block_mark:  0x00
    has_qe_bit:      false
    ecc_default:     off
    vcc_mv:          3300
    notes: "2Gb SLC"
"""


def test_converted_entry_flattens_like_the_shipped_chip(tmp_path):
    legacy = yaml.safe_load(LEGACY)["chips"]
    db = chipdb.load_db()
    shipped = chipdb.flatten(db, "MT29F2G01ABAGD")

    root = tmp_path / "db"
    for sub in ("families", "profiles", "chips"):
        (root / sub).mkdir(parents=True)
    for sub in ("families", "profiles"):
        for f in (chipdb.os.listdir(chipdb.os.path.join(chipdb.DB_DIR, sub))):
            (root / sub / f).write_text(open(chipdb.os.path.join(chipdb.DB_DIR, sub, f)).read())
    src = tmp_path / "chips.yml"
    src.write_text(LEGACY)
    assert conv.main([str(src), "--profile", "micron", "--db", str(root)]) == 0

    new = chipdb.load_db(str(root))
    got = chipdb.flatten(new, "MT29F2G01ABAGD")
    for k in ("id_mfr", "id_dev", "page_size", "spare_size", "pages_per_block",
              "total_blocks", "planes", "ecc_shift", "ecc_mask", "ecc_map", "bbm_len"):
        assert got[k] == shipped[k], k
    assert got["resident"] is False
    assert "UNCITED" in new["chips"]["MT29F2G01ABAGD"]["datasheet"]
    assert legacy  # parsed


def test_existing_chip_file_is_not_overwritten(tmp_path, capsys):
    root = tmp_path / "db"
    (root / "chips").mkdir(parents=True)
    keep = root / "chips" / "MT29F2G01ABAGD.yml"
    keep.write_text("# mine\n")
    src = tmp_path / "chips.yml"
    src.write_text(LEGACY)
    conv.main([str(src), "--profile", "micron", "--db", str(root)])
    assert keep.read_text() == "# mine\n"
    assert "skipped" in capsys.readouterr().out
