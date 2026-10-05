"""ESP32 SPI NAND/NOR + serial EEPROM Dumper - PC-side TCP receiver.

Connects to the ESP32 over WiFi TCP, sends a 'GO' trigger, reads a 32-byte
geometry header, then streams the raw NAND dump to a timestamped file in
target/ and writes a <dump>.meta.json sidecar describing the geometry.

Before the dump it asks the device which chip it detected ('I'). If the
firmware has no built-in profile for it, dump.py looks the ID up in the chip
database (db/, via tools/chipdb.py), pushes the flat profile ('P'), checks the
device's echo of it, and arms it ('A'). Firmware older than v4 does not answer
'I'; dump.py then sends a bare 'G', as it always did.

Serial EEPROMs (24xx on I2C, 25xx on SPI) have no ID, so the part is named:
--chip takes a DB name or an alias (AT24C256, 25LC040A). The device still
checks that the part is plausibly there (I2C: it answers at every address it
occupies; SPI: its status register reads like a 25xx).

Usage:
    python3 dump.py
    python3 dump.py --chip DS35Q1GA     # push this DB profile (ID still cross-checked)
    python3 dump.py --chip AT24C256     # serial EEPROM: name the part

Set ESP32_IP to the address printed in the ESP32 serial monitor.
"""
import argparse
import socket
import struct
import zlib
import json
import time
import sys
import os
import datetime
from collections import namedtuple

# ============ CONFIGURATION ============
# Precedence at runtime: CLI flag > dump.config.json > these defaults.
# Pass --ip/--port once and they are remembered in dump.config.json (local,
# gitignored) so later runs need no flags.
DEFAULT_IP = '10.65.224.57'   # first-run fallback; override with --ip
DEFAULT_PORT = 3333
DEFAULT_OUT_DIR = 'target'
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'dump.config.json')
PROGRESS_INTERVAL = 1024      # print progress every N pages
# =======================================


def format_duration(seconds):
    """Format a duration as H:MM:SS, or MM:SS when under an hour."""
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def load_config(path=CONFIG_PATH):
    """Return the saved config dict, or {} if missing/unreadable/corrupt."""
    try:
        with open(path) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(path, cfg):
    """Persist the resolved connection settings for next time."""
    with open(path, 'w') as f:
        json.dump(cfg, f, indent=2)


def resolve_config(cli, saved):
    """Merge with precedence CLI > saved > default. A None in `cli` (argparse
    leaves unspecified flags None) is ignored so it can't clobber a saved value."""
    def pick(key, default):
        if cli.get(key) is not None:
            return cli[key]
        if saved.get(key) is not None:
            return saved[key]
        return default
    return {"ip": pick("ip", DEFAULT_IP),
            "port": pick("port", DEFAULT_PORT),
            "out_dir": pick("out_dir", DEFAULT_OUT_DIR)}

# ---- Command session (src/nand_session.h) ---------------------------------------
RESP_MAGIC = b"NRSP"
RESP_HDR_FMT = "<4sBBH"          # magic, cmd, status, payload length
INFO_FMT = "<BBI3sB24s"          # session ver, schema ver, max page, id[3], state, name
INFO_V2_FMT = INFO_FMT + "3sB"   # session v2: + SPI NOR id view[3], active family
INFO_V3_FMT = INFO_V2_FMT + "BB"  # session v3: + I2C ACK mask (0x50..0x57), SPI RDSR
ECHO_FMT = "<24sIIIIB4s3sI"      # name, page, spare, ppb, blocks, planes,
                                 # expected id (mfr, dev, dev2, flags), detected id, blob crc
ECHO_V2_FMT = ECHO_FMT + "B"     # session v2: + profile family
INFO_TIMEOUT = 3.0               # pre-v4 firmware never answers 'I'
# nand_prf_err_t, in enum order (codes travel on the wire and only ever grow).
PRF_ERRORS = ["OK", "E_BAD_MAGIC", "E_SCHEMA_VER", "E_BAD_LEN", "E_BAD_CRC",
              "E_STRUCTURE", "E_GEOMETRY", "E_PAGE_TOO_BIG", "E_ID_MISMATCH",
              "E_AMBIGUOUS_ID", "E_UNKNOWN_ID", "E_NOT_STAGED", "E_ARM_CRC",
              "E_NOT_ARMED", "E_BAD_CMD", "E_TIMEOUT"]
CHIP_STATES = {0: "resident", 1: "unknown", 2: "ambiguous", 3: "pushed", 4: "sfdp",
               5: "picked"}
FAMILIES = {0: "spi-nand", 1: "spi-nor", 2: "i2c-eeprom", 3: "spi-eeprom"}
EEPROM_FAMILIES = ("i2c-eeprom", "spi-eeprom")


class SessionError(Exception):
    """The device refused a command. `code` is its nand_prf_err_t name."""
    def __init__(self, cmd, code, payload=b""):
        self.cmd, self.code, self.payload = cmd, code, payload
        super().__init__(f"'{cmd}' refused: {code}")


def prf_error_name(status):
    return PRF_ERRORS[status] if status < len(PRF_ERRORS) else f"E_{status}"


def read_response(sock, first=b""):
    """Read one 'NRSP' frame (whose first bytes may already be in `first`).
    Returns (cmd char, status code, payload)."""
    hdr = first + recv_exact(sock, 8 - len(first))
    magic, cmd, status, ln = struct.unpack(RESP_HDR_FMT, hdr)
    if magic != RESP_MAGIC:
        raise IOError(f"bad response magic {magic!r}")
    rest = recv_exact(sock, ln + 4)
    (crc,) = struct.unpack("<I", rest[ln:])
    if zlib.crc32(hdr + rest[:ln]) & 0xFFFFFFFF != crc:
        raise IOError("response CRC mismatch")
    return chr(cmd), status, rest[:ln]


def command(sock, cmd, payload=b""):
    """Send one command and return its payload; raise SessionError on refusal."""
    sock.sendall(cmd.encode() + payload)
    rcmd, status, body = read_response(sock)
    if rcmd != cmd:
        raise IOError(f"reply to '{rcmd}' while waiting for '{cmd}'")
    if status:
        raise SessionError(cmd, prf_error_name(status), body)
    return body


def parse_info(body):
    """'I' reply. `mfr/dev/dev2` is the SPI NAND view of the ID (9Fh + dummy
    byte); session v2 adds `nor_id`, the plain-9Fh SPI NOR view, and the
    active profile's family; v3 adds the EEPROM presence bytes (`i2c_ack_mask`,
    `spi_ee_status`). v1 firmware knows only SPI NAND."""
    v2 = len(body) >= struct.calcsize(INFO_V2_FMT)
    v3 = len(body) >= struct.calcsize(INFO_V3_FMT)
    fields = struct.unpack_from(INFO_V3_FMT if v3 else INFO_V2_FMT if v2 else INFO_FMT, body)
    sver, schema, max_page, ident, state, name = fields[:6]
    return {"session_ver": sver, "schema_ver": schema, "max_page_size": max_page,
            "mfr": ident[0], "dev": ident[1], "dev2": ident[2],
            "nor_id": tuple(fields[6]) if v2 else None,
            "family": FAMILIES.get(fields[7], "spi-nand") if v2 else "spi-nand",
            "i2c_ack_mask": fields[8] if v3 else None,
            "spi_ee_status": fields[9] if v3 else None,
            "state": CHIP_STATES.get(state, f"state{state}"),
            "name": name.split(b"\0", 1)[0].decode(errors="replace")}


def detected_id(info, family):
    """The detected ID as a chip of `family` reports it. A serial EEPROM has
    none; its view is the presence byte the device compared (session v3)."""
    if family == "spi-nor":
        return info.get("nor_id")
    if family == "i2c-eeprom":
        return (info.get("i2c_ack_mask") or 0, 0, 0)
    if family == "spi-eeprom":
        sr = info.get("spi_ee_status")
        return (0xFF if sr is None else sr, 0, 0)
    return (info["mfr"], info["dev"], info["dev2"])


def nor_id_plausible(nid):
    """Same rule as the firmware: a real manufacturer byte, not a flat bus."""
    return bool(nid) and nid[0] not in (0x00, 0xFF)


def spi_ee_status_plausible(sr):
    """Same rule as the firmware: a 25xx/FRAM reads status bits 6..4 as 0."""
    return sr is not None and sr & 0x70 == 0


def eeprom_hint(info, db):
    """What an unidentified socket might hold, from the v3 presence bytes:
    I2C parts whose address set answered, or a SPI EEPROM status."""
    from tools import chipdb
    hints = []
    mask = info.get("i2c_ack_mask")
    if mask:
        fits = [n for n, c in db["chips"].items() if c["family"] == "i2c-eeprom"
                and chipdb.eeprom_i2c_base(chipdb.flatten(db, n), mask) is not None]
        found = ", ".join(f"0x{0x50 + i:02X}" for i in range(8) if mask >> i & 1)
        hints.append(f"an I2C EEPROM answers at {found}; parts that fit: {', '.join(fits)}")
    nand_flat = (info["mfr"], info["dev"]) in ((0, 0), (0xFF, 0xFF))
    if nand_flat and not nor_id_plausible(info.get("nor_id")) and \
            spi_ee_status_plausible(info.get("spi_ee_status")):
        hints.append(f"a SPI EEPROM may be in the socket (status "
                     f"0x{info['spi_ee_status']:02X}); 25xx parts are listed by "
                     "tools/chipdb.py --list --family spi-eeprom")
    return hints


def query_info(sock, timeout=INFO_TIMEOUT):
    """Ask the device what it detected. None means pre-v4 firmware, which
    ignores 'I' and is still waiting for 'G'."""
    old = sock.gettimeout()
    sock.settimeout(timeout)
    try:
        sock.sendall(b"I")
        first = sock.recv(1)
    except socket.timeout:
        return None
    finally:
        sock.settimeout(old)
    if not first:
        raise IOError("connection closed")
    rcmd, status, body = read_response(sock, first)
    if rcmd != "I" or status:
        raise IOError(f"unexpected reply to 'I': '{rcmd}' {prf_error_name(status)}")
    return parse_info(body)


def parse_echo(body):
    v2 = len(body) >= struct.calcsize(ECHO_V2_FMT)
    fields = struct.unpack_from(ECHO_V2_FMT if v2 else ECHO_FMT, body)
    (name, page, spare, ppb, blocks, planes, exp, det, crc) = fields[:9]
    return {"name": name.split(b"\0", 1)[0].decode(errors="replace"),
            "page_size": page, "spare_size": spare, "pages_per_block": ppb,
            "total_blocks": blocks, "planes": planes,
            "expected_id": tuple(exp[:3]), "expected_flags": exp[3],
            "detected_id": tuple(det), "crc": crc,
            "family": FAMILIES.get(fields[9], "spi-nand") if v2 else "spi-nand"}


def echo_mismatches(echo, flat, blob, info):
    """Compare the device's echo with what was pushed. Empty list = arm."""
    want = {"name": flat["name"], "page_size": flat["page_size"],
            "spare_size": flat["spare_size"], "pages_per_block": flat["pages_per_block"],
            "total_blocks": flat["total_blocks"], "planes": flat["planes"],
            "expected_id": (flat["id_mfr"], flat["id_dev"], flat["id_dev2"]),
            "detected_id": detected_id(info, flat["family"]),
            "family": flat["family"],
            "crc": struct.unpack("<I", blob[-4:])[0]}
    return [f"{k}: device {echo[k]!r}, host {v!r}" for k, v in want.items() if echo[k] != v]


def push_and_arm(sock, flat, blob, info):
    """'P' the blob, check the echo field by field, then 'A' with its CRC.
    Nothing goes live on the device unless every check here passes."""
    try:
        echo = parse_echo(command(sock, "P", blob))
    except SessionError as e:
        if e.code == "E_ID_MISMATCH" and len(e.payload) >= struct.calcsize(ECHO_FMT):
            ec = parse_echo(e.payload)
            raise SessionError("P", "E_ID_MISMATCH: profile expects "
                               + " ".join(f"0x{b:02X}" for b in ec["expected_id"])
                               + ", chip reads "
                               + " ".join(f"0x{b:02X}" for b in ec["detected_id"])) from None
        raise
    bad = echo_mismatches(echo, flat, blob, info)
    if bad:
        # Not armed: the device drops the staged profile when the session ends.
        raise SessionError("P", "echo mismatch (" + "; ".join(bad) + ")")
    command(sock, "A", blob[-4:])
    return echo


def id_key(mfr, dev, family="spi-nand", dev2=None):
    """dump.config.json key for a remembered chip choice. SPI NAND keeps the
    stage-3 'MF:DV' form; SPI NOR adds the capacity byte and a prefix, since
    one (mfr, type) spans a whole series there."""
    if family == "spi-nor":
        return f"NOR:{mfr:02X}:{dev:02X}:{dev2:02X}"
    return f"{mfr:02X}:{dev:02X}"


def resolve_profile(info, chip, saved, ask=None, db_root=None):
    """Pick the DB profile to push, or None to dump with what the device has.

    `chip` (from --chip) always wins. Otherwise only a chip the firmware could
    not resolve on its own (unknown or ambiguous ID) is looked up; an ambiguous
    one uses the choice remembered in dump.config.json, else asks `ask`.
    Returns (flat, remember) where remember says to cache the choice."""
    if not chip and info["state"] not in ("unknown", "ambiguous"):
        return None, False
    from tools import chipdb
    db = chipdb.load_db(db_root) if db_root else chipdb.load_db()
    if chip:
        flat = chipdb.flatten(db, chipdb.resolve_name(db, chip))
        chipdb.check_flat(flat)
        if flat["family"] in EEPROM_FAMILIES and (info.get("session_ver") or 1) < 3:
            raise chipdb.ChipDBError(f"{flat['name']} is a serial EEPROM; this firmware "
                                     "predates EEPROM support (session v3)")
        # An EEPROM pick is not tied to an ID, so it is never remembered.
        return flat, flat["family"] not in EEPROM_FAMILIES
    chips = saved.get("chips") or {}
    # Look the chip up in each family through that family's view of the ID
    # (same rule as the firmware's chip_detect): SPI NAND always, SPI NOR when
    # v2 firmware reported a plausible plain-9Fh ID.
    views = [("spi-nand", (info["mfr"], info["dev"], info["dev2"]))]
    if nor_id_plausible(info.get("nor_id")):
        views.append(("spi-nor", info["nor_id"]))
    found, ambiguous = [], []
    for family, (mfr, dev, dev2) in views:
        cached = chips.get(id_key(mfr, dev, family, dev2))
        try:
            found.append(chipdb.identify(db, mfr, dev, dev2, cached, family=family))
        except chipdb.AmbiguousId as e:
            ambiguous += e.candidates
        except chipdb.ChipDBError:
            pass
    cands = found + ambiguous
    if not cands:
        return None, False      # not in the DB either: device keeps manual geometry
    if len(found) == 1 and not ambiguous:
        return found[0], False
    if ask is None:
        raise chipdb.AmbiguousId(cands)
    flat = ask(cands)
    return flat, flat is not None


def ask_candidate(cands):
    """Interactive tiebreak for a shared JEDEC ID (design section 5)."""
    print("[?] Several chips share this ID:")
    for i, c in enumerate(cands, 1):
        if c["family"] == "spi-nor":
            size = c["page_size"] * c["pages_per_block"] * c["total_blocks"]
            print(f"    [{i}] {c['name']}: SPI NOR, {size >> 10} KiB")
        else:
            print(f"    [{i}] {c['name']}: {c['total_blocks']} blocks x "
                  f"{c['pages_per_block']} x {c['page_size']} B, {c['planes']} plane(s)")
    while True:
        pick = input("    Pick one (empty to abort): ").strip()
        if not pick:
            return None
        if pick.isdigit() and 1 <= int(pick) <= len(cands):
            return cands[int(pick) - 1]


HEADER_FMT = "<6sBBHHHHIBBBBII"
HEADER_SIZE = 32

# Header flag bits (must match src/dump_header.h)
FLAG_ECC_ON = 0x01
FLAG_QUAD = 0x02
FLAG_VERIFY = 0x04
FLAG_PAGECRC = 0x08   # proto v2: each page followed by a 4-byte CRC32 seal
FLAG_NOR = 0x10       # SPI NOR: pages are 4 KiB read units, no spare area
FLAG_EEPROM = 0x20    # serial EEPROM: 256 B read units (or the whole part)
FLAG_I2C = 0x40       # ... on I2C; mfr_id is the device address it answered at

# Result of streaming a dump off the wire.
ReceiveResult = namedtuple(
    "ReceiveResult", "bytes_received pages_received bad_pages truncated")


def parse_header(buf):
    """Parse and CRC-check the 32-byte geometry header. Returns a dict."""
    assert len(buf) == HEADER_SIZE, "short header"
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, pab, _r, total_bytes, crc) = struct.unpack(HEADER_FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver in (1, 2), "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=pab, flags=flags, total_bytes=total_bytes,
                proto_version=ver)


def recv_exact(sock, n):
    """Receive exactly n bytes or raise."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed mid-header")
        buf += chunk
    return buf


def receive_pages(sock, geom, fout, progress=None):
    """Stream a dump off `sock` into file object `fout`.

    Proto v2 (FLAG_PAGECRC set): each page arrives as [page_size data bytes]
    [4-byte CRC32 of that data]. We verify the seal, record any page whose CRC
    mismatches in `bad_pages`, and ALWAYS write the data (even when bad) so a
    later majority vote across dumps still has the bytes. Proto v1: a raw byte
    stream with no per-page seal.

    `progress`, if given, is called as progress(pages_received, bytes_received).
    Returns a ReceiveResult. Sets `truncated` if the stream ended early.
    """
    page_size = geom["page_size"]
    total_pages = geom["total_pages"]
    total_bytes = geom["total_bytes"]

    if geom["flags"] & FLAG_PAGECRC:
        bad_pages, pages_received, bytes_received, truncated = [], 0, 0, False
        for i in range(total_pages):
            try:
                frame = recv_exact(sock, page_size + 4)
            except (socket.timeout, OSError):
                truncated = True
                break
            data = frame[:page_size]
            (crc_rx,) = struct.unpack("<I", frame[page_size:])
            if zlib.crc32(data) & 0xFFFFFFFF != crc_rx:
                bad_pages.append(i)
            fout.write(data)
            pages_received += 1
            bytes_received += page_size
            if progress:
                progress(pages_received, bytes_received)
        return ReceiveResult(bytes_received, pages_received, bad_pages, truncated)

    # Proto v1: unframed byte stream.
    bytes_received, truncated = 0, False
    while bytes_received < total_bytes:
        try:
            chunk = sock.recv(min(65536, total_bytes - bytes_received))
        except socket.timeout:
            truncated = True
            break
        if not chunk:
            truncated = True
            break
        fout.write(chunk)
        bytes_received += len(chunk)
        if progress:
            progress(bytes_received // page_size, bytes_received)
    return ReceiveResult(bytes_received, bytes_received // page_size, [], truncated)


def write_badpages(out_path, bad_pages, total_pages):
    """Write the <out_path>.badpages.json sidecar listing pages that failed
    their CRC seal. Only called when there is at least one bad page."""
    with open(out_path + ".badpages.json", "w") as f:
        json.dump({"total_pages": total_pages,
                   "bad_page_count": len(bad_pages),
                   "bad_pages": bad_pages}, f)


def dump_family(flags):
    """The chip family a dump header's flags describe."""
    if flags & FLAG_EEPROM:
        return "i2c-eeprom" if flags & FLAG_I2C else "spi-eeprom"
    return "spi-nor" if flags & FLAG_NOR else "spi-nand"


def write_metadata(out_path, geom, byte_count, result=None, profile=None):
    """Write the <out_path>.meta.json sidecar next to the dump."""
    meta = {
        "geometry": geom,
        "ecc_on": bool(geom["flags"] & FLAG_ECC_ON),
        "quad": bool(geom["flags"] & FLAG_QUAD),
        "verify": bool(geom["flags"] & FLAG_VERIFY),
        "page_crc": bool(geom["flags"] & FLAG_PAGECRC),
        "family": dump_family(geom["flags"]),
        "proto_version": geom.get("proto_version"),
        "bytes_received": byte_count,
        "timestamp": datetime.datetime.now().isoformat(),
    }
    if profile is not None:
        meta["profile"] = profile
    if result is not None:
        meta["bad_page_count"] = len(result.bad_pages)
        meta["truncated"] = result.truncated
    with open(out_path + ".meta.json", "w") as f:
        json.dump(meta, f, indent=2)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="ESP32 SPI NAND dumper — PC-side TCP receiver.")
    ap.add_argument("--ip", help="ESP32 IP address (shown in the serial monitor).")
    ap.add_argument("--port", type=int, help="TCP port (default 3333).")
    ap.add_argument("--out-dir", dest="out_dir",
                    help="Directory for dumps (default target/).")
    ap.add_argument("--chip", help="Push this chip's profile from db/ before dumping "
                    "(the device still checks it against the chip's ID). Remembered "
                    "for this ID in dump.config.json.")
    return ap.parse_args(argv)


def prepare_profile(sock, chip, saved):
    """Run the pre-dump session. Returns the profile identity for the meta
    sidecar, or None for pre-v4 firmware. `saved` is dump.config.json; a chip
    picked by --chip or at the prompt is remembered there for this ID."""
    from tools import chipdb
    info = query_info(sock)
    if info is None:
        if chip:
            raise SessionError("I", "this firmware predates profile push; "
                               "drop --chip or flash v4")
        print("[*] Pre-v4 firmware (no 'I' reply): dumping with its built-in table.")
        return None
    ident = " ".join(f"0x{b:02X}" for b in detected_id(info, info["family"]))
    if info["state"] in ("unknown", "ambiguous") and info.get("nor_id"):
        ident = ("NAND view " + " ".join(f"0x{b:02X}" for b in detected_id(info, "spi-nand"))
                 + ", NOR view " + " ".join(f"0x{b:02X}" for b in info["nor_id"]))
    print(f"[*] Device detected {ident}: {info['state']}"
          + (f" ({info['name']}, {info['family']})"
             if info["state"] in ("resident", "pushed", "sfdp") else ""))

    try:
        flat, remember = resolve_profile(info, chip, saved,
                                         ask_candidate if sys.stdin.isatty() else None)
    except chipdb.AmbiguousId as e:
        raise SessionError("I", f"{e}; pick one with --chip NAME") from None
    except chipdb.ChipDBError as e:
        raise SessionError("I", str(e)) from None
    except ImportError:
        raise SessionError("I", "the chip database needs PyYAML: pip install pyyaml") from None

    if flat is None:
        if info["state"] in ("unknown", "ambiguous"):
            print("[!] Chip not in the database: the device will dump with the geometry "
                  "set in its serial menu.")
            try:
                for h in eeprom_hint(info, chipdb.load_db()):
                    print(f"    Hint: {h}. Name the part with --chip.")
            except chipdb.ChipDBError:
                pass
        return {"name": info["name"], "source": info["state"], "family": info["family"]}

    blob = chipdb.pack_blob(flat)
    print(f"[*] Pushing profile {flat['name']} ({len(blob)} B, CRC "
          f"0x{struct.unpack('<I', blob[-4:])[0]:08X})...")
    push_and_arm(sock, flat, blob, info)
    print(f"[+] Device verified and armed {flat['name']}.")
    if remember:
        did = detected_id(info, flat["family"]) or (flat["id_mfr"], flat["id_dev"], flat["id_dev2"])
        saved["chips"] = dict(saved.get("chips") or {},
                              **{id_key(did[0], did[1], flat["family"], did[2]): flat["name"]})
        save_config(CONFIG_PATH, saved)
    return {"name": flat["name"], "source": "pushed", "family": flat["family"],
            "profile": flat["profile"], "ecc_scheme": flat["ecc_scheme"],
            "schema_ver": chipdb.SCHEMA_VER}


def main(argv=None):
    args = parse_args(argv)
    cli = {"ip": args.ip, "port": args.port, "out_dir": args.out_dir}
    saved = load_config()
    cfg = resolve_config(cli, saved)
    # Any flag the user passed is remembered, so next run needs no flags.
    if any(v is not None for v in cli.values()):
        saved = dict(saved, **cfg)
        save_config(CONFIG_PATH, saved)
        print(f"[*] Saved connection settings to {os.path.basename(CONFIG_PATH)}")
    ip, port, out_dir = cfg["ip"], cfg["port"], cfg["out_dir"]

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    os.makedirs(out_dir or '.', exist_ok=True)

    print(f"[*] Connecting to ESP32 at {ip}:{port}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((ip, port))
    except Exception as e:
        print(f"[!] Could not connect: {e}")
        sys.exit(1)

    print("[+] Connected!")
    try:
        profile = prepare_profile(sock, args.chip, saved)
    except (SessionError, IOError, ValueError) as e:
        print(f"[!] {e}")
        sock.close()
        sys.exit(1)

    print("[*] Sending 'GO' trigger...")
    sock.sendall(b'G')
    first = recv_exact(sock, 4)
    if first == RESP_MAGIC:
        _, status, _ = read_response(sock, first)
        print(f"[!] Device refused the dump: {prf_error_name(status)}")
        sock.close()
        sys.exit(1)

    geom = parse_header(first + recv_exact(sock, HEADER_SIZE - 4))
    total_bytes = geom["total_bytes"]
    total_mb = total_bytes / (1024 * 1024)
    page_size = geom["page_size"]
    pagecrc = bool(geom["flags"] & FLAG_PAGECRC)
    nor = bool(geom["flags"] & FLAG_NOR)
    family = dump_family(geom["flags"])
    prefix = {"spi-nand": "nand", "spi-nor": "nor"}.get(family, "eeprom")
    out_file = os.path.join(out_dir, f"{prefix}_raw_dump_{stamp}.bin")
    if family in EEPROM_FAMILIES:
        where = (f"at I2C 0x{geom['mfr_id']:02X}" if family == "i2c-eeprom" else "on SPI")
        print(f"[*] Serial EEPROM {where} | {total_bytes} bytes in {page_size} B units")
    elif nor:
        print(f"[*] SPI NOR 0x{geom['mfr_id']:02X} 0x{geom['dev_id']:02X} | "
              f"{total_bytes >> 10} KiB in {page_size} B units "
              f"| {'quad' if geom['flags'] & FLAG_QUAD else 'single'}")
    else:
        print(f"[*] Chip 0x{geom['mfr_id']:02X} 0x{geom['dev_id']:02X} | "
              f"{geom['total_blocks']} blocks x {geom['pages_per_block']} x {page_size} B "
              f"| ECC {'on' if geom['flags'] & FLAG_ECC_ON else 'off'} "
              f"| {'quad' if geom['flags'] & FLAG_QUAD else 'single'}")
    print(f"[*] Proto v{geom['proto_version']} | "
          f"per-page CRC {'on' if pagecrc else 'off'} | expecting {total_mb:.2f} MB.")

    start_time = time.time()

    def progress(pages_done, bytes_done):
        if pages_done % PROGRESS_INTERVAL:
            return
        mb = bytes_done / (1024 * 1024)
        el = time.time() - start_time
        rate = bytes_done / el if el > 0 else 0            # bytes/sec
        eta = (total_bytes - bytes_done) / rate if rate > 0 else 0
        print(f"\r[>] {mb:.1f}/{total_mb:.0f} MB "
              f"({bytes_done / total_bytes * 100:.1f}%) {rate / (1024*1024):.2f} MB/s | "
              f"elapsed {format_duration(el)} | ETA {format_duration(eta)}   ",
              end="", flush=True)

    try:
        with open(out_file, 'wb') as f:
            res = receive_pages(sock, geom, f, progress)

        write_metadata(out_file, geom, res.bytes_received, res, profile)
        el = time.time() - start_time
        avg = res.bytes_received / el / (1024 * 1024) if el > 0 else 0
        print(f"\n[*] Dump complete! Saved to {out_file}")
        print(f"[*] Metadata: {out_file}.meta.json")
        print(f"[*] Received {res.bytes_received} bytes "
              f"({res.pages_received}/{geom['total_pages']} pages) in "
              f"{format_duration(el)} ({avg:.2f} MB/s avg)")
        if res.truncated or res.bytes_received < total_bytes:
            print(f"[!] WARNING: truncated — expected {total_bytes} "
                  f"but got {res.bytes_received} bytes.")
        if pagecrc:
            if res.bad_pages:
                write_badpages(out_file, res.bad_pages, geom["total_pages"])
                print(f"[!] {len(res.bad_pages)} page(s) FAILED their CRC seal — "
                      f"see {out_file}.badpages.json")
            else:
                print("[+] All pages passed their CRC seal.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
