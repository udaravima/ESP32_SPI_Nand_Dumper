import os, sys, json
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import dump


def test_resolve_defaults_when_no_file_no_cli():
    cfg = dump.resolve_config(cli={}, saved={})
    assert cfg["ip"] == dump.DEFAULT_IP
    assert cfg["port"] == dump.DEFAULT_PORT
    assert cfg["out_dir"] == dump.DEFAULT_OUT_DIR


def test_saved_file_overrides_defaults():
    cfg = dump.resolve_config(cli={}, saved={"ip": "192.168.1.9", "port": 4444})
    assert cfg["ip"] == "192.168.1.9"
    assert cfg["port"] == 4444
    assert cfg["out_dir"] == dump.DEFAULT_OUT_DIR   # unspecified falls back


def test_cli_overrides_saved_file():
    cfg = dump.resolve_config(cli={"ip": "10.0.0.5"},
                              saved={"ip": "192.168.1.9", "port": 4444})
    assert cfg["ip"] == "10.0.0.5"     # CLI wins
    assert cfg["port"] == 4444         # saved still supplies port


def test_cli_none_values_are_ignored():
    # argparse gives None for unspecified flags; those must not clobber saved.
    cfg = dump.resolve_config(cli={"ip": None, "port": None},
                              saved={"ip": "192.168.1.9"})
    assert cfg["ip"] == "192.168.1.9"


def test_save_and_load_roundtrip(tmp_path):
    p = tmp_path / "dump.config.json"
    dump.save_config(str(p), {"ip": "10.1.2.3", "port": 5555, "out_dir": "target"})
    assert dump.load_config(str(p)) == {"ip": "10.1.2.3", "port": 5555, "out_dir": "target"}


def test_load_missing_file_returns_empty(tmp_path):
    assert dump.load_config(str(tmp_path / "nope.json")) == {}


def test_load_corrupt_file_returns_empty(tmp_path):
    p = tmp_path / "dump.config.json"
    p.write_text("{ this is not json")
    assert dump.load_config(str(p)) == {}
