"""Chip database: load -> validate -> resolve -> flatten -> pack.

The single home for the v4 vendor-profile model (design:
docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md).
The host owns the layered DB (db/families, db/profiles, db/chips); the device
only ever sees one flat `active_profile_t`, produced here and nowhere else.

Stage 1 of the rollout: host only. The firmware still builds from chips.yml.

    python3 tools/chipdb.py                        # validate + list
    python3 tools/chipdb.py --show MT29F2G01ABAGD  # flattened profile
    python3 tools/chipdb.py --blob MT29F2G01ABAGD  # push blob as hex
"""
import argparse
import glob
import os
import struct
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_DIR = os.path.join(REPO, "db")

SCHEMA_VER = 1
MAX_PAGE_BUFFER = 8192          # firmware MAX_PAGE_SIZE (src/main.cpp)
NAME_MAX = 23                   # name[24], NUL-terminated

# ECC severities as the device stores them in ecc_map.
OK, CORR, CORR_REFRESH, UNCOR = 0, 1, 2, 3

# Named ECC status schemes -> (shift, mask, {field value: severity}).
# Any value a scheme does not name decodes to UNCOR: a reserved or unexpected
# value must never pass as a clean read.
SCHEMES = {
    # Generic 2-bit SR[5:4]: 00 ok, 01 corrected, 10 uncorrectable, 11 reserved.
    "generic2": (4, 0x3, {0: OK, 1: CORR, 2: UNCOR}),
    # Micron SR[6:4] (MT29F2G01 Table 9): 001 1-3 corrected, 010 uncorrectable,
    # 011 4-6 corrected (refresh might be needed), 101 7-8 corrected (refresh).
    "micron3": (4, 0x7, {0: OK, 1: CORR, 2: UNCOR, 3: CORR_REFRESH, 5: CORR_REFRESH}),
    # GigaDevice UC SR[6:4]: 1..6 corrected, 7 uncorrectable.
    "gd_uc": (4, 0x7, {0: OK, **{v: CORR for v in range(1, 7)}, 7: UNCOR}),
    # XTX XT26G01C SR[7:4]: 1..8 bitflips corrected, 0xF uncorrectable.
    "xtx4": (4, 0xF, {0: OK, **{v: CORR for v in range(1, 9)}, 0xF: UNCOR}),
    # XTX XT26G0xA SR[5:2] (mainline): 1..7 corrected, 8 uncorrectable,
    # 12 = 8 bitflips corrected (at strength -> refresh).
    "xtx_g0xa": (2, 0xF, {0: OK, **{v: CORR for v in range(1, 8)}, 8: UNCOR,
                          12: CORR_REFRESH}),
}

READ_MODES = {"single": 0, "quad": 1}
ID_METHODS = {"addr": 0, "dummy": 1}
BBM_PAGES = {"first": 0x1, "second": 0x2, "last": 0x4}
PLANES = (1, 2, 4)

# ---- Flat struct layout (little-endian, naturally aligned) -------------------
# Field order == the C active_profile_t that stage 2 adds to the firmware. Every
# field sits at an offset that is a multiple of its size, so the C compiler
# lays it out identically without packing attributes; the test suite checks
# this, and stage 2 adds a static_assert on sizeof().
LAYOUT = [
    ("name", "24s"),
    ("id_mfr", "B"), ("id_dev", "B"), ("id_dev2", "B"), ("id_flags", "B"),
    ("page_size", "I"), ("spare_size", "I"), ("pages_per_block", "I"),
    ("total_blocks", "I"),
    ("op_page_read", "B"), ("op_read_cache", "B"), ("op_read_cache_x4", "B"),
    ("op_get_feat", "B"), ("op_set_feat", "B"), ("op_status_addr", "B"),
    ("op_cfg_addr", "B"),
    ("ecc_en_bit", "B"), ("ecc_shift", "B"), ("ecc_mask", "B"),
    ("ecc_map", "16s"),
    ("status2_reg", "B"), ("id_method", "B"), ("id_n_bytes", "B"),
    ("qe_addr", "B"), ("qe_bit", "B"),
    ("read_mode", "B"), ("planes", "B"), ("_pad0", "x"),
    ("vcc_mv", "H"),
    ("bbm_off", "B"), ("bbm_len", "B"), ("bbm_good", "B"),
    ("oob_free_n", "B"), ("oob_ecc_n", "B"), ("bbm_pages", "B"),
    ("oob_free", "8H"), ("oob_ecc", "8H"),
]
ID_FLAG_HAS_DEV2 = 0x01
STRUCT_FMT = "<" + "".join(f for _, f in LAYOUT)
STRUCT_SIZE = (struct.calcsize(STRUCT_FMT) + 3) // 4 * 4   # C pads to align 4
BLOB_MAGIC = b"PRF"


class ChipDBError(ValueError):
    pass


class AmbiguousId(ChipDBError):
    def __init__(self, candidates):
        self.candidates = candidates
        super().__init__("E_AMBIGUOUS_ID: " + ", ".join(c["name"] for c in candidates))


# ---- Load ---------------------------------------------------------------------
def _load_dir(path):
    import yaml
    out = {}
    for f in sorted(glob.glob(os.path.join(path, "*.yml"))):
        with open(f) as fh:
            doc = yaml.safe_load(fh) or {}
        for name, body in doc.items():
            if name in out:
                raise ChipDBError(f"{name}: defined twice (second in {os.path.basename(f)})")
            out[name] = body
    return out


def load_db(root=DB_DIR):
    return {
        "families": _load_dir(os.path.join(root, "families")),
        "profiles": _load_dir(os.path.join(root, "profiles")),
        "chips": _load_dir(os.path.join(root, "chips")),
    }


# ---- Resolve + flatten ----------------------------------------------------------
def expand_scheme(ecc):
    """Profile `ecc` block -> (shift, mask, 16-entry severity list)."""
    if "scheme" in ecc:
        if ecc["scheme"] not in SCHEMES:
            raise ChipDBError(f"unknown ECC scheme '{ecc['scheme']}'")
        shift, mask, values = SCHEMES[ecc["scheme"]]
    else:
        shift, mask = ecc["status_shift"], ecc["status_mask"]
        values = {int(k): v for k, v in ecc["value_map"].items()}
    ecc_map = [UNCOR] * 16
    for v, sev in values.items():
        ecc_map[v] = sev
    return shift, mask, ecc_map


def _regions(regs, what, name):
    if len(regs) > 4:
        raise ChipDBError(f"{name}: {what} has {len(regs)} regions (max 4)")
    flat = [x for r in regs for x in r] + [0] * (8 - 2 * len(regs))
    return len(regs), flat


def flatten(db, name):
    """Resolve chip -> profile -> family into one flat dict (all LAYOUT fields)."""
    chips = db["chips"]
    if name not in chips:
        raise ChipDBError(f"unknown chip '{name}'")
    c = chips[name]
    for k in ("id", "family", "profile", "geometry", "datasheet"):
        if k not in c:
            raise ChipDBError(f"{name}: missing required field '{k}'")
    if c["family"] not in db["families"]:
        raise ChipDBError(f"{name}: unknown family '{c['family']}'")
    if c["profile"] not in db["profiles"]:
        raise ChipDBError(f"{name}: unknown profile '{c['profile']}'")
    fam, prof = db["families"][c["family"]], db["profiles"][c["profile"]]
    ov = c.get("overrides") or {}
    g, ident, ops = c["geometry"], c["id"], fam["opcodes"]
    shift, mask, ecc_map = expand_scheme(prof["ecc"])
    rid = c.get("read_id") or fam["read_id_default"]
    qe = prof.get("qe") or {"has": False}
    oob = prof["oob_layout"]
    bbm = oob["bbm"]   # keys are offset/length: bare `off` is a YAML 1.1 boolean
    free_n, free = _regions(oob.get("free_regions", []), "free_regions", name)
    ecc_n, eccr = _regions(oob.get("ecc_regions", []), "ecc_regions", name)
    ecc_en = ov.get("config_ecc_en_bit",
                    prof.get("config_ecc_en_bit", fam["config_ecc_en_bit"]))
    dev2 = ident.get("dev2")
    return {
        "name": name, "family": c["family"], "profile": c["profile"],
        "resident": bool(c.get("resident", False)),
        "id_mfr": ident["mfr"], "id_dev": ident["dev"], "id_dev2": dev2 or 0,
        "id_flags": ID_FLAG_HAS_DEV2 if dev2 is not None else 0,
        "page_size": g["page_size"], "spare_size": g["spare_size"],
        "pages_per_block": g["pages_per_block"], "total_blocks": g["total_blocks"],
        "op_page_read": ops["page_read"], "op_read_cache": ops["read_cache"]["x1"],
        "op_read_cache_x4": ops["read_cache"]["x4"],
        "op_get_feat": ops["get_feature"], "op_set_feat": ops["set_feature"],
        "op_status_addr": fam["feature_addrs"]["status"],
        "op_cfg_addr": fam["feature_addrs"]["config"],
        "ecc_en_bit": ecc_en, "ecc_shift": shift, "ecc_mask": mask,
        "ecc_map": ecc_map, "ecc_scheme": prof["ecc"].get("scheme", "custom"),
        "status2_reg": prof["ecc"].get("status2_reg") or 0,
        "id_method": ID_METHODS.get(rid["method"], -1), "id_n_bytes": rid["id_bytes"],
        "qe_addr": qe.get("feature_addr", 0) if qe.get("has") else 0,
        "qe_bit": qe.get("bit", 0) if qe.get("has") else 0,
        "read_mode": READ_MODES.get(c.get("read_mode", "single"), -1),
        "planes": g.get("planes", 1), "vcc_mv": c.get("vcc_mv", 3300),
        "bbm_off": bbm["offset"], "bbm_len": bbm["length"], "bbm_good": bbm["good"],
        "bbm_pages": sum(BBM_PAGES.get(p, 0x80) for p in bbm.get("pages", ["first"])),
        "oob_free_n": free_n, "oob_free": free, "oob_ecc_n": ecc_n, "oob_ecc": eccr,
    }


# ---- Validate (mirrors device tiers 2-3, plus host-only plausibility) -----------
def check_flat(f):
    """Raise ChipDBError on anything the device would reject. Returns warnings."""
    n = f["name"]
    err = lambda msg: ChipDBError(f"{n}: {msg}")   # noqa: E731
    if len(n.encode()) > NAME_MAX:
        raise err(f"name longer than {NAME_MAX} bytes")
    # Tier 2: structural
    if f["ecc_shift"] > 7:
        raise err("ecc_shift > 7")
    if f["ecc_mask"] not in (0x1, 0x3, 0x7, 0xF):
        raise err("ecc_mask must be 0x1, 0x3, 0x7 or 0xF")
    if len(f["ecc_map"]) != 16 or any(s not in (OK, CORR, CORR_REFRESH, UNCOR)
                                      for s in f["ecc_map"]):
        raise err("ecc_map must hold 16 valid severities")
    for key, cnt in (("oob_free", "oob_free_n"), ("oob_ecc", "oob_ecc_n")):
        regs = f[key]
        for i in range(f[cnt]):
            off, ln = regs[2 * i], regs[2 * i + 1]
            if ln == 0 or off + ln > f["spare_size"]:
                raise err(f"{key} region ({off}, {ln}) outside the {f['spare_size']}-byte spare")
    if f["bbm_len"] == 0 or f["bbm_off"] + f["bbm_len"] > f["spare_size"]:
        raise err("bad-block marker outside the spare area")
    if f["bbm_pages"] == 0 or f["bbm_pages"] & 0x80:
        raise err("bbm.pages must list first/second/last")
    for op in ("op_page_read", "op_read_cache", "op_read_cache_x4", "op_get_feat",
               "op_set_feat", "op_status_addr", "op_cfg_addr"):
        if not f[op]:
            raise err(f"{op} is zero")
    if f["read_mode"] < 0:
        raise err("read_mode must be single or quad")
    if f["id_method"] < 0 or not 1 <= f["id_n_bytes"] <= 4:
        raise err("read_id method/id_bytes invalid")
    if f["planes"] not in PLANES:
        raise err("planes must be 1, 2 or 4")
    # Tier 3: physical sanity
    if not 0 < f["spare_size"] < f["page_size"]:
        raise err("need 0 < spare_size < page_size")
    ppb = f["pages_per_block"]
    if ppb <= 0 or ppb & (ppb - 1):
        raise err("pages_per_block must be a power of two")
    if f["total_blocks"] <= 0 or f["total_blocks"] % f["planes"]:
        raise err("total_blocks must be > 0 and divide across planes")
    if f["page_size"] > MAX_PAGE_BUFFER:
        raise err(f"page_size exceeds MAX_PAGE_BUFFER ({MAX_PAGE_BUFFER})")
    if f["page_size"] * ppb * f["total_blocks"] >= 1 << 32:
        raise err("total size overflows uint32")
    # Host-only: market plausibility (fuzzy, so a warning, never a device check)
    warnings = []
    main_bits = (f["page_size"] - f["spare_size"]) * ppb * f["total_blocks"] * 8
    if not (512 << 20) <= main_bits <= (8 << 30):
        warnings.append(f"{n}: capacity {main_bits >> 20} Mbit is outside the usual "
                        "512 Mbit - 8 Gbit SPI NAND range; double-check the geometry")
    return warnings


def validate_db(db):
    """Validate every chip; returns (flats by name, warnings)."""
    flats, warnings = {}, []
    for name in db["chips"]:
        f = flatten(db, name)
        warnings += check_flat(f)
        flats[name] = f
    return flats, warnings


# ---- Identify / disambiguate ------------------------------------------------------
def candidates(db, mfr, dev):
    return [flatten(db, n) for n, c in db["chips"].items()
            if c["id"]["mfr"] == mfr and c["id"]["dev"] == dev]


def identify(db, mfr, dev, dev2=None, cached=None):
    """Resolve a detected JEDEC id to one flat profile (design section 5).

    Ladder: dev2 byte -> (ONFI, reserved) -> cached user choice. Anything left
    ambiguous raises AmbiguousId; nothing is ever guessed."""
    cands = candidates(db, mfr, dev)
    if not cands:
        raise ChipDBError(f"no chip with id 0x{mfr:02X} 0x{dev:02X}")
    if len(cands) > 1 and dev2 is not None:
        by_dev2 = [c for c in cands if c["id_flags"] & ID_FLAG_HAS_DEV2 and c["id_dev2"] == dev2]
        if by_dev2:
            cands = by_dev2
    if len(cands) > 1 and cached:
        cands = [c for c in cands if c["name"] == cached] or cands
    if len(cands) > 1:
        raise AmbiguousId(cands)
    return cands[0]


# ---- Pack ---------------------------------------------------------------------------
def pack_struct(f):
    vals = []
    for field, fmt in LAYOUT:
        if fmt == "x":
            continue
        v = f[field]
        if field == "name":
            v = v.encode()
        elif field == "ecc_map":
            v = bytes(v)
        if fmt == "8H":
            vals.extend(v)
        else:
            vals.append(v)
    body = struct.pack(STRUCT_FMT, *vals)
    return body + b"\x00" * (STRUCT_SIZE - len(body))


def pack_blob(f):
    """[ 'PRF' ][ schema_ver u8 ][ len u16 ][ struct ][ crc32 over all before ]"""
    body = pack_struct(f)
    head = BLOB_MAGIC + struct.pack("<BH", SCHEMA_VER, len(body))
    return head + body + struct.pack("<I", zlib.crc32(head + body) & 0xFFFFFFFF)


def unpack_blob(blob):
    """Inverse of pack_blob with the transport checks the device runs (tier 1)."""
    if len(blob) < 10 or blob[:3] != BLOB_MAGIC:
        raise ChipDBError("E_BAD_MAGIC")
    ver, ln = struct.unpack_from("<BH", blob, 3)
    if ver != SCHEMA_VER:
        raise ChipDBError("E_SCHEMA_VER")
    if ln != STRUCT_SIZE or len(blob) != 6 + ln + 4:
        raise ChipDBError("E_BAD_LEN")
    (crc,) = struct.unpack_from("<I", blob, 6 + ln)
    if zlib.crc32(blob[:6 + ln]) & 0xFFFFFFFF != crc:
        raise ChipDBError("E_BAD_CRC")
    raw = struct.unpack(STRUCT_FMT, blob[6:6 + struct.calcsize(STRUCT_FMT)])
    out, i = {}, 0
    for field, fmt in LAYOUT:
        if fmt == "x":
            continue
        if fmt == "8H":
            out[field] = list(raw[i:i + 8]); i += 8
            continue
        v = raw[i]; i += 1
        if field == "name":
            v = v.rstrip(b"\x00").decode()
        elif field == "ecc_map":
            v = list(v)
        out[field] = v
    return out


def field_offsets():
    """(field, offset, size) for every real field — used to prove C alignment."""
    out, off = [], 0
    for field, fmt in LAYOUT:
        size = struct.calcsize("<" + fmt)
        if fmt != "x":
            out.append((field, off, size))
        off += size
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate and query the chip database.")
    ap.add_argument("--db", default=DB_DIR)
    ap.add_argument("--show", metavar="CHIP")
    ap.add_argument("--blob", metavar="CHIP")
    a = ap.parse_args(argv)
    try:
        db = load_db(a.db)
        flats, warnings = validate_db(db)
    except ChipDBError as e:
        print(f"[!] {e}")
        return 1
    for w in warnings:
        print(f"[~] {w}")
    if a.show:
        for k, v in flats[a.show].items():
            print(f"  {k:18} {v}")
    elif a.blob:
        print(pack_blob(flats[a.blob]).hex())
    else:
        print(f"[+] {len(flats)} chips OK (schema v{SCHEMA_VER}, {STRUCT_SIZE}-byte profile)")
        for f in flats.values():
            print(f"  {f['name']:18} 0x{f['id_mfr']:02X} 0x{f['id_dev']:02X}  "
                  f"{f['profile']:10} {f['ecc_scheme']:9} planes={f['planes']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
