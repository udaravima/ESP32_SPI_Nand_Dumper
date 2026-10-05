"""Chip database: load -> validate -> resolve -> flatten -> pack.

The single home for the v4 vendor-profile model (design:
docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md, SPI
NOR: docs/superpowers/specs/2026-10-05-spi-nor-design.md). The host owns the
layered DB (db/families, db/profiles, db/chips); the device only ever sees one
flat `active_profile_t`, produced here and nowhere else.

The firmware's resident table is generated from here (tools/gen_profiles.py),
so resident and pushed profiles are the same bytes.

    python3 tools/chipdb.py                        # validate + summary
    python3 tools/chipdb.py --list [--family spi-nor]
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

SCHEMA_VER = 2                  # v2: family byte + SPI NOR address/dummy/QER tail
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
ID_METHODS = {"addr": 0, "dummy": 1, "none": 2}   # none: 9Fh then data (SPI NOR)
FAMILIES = {"spi-nand": 0, "spi-nor": 1}
# How a SPI NOR part above 16 MiB is addressed (flat `addr4_mode`).
ADDR4_MODES = {"none": 0, "native": 1, "enter": 2, "enter_wren": 3}
QER_MAX = 6                     # JESD216 BFPT DWORD15 quad-enable requirement codes
NOR_MAX_DUMMY = 32
BBM_PAGES = {"first": 0x1, "second": 0x2, "last": 0x4}
PLANES = (1, 2, 4)

# ---- Flat struct layout (little-endian, naturally aligned) -------------------
# Field order == active_profile_t in src/nand_profile.h. Every field sits at an
# offset that is a multiple of its size, so the C compiler lays it out
# identically without packing attributes; the test suite checks this, the C
# header static_asserts it, and the native golden-blob test proves it.
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
    ("read_mode", "B"), ("planes", "B"), ("family", "B"),
    ("vcc_mv", "H"),
    ("bbm_off", "B"), ("bbm_len", "B"), ("bbm_good", "B"),
    ("oob_free_n", "B"), ("oob_ecc_n", "B"), ("bbm_pages", "B"),
    ("oob_free", "8H"), ("oob_ecc", "8H"),
    # v2 tail, SPI NOR only (zero for spi-nand)
    ("addr_bytes", "B"), ("addr4_mode", "B"), ("dummy_x1", "B"), ("dummy_x4", "B"),
    ("qer", "B"), ("_pad1", "5x"),
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
    for f in sorted(glob.glob(os.path.join(path, "**", "*.yml"), recursive=True)):
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


# Flat fields that only one family uses; the other family packs them as zero.
NAND_ONLY_ZERO = {
    "op_page_read": 0, "op_status_addr": 0, "op_cfg_addr": 0, "ecc_en_bit": 0,
    "ecc_shift": 0, "ecc_mask": 0, "ecc_map": [OK] * 16, "ecc_scheme": "none",
    "status2_reg": 0, "qe_addr": 0, "qe_bit": 0, "planes": 1,
    "bbm_off": 0, "bbm_len": 0, "bbm_good": 0, "bbm_pages": 0,
    "oob_free_n": 0, "oob_free": [0] * 8, "oob_ecc_n": 0, "oob_ecc": [0] * 8,
}
NOR_ONLY_ZERO = {"addr_bytes": 0, "addr4_mode": 0, "dummy_x1": 0, "dummy_x4": 0, "qer": 0}


def flatten(db, name):
    """Resolve chip -> profile -> family into one flat dict (all LAYOUT fields)."""
    chips = db["chips"]
    if name not in chips:
        raise ChipDBError(f"unknown chip '{name}'")
    c = chips[name]
    for k in ("id", "family", "profile", "geometry"):
        if k not in c:
            raise ChipDBError(f"{name}: missing required field '{k}'")
    if not (c.get("datasheet") or c.get("source")):
        # Provenance is mandatory: a datasheet for hand-written chips, a source
        # link for chips imported from a public database.
        raise ChipDBError(f"{name}: missing required field 'datasheet' (or 'source')")
    if c["family"] not in db["families"]:
        raise ChipDBError(f"{name}: unknown family '{c['family']}'")
    if c["family"] not in FAMILIES:
        raise ChipDBError(f"{name}: family '{c['family']}' has no flattener")
    if c["profile"] not in db["profiles"]:
        raise ChipDBError(f"{name}: unknown profile '{c['profile']}'")
    fam, prof = db["families"][c["family"]], db["profiles"][c["profile"]]
    if c["family"] == "spi-nor":
        return _flatten_nor(name, c, fam, prof)
    return _flatten_nand(name, c, fam, prof)


def _flatten_nand(name, c, fam, prof):
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
        "family_code": FAMILIES["spi-nand"], **NOR_ONLY_ZERO,
    }


def _flatten_nor(name, c, fam, prof):
    """SPI NOR: linear address space read in `read_unit` frames, no spare/ECC.

    The flat geometry is the dump framing, not the erase map: page_size is one
    read unit (4 KiB), pages_per_block groups 16 of them (a 64 KiB block)."""
    ident, ops, g = c["id"], fam["opcodes"], c["geometry"]
    size = g["size_kib"] * 1024
    unit = min(fam["read_unit"], size)
    ppb = max(1, min(16, size // unit))
    mode = c.get("addr4", "none")
    if mode not in ADDR4_MODES:
        raise ChipDBError(f"{name}: addr4 must be one of {', '.join(ADDR4_MODES)}")
    read = ops["read_4b"] if mode == "native" else ops["read"]
    rid = c.get("read_id") or fam["read_id_default"]
    dev2 = ident.get("dev2")
    return {
        "name": name, "family": c["family"], "profile": c["profile"],
        "resident": bool(c.get("resident", False)),
        "id_mfr": ident["mfr"], "id_dev": ident["dev"], "id_dev2": dev2 or 0,
        "id_flags": ID_FLAG_HAS_DEV2 if dev2 is not None else 0,
        "page_size": unit, "spare_size": 0, "pages_per_block": ppb,
        "total_blocks": size // (unit * ppb), "size_bytes": size,
        "op_read_cache": read["x1"], "op_read_cache_x4": read["x4"],
        "op_get_feat": ops["read_sr"], "op_set_feat": ops["write_sr"],
        "id_method": ID_METHODS.get(rid["method"], -1), "id_n_bytes": rid["id_bytes"],
        "read_mode": READ_MODES.get(c.get("read_mode", "single"), -1),
        "vcc_mv": c.get("vcc_mv", 3300),
        **NAND_ONLY_ZERO,
        "family_code": FAMILIES["spi-nor"],
        "addr_bytes": 4 if mode != "none" else 3, "addr4_mode": ADDR4_MODES[mode],
        "dummy_x1": fam["dummy_cycles"]["x1"], "dummy_x4": fam["dummy_cycles"]["x4"],
        "qer": prof.get("qer") or 0,
    }


# ---- Validate (mirrors device tiers 2-3, plus host-only plausibility) -----------
def check_flat(f):
    """Raise ChipDBError on anything the device would reject. Returns warnings."""
    n = f["name"]
    err = lambda msg: ChipDBError(f"{n}: {msg}")   # noqa: E731
    if len(n.encode()) > NAME_MAX:
        raise err(f"name longer than {NAME_MAX} bytes")
    if f["family_code"] == FAMILIES["spi-nor"]:
        return _check_nor(f, err)
    if any(f[k] for k in NOR_ONLY_ZERO):
        raise err("SPI NOR fields set on a spi-nand profile")
    if f["id_method"] == ID_METHODS["none"]:
        raise err("read_id method 'none' is SPI NOR only")
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


def _check_nor(f, err):
    """Tier 2/3 for spi-nor (mirrors nand_profile_check's NOR branch)."""
    if any(f[k] != v for k, v in NAND_ONLY_ZERO.items() if k != "ecc_scheme"):
        raise err("spi-nand fields set on a spi-nor profile")
    for op in ("op_read_cache", "op_read_cache_x4", "op_get_feat", "op_set_feat"):
        if not f[op]:
            raise err(f"{op} is zero")
    if f["read_mode"] < 0:
        raise err("read_mode must be single or quad")
    if f["id_method"] != ID_METHODS["none"] or f["id_n_bytes"] != 3:
        raise err("SPI NOR reads a plain 3-byte JEDEC ID (method none, id_bytes 3)")
    if f["addr_bytes"] not in (3, 4) or (f["addr_bytes"] == 4) != (f["addr4_mode"] != 0):
        raise err("addr_bytes 4 needs an addr4 mode, 3 needs none")
    if f["addr4_mode"] > max(ADDR4_MODES.values()):
        raise err("unknown addr4 mode")
    if f["dummy_x1"] > NOR_MAX_DUMMY or f["dummy_x4"] > NOR_MAX_DUMMY:
        raise err(f"dummy cycles above {NOR_MAX_DUMMY}")
    if not 0 <= f["qer"] <= QER_MAX:
        raise err(f"qer must be a JESD216 code 0..{QER_MAX}")
    page, ppb, blocks = f["page_size"], f["pages_per_block"], f["total_blocks"]
    if f["spare_size"] != 0:
        raise err("SPI NOR has no spare area (spare_size 0)")
    if page <= 0 or page & (page - 1) or ppb <= 0 or ppb & (ppb - 1) or blocks <= 0:
        raise err("read unit and pages_per_block must be powers of two, blocks > 0")
    if page > MAX_PAGE_BUFFER:
        raise err(f"page_size exceeds MAX_PAGE_BUFFER ({MAX_PAGE_BUFFER})")
    size = page * ppb * blocks
    if size >= 1 << 32:
        raise err("total size overflows uint32")
    if size > 1 << 24 and f["addr_bytes"] != 4:
        raise err("over 16 MiB needs 4-byte addressing (addr4: native/enter)")
    warnings = []
    if not 32 << 10 <= size <= 256 << 20:
        warnings.append(f"{f['name']}: size {size >> 10} KiB is outside the usual "
                        "32 KiB - 256 MiB SPI NOR range; double-check the geometry")
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
def candidates(db, mfr, dev, family=None, dev2=None):
    """Chips with this (mfr, dev). A chip that declares dev2 is only a
    candidate when dev2 is unknown or matches it (as the device's lookup)."""
    out = []
    for n, c in db["chips"].items():
        i = c["id"]
        if i["mfr"] != mfr or i["dev"] != dev:
            continue
        if family is not None and c["family"] != family:
            continue
        if dev2 is not None and i.get("dev2") is not None and i["dev2"] != dev2:
            continue
        out.append(flatten(db, n))
    return out


def identify(db, mfr, dev, dev2=None, cached=None, family=None):
    """Resolve a detected JEDEC id to one flat profile (design section 5).

    Ladder: dev2 byte -> (ONFI, reserved) -> cached user choice. Anything left
    ambiguous raises AmbiguousId; nothing is ever guessed. `family` limits the
    search to one bus family (SPI NAND and SPI NOR IDs are read differently)."""
    cands = candidates(db, mfr, dev, family, dev2)
    if not cands:
        raise ChipDBError(f"no chip with id 0x{mfr:02X} 0x{dev:02X}"
                          + (f" 0x{dev2:02X}" if dev2 is not None else ""))
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
        if fmt.endswith("x"):
            continue
        v = f["family_code"] if field == "family" else f[field]
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
        if fmt.endswith("x"):
            continue
        if fmt == "8H":
            out[field] = list(raw[i:i + 8]); i += 8
            continue
        v = raw[i]; i += 1
        if field == "name":
            v = v.rstrip(b"\x00").decode()
        elif field == "ecc_map":
            v = list(v)
        out["family_code" if field == "family" else field] = v
    return out


def field_offsets():
    """(field, offset, size) for every real field — used to prove C alignment."""
    out, off = [], 0
    for field, fmt in LAYOUT:
        size = struct.calcsize("<" + fmt)
        if not fmt.endswith("x"):
            out.append((field, off, size))
        off += size
    return out


def shared_ids(flats):
    """Groups of chips the device can't tell apart by (family, mfr, dev, dev2)."""
    groups = {}
    for f in flats.values():
        key = (f["family"], f["id_mfr"], f["id_dev"],
               f["id_dev2"] if f["id_flags"] & ID_FLAG_HAS_DEV2 else None)
        groups.setdefault(key, []).append(f["name"])
    return {k: v for k, v in groups.items() if len(v) > 1}


def _list_row(f):
    ident = f"0x{f['id_mfr']:02X} 0x{f['id_dev']:02X}"
    ident += f" 0x{f['id_dev2']:02X}" if f["id_flags"] & ID_FLAG_HAS_DEV2 else "     "
    if f["family"] == "spi-nor":
        size = f["page_size"] * f["pages_per_block"] * f["total_blocks"]
        detail = (f"{size >> 10:>7} KiB  addr{f['addr_bytes']}  "
                  f"{f['vcc_mv']} mV{'  resident' if f['resident'] else ''}")
    else:
        detail = f"{f['ecc_scheme']:9} planes={f['planes']}{'  resident' if f['resident'] else ''}"
    return f"  {f['name']:23} {ident}  {f['profile']:14} {detail}"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate and query the chip database.")
    ap.add_argument("--db", default=DB_DIR)
    ap.add_argument("--show", metavar="CHIP")
    ap.add_argument("--blob", metavar="CHIP")
    ap.add_argument("--list", action="store_true", help="list every chip")
    ap.add_argument("--family", choices=sorted(FAMILIES), help="limit --list to one family")
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
        for fam in FAMILIES:
            sub = [f for f in flats.values() if f["family"] == fam]
            if sub:
                print(f"    {fam}: {len(sub)} chips, "
                      f"{sum(f['resident'] for f in sub)} resident")
        shared = shared_ids(flats)
        if shared:
            print(f"[~] {len(shared)} IDs are shared by several chips (the device asks "
                  "the host to pick; see --list)")
        for f in flats.values():
            # Without --list, show the short spi-nand table only.
            if a.list and a.family in (None, f["family"]) or \
                    not a.list and f["family"] == "spi-nand":
                print(_list_row(f))
    return 0


if __name__ == "__main__":
    sys.exit(main())
