"""dump.py's side of the stage-3 command session (src/nand_session.h).

A scripted socket plays the device: it serves canned response frames and
records what the client sent, so each test pins both directions of the wire.
"""
import json
import os
import shutil
import socket
import subprocess
import struct
import sys
import zlib

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import dump  # noqa: E402
from tools import chipdb  # noqa: E402

DS35_ID = (0xE5, 0x71, 0x00)


def frame(cmd, status=0, payload=b""):
    """A device response frame: 'NRSP' cmd status len payload crc32."""
    head = b"NRSP" + struct.pack("<BBH", ord(cmd), status, len(payload)) + payload
    return head + struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)


def info_payload(ident=DS35_ID, state=1, name=b"MANUAL"):
    return struct.pack(dump.INFO_FMT, 1, 1, 8192, bytes(ident), state, name)


W25Q128_NOR_ID = (0xEF, 0x40, 0x18)
FLAT_BUS = (0xFF, 0xFF, 0xFF)


def info_v2_payload(ident=FLAT_BUS, nor_id=W25Q128_NOR_ID, state=1, name=b"MANUAL",
                    family=0):
    return struct.pack(dump.INFO_V2_FMT, 2, chipdb.SCHEMA_VER, 8192, bytes(ident), state,
                       name, bytes(nor_id), family)


def echo_v2_payload(flat, blob, detected):
    f = flat
    exp = bytes([f["id_mfr"], f["id_dev"], f["id_dev2"], f["id_flags"]])
    return struct.pack(dump.ECHO_V2_FMT, f["name"].encode(), f["page_size"], f["spare_size"],
                       f["pages_per_block"], f["total_blocks"], f["planes"], exp,
                       bytes(detected), struct.unpack("<I", blob[-4:])[0],
                       list(dump.FAMILIES.values()).index(f["family"]))


def echo_payload(flat, blob, detected=DS35_ID, **over):
    f = dict(flat, **over)
    exp = bytes([f["id_mfr"], f["id_dev"], f["id_dev2"], f["id_flags"]])
    return struct.pack(dump.ECHO_FMT, f["name"].encode(), f["page_size"], f["spare_size"],
                       f["pages_per_block"], f["total_blocks"], f["planes"], exp,
                       bytes(detected), struct.unpack("<I", blob[-4:])[0])


class ScriptedDevice:
    """recv() serves `replies` in order; an empty script means the device is
    silent (socket.timeout), like pre-v4 firmware waiting for 'G'."""
    def __init__(self, *replies):
        self.rx = b"".join(replies)
        self.sent = b""
        self.timeout = 30

    def sendall(self, data):
        self.sent += data

    def recv(self, n):
        if not self.rx:
            raise socket.timeout()
        out, self.rx = self.rx[:n], self.rx[n:]
        return out

    def gettimeout(self):
        return self.timeout

    def settimeout(self, t):
        self.timeout = t


@pytest.fixture
def ds35():
    flat = chipdb.flatten(chipdb.load_db(), "DS35Q1GA")
    return flat, chipdb.pack_blob(flat)


def test_info_parses_detected_chip():
    dev = ScriptedDevice(frame("I", payload=info_payload(state=2)))
    info = dump.query_info(dev)
    assert dev.sent == b"I"
    assert (info["mfr"], info["dev"], info["dev2"]) == DS35_ID
    assert info["state"] == "ambiguous"
    assert info["name"] == "MANUAL"
    assert info["max_page_size"] == 8192


def test_silent_device_is_pre_v4_firmware():
    dev = ScriptedDevice()
    assert dump.query_info(dev) is None
    assert dev.timeout == 30                     # the dump timeout is restored


def test_corrupt_response_is_rejected():
    bad = bytearray(frame("I", payload=info_payload()))
    bad[12] ^= 1
    with pytest.raises(IOError, match="CRC"):
        dump.query_info(ScriptedDevice(bytes(bad)))


def test_push_checks_echo_then_arms_with_blob_crc(ds35):
    flat, blob = ds35
    info = dump.parse_info(info_payload())
    dev = ScriptedDevice(frame("P", payload=echo_payload(flat, blob)), frame("A"))
    dump.push_and_arm(dev, flat, blob, info)
    assert dev.sent == b"P" + blob + b"A" + blob[-4:]


def test_echo_mismatch_never_arms(ds35):
    flat, blob = ds35
    info = dump.parse_info(info_payload())
    dev = ScriptedDevice(frame("P", payload=echo_payload(flat, blob, page_size=2176)))
    with pytest.raises(dump.SessionError, match="page_size"):
        dump.push_and_arm(dev, flat, blob, info)
    assert b"A" not in dev.sent[1 + len(blob):]


def test_id_mismatch_names_both_ids(ds35):
    flat, blob = ds35
    info = dump.parse_info(info_payload(ident=(0x2C, 0x24, 0)))
    dev = ScriptedDevice(frame("P", status=dump.PRF_ERRORS.index("E_ID_MISMATCH"),
                               payload=echo_payload(flat, blob, detected=(0x2C, 0x24, 0))))
    with pytest.raises(dump.SessionError) as e:
        dump.push_and_arm(dev, flat, blob, info)
    assert "0xE5 0x71" in str(e.value) and "0x2C 0x24" in str(e.value)


def test_device_refusal_carries_error_name(ds35):
    flat, blob = ds35
    dev = ScriptedDevice(frame("P", status=dump.PRF_ERRORS.index("E_PAGE_TOO_BIG")))
    with pytest.raises(dump.SessionError, match="E_PAGE_TOO_BIG"):
        dump.push_and_arm(dev, flat, blob, dump.parse_info(info_payload()))


def test_error_names_follow_the_firmware_enum():
    # Order must match nand_prf_err_t in src/nand_profile.h.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "src", "nand_profile.h")) as f:
        src = f.read()
    body = src[src.index("typedef enum {\n  NAND_PRF_OK"):src.index("} nand_prf_err_t;")]
    names = [ln.strip().split(",")[0].split()[0].replace("NAND_PRF_", "")
             for ln in body.splitlines()[1:] if ln.strip().startswith("NAND_PRF_")]
    assert names == dump.PRF_ERRORS


# ---- choosing what to push ---------------------------------------------------------
def test_resident_chip_needs_no_push():
    info = dump.parse_info(info_payload(state=0, name=b"DS35Q1GA"))
    assert dump.resolve_profile(info, None, {}) == (None, False)


def test_unknown_chip_resolves_from_db():
    flat, remember = dump.resolve_profile(dump.parse_info(info_payload()), None, {})
    assert flat["name"] == "DS35Q1GA" and remember is False


def test_unknown_chip_not_in_db_keeps_manual_geometry():
    info = dump.parse_info(info_payload(ident=(0xAB, 0xCD, 0)))
    assert dump.resolve_profile(info, None, {}) == (None, False)


def test_chip_flag_forces_a_profile():
    info = dump.parse_info(info_payload(state=0, name=b"DS35Q1GA"))
    flat, remember = dump.resolve_profile(info, "MT29F2G01ABAGD", {})
    assert flat["name"] == "MT29F2G01ABAGD" and remember is True


@pytest.fixture
def twin_db(tmp_path):
    """The real DB plus a second chip sharing DS35Q1GA's JEDEC ID."""
    root = tmp_path / "db"
    shutil.copytree(os.path.join(chipdb.DB_DIR), root)
    src = (root / "chips" / "DS35Q1GA.yml").read_text()
    (root / "chips" / "DS35TWIN.yml").write_text(
        src.replace("DS35Q1GA:", "DS35TWIN:").replace("total_blocks: 1024", "total_blocks: 2048"))
    return str(root)


def test_ambiguous_id_uses_cached_choice(twin_db):
    info = dump.parse_info(info_payload(state=1))
    saved = {"chips": {"E5:71": "DS35TWIN"}}
    flat, remember = dump.resolve_profile(info, None, saved, db_root=twin_db)
    assert flat["name"] == "DS35TWIN" and remember is False


def test_ambiguous_id_asks_and_never_guesses(twin_db):
    info = dump.parse_info(info_payload(state=1))
    with pytest.raises(chipdb.AmbiguousId):
        dump.resolve_profile(info, None, {}, db_root=twin_db)
    seen = []
    flat, remember = dump.resolve_profile(
        info, None, {}, ask=lambda c: seen.append(c) or c[1], db_root=twin_db)
    assert [c["name"] for c in seen[0]] == ["DS35Q1GA", "DS35TWIN"]
    assert flat["name"] == "DS35TWIN" and remember is True


# ---- SPI NOR (session v2) -------------------------------------------------------------
def test_v1_info_still_parses_as_spi_nand():
    info = dump.parse_info(info_payload())
    assert info["nor_id"] is None and info["family"] == "spi-nand"


def test_v2_info_carries_the_nor_view_and_family():
    info = dump.parse_info(info_v2_payload(state=0, name=b"W25Q128.V", family=1))
    assert info["nor_id"] == W25Q128_NOR_ID and info["family"] == "spi-nor"
    assert info["state"] == "resident"
    assert dump.detected_id(info, "spi-nor") == W25Q128_NOR_ID
    assert dump.detected_id(info, "spi-nand") == FLAT_BUS


def test_unknown_nor_chip_resolves_through_the_nor_view():
    info = dump.parse_info(info_v2_payload())
    flat, remember = dump.resolve_profile(info, None, {})
    assert flat["name"] == "W25Q128.V" and flat["family"] == "spi-nor"
    assert remember is False


def test_nor_capacity_byte_picks_the_series_member():
    # EF 40 19 is the 32 MiB part, not the 16 MiB EF 40 18 entry.
    info = dump.parse_info(info_v2_payload(nor_id=(0xEF, 0x40, 0x19)))
    flat, _ = dump.resolve_profile(info, None, {})
    assert flat["family"] == "spi-nor" and flat["id_dev2"] == 0x19


def test_flat_nor_view_is_ignored():
    info = dump.parse_info(info_v2_payload(ident=DS35_ID, nor_id=FLAT_BUS))
    flat, _ = dump.resolve_profile(info, None, {})
    assert flat["name"] == "DS35Q1GA"


def test_nand_and_nor_views_both_matching_is_ambiguous():
    info = dump.parse_info(info_v2_payload(ident=DS35_ID))
    with pytest.raises(chipdb.AmbiguousId) as e:
        dump.resolve_profile(info, None, {})
    assert {c["name"] for c in e.value.candidates} == {"DS35Q1GA", "W25Q128.V"}


def test_shared_nor_id_uses_the_nor_cache_key():
    info = dump.parse_info(info_v2_payload(nor_id=(0xEF, 0x8A, 0x16)))
    with pytest.raises(chipdb.AmbiguousId):
        dump.resolve_profile(info, None, {})
    saved = {"chips": {"NOR:EF:8A:16": "W77Q32JW"}}
    flat, remember = dump.resolve_profile(info, None, saved)
    assert flat["name"] == "W77Q32JW" and remember is False


def test_nor_push_checks_the_nor_view_and_family(tmp_path, monkeypatch):
    monkeypatch.setattr(dump, "CONFIG_PATH", str(tmp_path / "cfg.json"))
    flat = chipdb.flatten(chipdb.load_db(), "W25Q128.V")
    blob = chipdb.pack_blob(flat)
    dev = ScriptedDevice(frame("I", payload=info_v2_payload()),
                         frame("P", payload=echo_v2_payload(flat, blob, W25Q128_NOR_ID)),
                         frame("A"))
    prof = dump.prepare_profile(dev, "W25Q128.V", {})
    assert prof["family"] == "spi-nor" and prof["source"] == "pushed"
    assert json.loads((tmp_path / "cfg.json").read_text()) == {
        "chips": {"NOR:EF:40:18": "W25Q128.V"}}


def test_nor_echo_with_nand_family_never_arms():
    flat = chipdb.flatten(chipdb.load_db(), "W25Q128.V")
    blob = chipdb.pack_blob(flat)
    info = dump.parse_info(info_v2_payload())
    body = bytearray(echo_v2_payload(flat, blob, W25Q128_NOR_ID))
    body[-1] = 0                                     # device says spi-nand
    dev = ScriptedDevice(frame("P", payload=bytes(body)))
    with pytest.raises(dump.SessionError, match="family"):
        dump.push_and_arm(dev, flat, blob, info)
    assert dev.sent == b"P" + blob


# ---- whole pre-dump session ---------------------------------------------------------
def test_prepare_profile_pushes_and_remembers_forced_chip(ds35, tmp_path, monkeypatch):
    flat, blob = ds35
    cfg = tmp_path / "dump.config.json"
    monkeypatch.setattr(dump, "CONFIG_PATH", str(cfg))
    dev = ScriptedDevice(frame("I", payload=info_payload(state=0, name=b"DS35Q1GA")),
                         frame("P", payload=echo_payload(flat, blob)), frame("A"))
    saved = {"ip": "10.0.0.2"}
    prof = dump.prepare_profile(dev, "DS35Q1GA", saved)
    assert prof == {"name": "DS35Q1GA", "source": "pushed", "family": "spi-nand",
                    "profile": "dosilicon", "ecc_scheme": "generic2",
                    "schema_ver": chipdb.SCHEMA_VER}
    assert dev.sent == b"I" + b"P" + blob + b"A" + blob[-4:]
    assert json.loads(cfg.read_text()) == {"ip": "10.0.0.2", "chips": {"E5:71": "DS35Q1GA"}}


def test_prepare_profile_resident_sends_only_info():
    dev = ScriptedDevice(frame("I", payload=info_payload(state=0, name=b"DS35Q1GA")))
    assert dump.prepare_profile(dev, None, {}) == {"name": "DS35Q1GA", "source": "resident",
                                                    "family": "spi-nand"}
    assert dev.sent == b"I"


def test_prepare_profile_legacy_firmware_refuses_chip_flag():
    assert dump.prepare_profile(ScriptedDevice(), None, {}) is None
    with pytest.raises(dump.SessionError, match="predates"):
        dump.prepare_profile(ScriptedDevice(), "DS35Q1GA", {})


def test_refused_go_is_a_response_frame_not_a_header():
    data = frame("G", status=dump.PRF_ERRORS.index("E_NOT_ARMED"))
    dev = ScriptedDevice(data[4:])
    cmd, status, _ = dump.read_response(dev, data[:4])
    assert cmd == "G" and dump.prf_error_name(status) == "E_NOT_ARMED"


def test_metadata_records_profile(tmp_path):
    g = dict(page_size=2112, spare_size=64, pages_per_block=64, total_blocks=1024,
             total_pages=65536, total_bytes=65536 * 2112, mfr_id=0xE5, dev_id=0x71,
             page_addr_bits=6, flags=0)
    p = tmp_path / "d.bin"
    dump.write_metadata(str(p), g, g["total_bytes"], profile={"name": "DS35Q1GA",
                                                              "source": "resident"})
    meta = json.loads((tmp_path / "d.bin.meta.json").read_text())
    assert meta["profile"] == {"name": "DS35Q1GA", "source": "resident"}


# ---- against the firmware's own session code ----------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@pytest.fixture(scope="module")
def device_bin(tmp_path_factory):
    """Build tests/session_device.cpp with the real src/nand_session.cpp."""
    cxx = shutil.which("g++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler")
    out = str(tmp_path_factory.mktemp("dev") / "session_device")
    src = [os.path.join(ROOT, p) for p in ("tests/session_device.cpp", "src/nand_session.cpp",
                                           "src/nand_profile.cpp", "src/dump_header.cpp",
                                           "src/eeprom_seq.cpp")]
    subprocess.run([cxx, "-std=gnu++17", "-I", os.path.join(ROOT, "src"), *src, "-o", out],
                   check=True)
    return out


class PipeDevice:
    """A socket-shaped wrapper around the session_device process."""
    def __init__(self, exe, ident, nor_id=(), presence=()):
        # presence = (I2C ACK mask, SPI EEPROM status); needs nor_id too.
        args = (*ident, *nor_id, *presence)
        self.p = subprocess.Popen([exe, *(f"{b:02X}" for b in args)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def sendall(self, data):
        self.p.stdin.write(data)
        self.p.stdin.flush()

    def recv(self, n):
        return self.p.stdout.read1(n)

    def gettimeout(self):
        return 30

    def settimeout(self, t):
        pass

    def close(self):
        self.p.stdin.close()
        return self.p.wait(timeout=5)


def test_c_device_arms_pushed_profile_then_dumps_on_it(device_bin):
    dev = PipeDevice(device_bin, DS35_ID)
    prof = dump.prepare_profile(dev, None, {})       # unknown ID -> DB -> push -> arm
    assert prof["source"] == "pushed" and prof["name"] == "DS35Q1GA"
    dev.sendall(b"G")
    assert dump.recv_exact(dev, 24).rstrip(b"\0") == b"DS35Q1GA"
    assert dev.close() == 0


def test_c_device_refuses_profile_for_another_chip(device_bin, tmp_path, monkeypatch):
    monkeypatch.setattr(dump, "CONFIG_PATH", str(tmp_path / "cfg.json"))
    dev = PipeDevice(device_bin, (0x2C, 0x24, 0x00))
    with pytest.raises(dump.SessionError, match="E_ID_MISMATCH"):
        dump.prepare_profile(dev, "DS35Q1GA", {})
    dev.sendall(b"G")                                 # nothing staged: manual profile
    assert dump.recv_exact(dev, 24).rstrip(b"\0") == b"MANUAL"
    assert not (tmp_path / "cfg.json").exists()       # a refused choice isn't remembered
    dev.close()


def test_c_device_arms_a_nor_profile_on_the_nor_view(device_bin):
    dev = PipeDevice(device_bin, FLAT_BUS, W25Q128_NOR_ID)
    prof = dump.prepare_profile(dev, None, {})
    assert prof["source"] == "pushed" and prof["name"] == "W25Q128.V"
    assert prof["family"] == "spi-nor"
    dev.sendall(b"G")
    assert dump.recv_exact(dev, 24).rstrip(b"\0") == b"W25Q128.V"
    assert dev.close() == 0


def test_c_device_refuses_nor_profile_when_only_the_nand_view_matches(device_bin, tmp_path,
                                                                      monkeypatch):
    monkeypatch.setattr(dump, "CONFIG_PATH", str(tmp_path / "cfg.json"))
    dev = PipeDevice(device_bin, W25Q128_NOR_ID, FLAT_BUS)
    with pytest.raises(dump.SessionError, match="E_ID_MISMATCH"):
        dump.prepare_profile(dev, "W25Q128.V", {})
    dev.close()


# ---- serial EEPROM (session v3) ----------------------------------------------------
def info_v3_payload(mask=0, sr=0xFF, state=1, name=b"MANUAL", family=0):
    return struct.pack(dump.INFO_V3_FMT, 3, chipdb.SCHEMA_VER, 8192, bytes(FLAT_BUS), state,
                       name, bytes(FLAT_BUS), family, mask, sr)


def test_info_v3_parses_eeprom_presence():
    info = dump.parse_info(info_v3_payload(mask=0x0F, sr=0x0C, state=5, name=b"24C256",
                                           family=2))
    assert info["session_ver"] == 3
    assert info["i2c_ack_mask"] == 0x0F and info["spi_ee_status"] == 0x0C
    assert info["state"] == "picked" and info["family"] == "i2c-eeprom"
    assert dump.detected_id(info, "i2c-eeprom") == (0x0F, 0, 0)
    assert dump.detected_id(info, "spi-eeprom") == (0x0C, 0, 0)
    v2 = dump.parse_info(info_v2_payload())
    assert v2["i2c_ack_mask"] is None and v2["spi_ee_status"] is None


def test_hint_lists_the_i2c_parts_that_fit():
    db = chipdb.load_db()
    info = dump.parse_info(info_v3_payload(mask=0x03))
    (hint,) = dump.eeprom_hint(info, db)
    assert "0x50, 0x51" in hint
    fits = hint.split("parts that fit: ")[1].split(", ")
    assert "24C04" in fits and "24C02" in fits and "24C16" not in fits
    info = dump.parse_info(info_v3_payload(sr=0x00))
    (hint,) = dump.eeprom_hint(info, db)
    assert "SPI EEPROM" in hint
    assert dump.eeprom_hint(dump.parse_info(info_v3_payload()), db) == []


def test_eeprom_pick_needs_v3_firmware():
    info = dump.parse_info(info_v2_payload(nor_id=FLAT_BUS))
    with pytest.raises(chipdb.ChipDBError, match="predates EEPROM"):
        dump.resolve_profile(info, "AT24C256", {})


def test_dump_family_from_header_flags():
    assert dump.dump_family(0) == "spi-nand"
    assert dump.dump_family(dump.FLAG_NOR) == "spi-nor"
    assert dump.dump_family(dump.FLAG_EEPROM) == "spi-eeprom"
    assert dump.dump_family(dump.FLAG_EEPROM | dump.FLAG_I2C) == "i2c-eeprom"


def test_c_device_arms_an_i2c_eeprom_named_by_alias(device_bin, tmp_path, monkeypatch):
    monkeypatch.setattr(dump, "CONFIG_PATH", str(tmp_path / "cfg.json"))
    dev = PipeDevice(device_bin, FLAT_BUS, FLAT_BUS, (0x01, 0xFF))
    prof = dump.prepare_profile(dev, "AT24C256", {})
    assert prof["name"] == "24C256" and prof["family"] == "i2c-eeprom"
    dev.sendall(b"G")
    assert dump.recv_exact(dev, 24).rstrip(b"\0") == b"24C256"
    assert dev.close() == 0
    assert not (tmp_path / "cfg.json").exists()       # an EEPROM pick is never remembered


def test_c_device_refuses_an_i2c_part_whose_addresses_do_not_answer(device_bin):
    dev = PipeDevice(device_bin, FLAT_BUS, FLAT_BUS, (0x01, 0xFF))
    with pytest.raises(dump.SessionError, match="E_ID_MISMATCH"):
        dump.prepare_profile(dev, "24C16", {})        # a 24C16 answers at all eight
    dev.close()


def test_c_device_spi_eeprom_needs_a_plausible_status(device_bin):
    dev = PipeDevice(device_bin, FLAT_BUS, FLAT_BUS, (0x00, 0xFF))
    with pytest.raises(dump.SessionError, match="E_ID_MISMATCH"):
        dump.prepare_profile(dev, "25LC040A", {})     # RDSR read 0xFF: nothing there
    dev.close()
    dev = PipeDevice(device_bin, FLAT_BUS, FLAT_BUS, (0x00, 0x00))
    prof = dump.prepare_profile(dev, "25LC040A", {})
    assert prof["name"] == "25xx040" and prof["family"] == "spi-eeprom"
    dev.sendall(b"G")
    assert dump.recv_exact(dev, 24).rstrip(b"\0") == b"25xx040"
    assert dev.close() == 0
