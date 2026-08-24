"""Load / validate / flatten the three-layer NAND chip database (db/)."""
import glob
import os
import struct
import yaml

DB_ROOT = os.path.join(os.path.dirname(__file__), "..", "db")


class RefError(Exception):
    pass


class ValidationError(Exception):
    pass


class AmbiguousID(Exception):
    def __init__(self, candidates):
        self.candidates = candidates
        super().__init__("ambiguous JEDEC id: " + ", ".join(c["name"] for c in candidates))


class DB:
    def __init__(self, families, profiles, chips):
        self.families = families
        self.profiles = profiles
        self.chips = chips


def _load_dir(path):
    out = {}
    for f in sorted(glob.glob(os.path.join(path, "*.yml"))):
        with open(f) as fh:
            doc = yaml.safe_load(fh)
        out[doc["name"]] = doc
    return out


def load(root=DB_ROOT):
    return DB(_load_dir(os.path.join(root, "families")),
              _load_dir(os.path.join(root, "profiles")),
              _load_dir(os.path.join(root, "chips")))


def resolve_refs(db):
    for name, c in db.chips.items():
        if c.get("family") not in db.families:
            raise RefError(f"{name}: unknown family {c.get('family')!r}")
        if c.get("profile") not in db.profiles:
            raise RefError(f"{name}: unknown profile {c.get('profile')!r}")
    return db


def get(db, chip_name):
    resolve_refs(db)
    c = dict(db.chips[chip_name])
    c["family"] = db.families[c["family"]]
    c["profile"] = db.profiles[c["profile"]]
    return c


OK, CORR, REFRESH, UNCOR = 0, 1, 2, 3


def _map16(named, default=UNCOR):
    m = [default] * 16
    for i, v in named.items():
        m[i] = v
    return m


# scheme -> (ecc_shift, ecc_mask, 16-entry severity map)
SCHEMES = {
    "generic2": (4, 0x3, _map16({0: OK, 1: CORR, 2: UNCOR, 3: UNCOR})),
    "micron3":  (4, 0x7, _map16({0: OK, 1: CORR, 2: UNCOR, 3: REFRESH, 5: REFRESH})),
    "gd_uc":    (4, 0x7, _map16({0: OK, 1: CORR, 2: CORR, 3: CORR,
                                 4: CORR, 5: REFRESH, 6: REFRESH, 7: UNCOR})),
    "xtx4":     (4, 0xF, _map16({**{i: CORR for i in range(1, 15)}, 0: OK, 15: UNCOR})),
    "xtx_g0xa": (2, 0xF, _map16({**{i: CORR for i in range(1, 16)}, 0: OK, 8: UNCOR, 12: REFRESH})),
}

# main-area capacity window: 512 Mb .. 8 Gb, expressed in bytes (64 MiB .. 1 GiB)
_CAP_MIN = 64 * 1024 * 1024
_CAP_MAX = 1024 * 1024 * 1024


def expand_scheme(name):
    if name not in SCHEMES:
        raise ValidationError(f"unknown ECC scheme {name!r}")
    return SCHEMES[name]


def validate(chip):
    g = chip["geometry"]
    name = chip["name"]
    if len(name) > 23:
        raise ValidationError(f"{name!r}: name exceeds 23 chars (no room for NUL in name[24])")
    if g["spare_size"] >= g["page_size"]:
        raise ValidationError(f"{name}: spare_size must be < page_size")
    ppb = g["pages_per_block"]
    if ppb <= 0 or (ppb & (ppb - 1)) != 0:
        raise ValidationError(f"{name}: pages_per_block must be a power of two")
    if g["total_blocks"] <= 0:
        raise ValidationError(f"{name}: total_blocks must be > 0")
    main = g["page_size"] - g["spare_size"]
    total_main = main * ppb * g["total_blocks"]
    # DEFENSIVE: overflow check subsumed by capacity-window check below; unreachable with valid data
    if total_main > 0xFFFFFFFF:
        raise ValidationError(f"{name}: capacity overflows uint32")
    if not (_CAP_MIN <= total_main <= _CAP_MAX):
        raise ValidationError(f"{name}: main capacity {total_main} outside 512 Mb..8 Gb window")
    shift, mask, _m = expand_scheme(chip["profile"]["ecc"]["scheme"])
    # DEFENSIVE: expand_scheme raises ValidationError first on unknown scheme; shift/mask check unreachable with valid data
    if shift > 7 or mask not in (0x1, 0x3, 0x7, 0xF):
        raise ValidationError(f"{name}: bad ecc shift/mask {shift}/{mask}")
    oob = chip["profile"]["oob_layout"]
    bbm = oob["bbm"]
    if bbm["offset"] + bbm["len"] > g["spare_size"]:
        raise ValidationError(f"{name}: bbm exceeds spare_size")
    # The flat struct caps OOB at 4 regions each (8 uint16 = 4 pairs); reject
    # rather than let _regions_to_pairs silently truncate a longer list.
    if len(oob["free_regions"]) > 4:
        raise ValidationError(f"{name}: oob free_regions exceeds 4")
    if len(oob["ecc_regions"]) > 4:
        raise ValidationError(f"{name}: oob ecc_regions exceeds 4")


PROFILE_SIZE = 110
# Matches active_profile_t (packed, little-endian). Field order is load-bearing.
PACK_FORMAT = "<24s IIII BBBBBB B BB 16B B BB BB B H BBB BB 8H 8H"

_READ_MODE = {"single": 0, "quad": 1}
_ID_METHOD = {"addr": 0, "dummy": 1}


def _regions_to_pairs(regions):
    flat = []
    for off, ln in regions:
        flat += [off, ln]
    flat += [0] * (8 - len(flat))   # pad to 4 (off,len) pairs
    return flat[:8]


def flatten(chip):
    validate(chip)
    g, fam, prof = chip["geometry"], chip["family"], chip["profile"]
    shift, mask, emap = expand_scheme(prof["ecc"]["scheme"])
    ecc_en_bit = prof.get("config_ecc_en_bit")
    if ecc_en_bit is None:
        ecc_en_bit = fam["config_ecc_en_bit"]
    qe = prof.get("qe", {})
    rid = chip.get("read_id") or fam["read_id_default"]
    oob = prof["oob_layout"]
    bbm = oob["bbm"]
    s2 = prof["ecc"].get("status2_reg") or 0
    ops = fam["opcodes"]
    return {
        "name": chip["name"],
        "page_size": g["page_size"], "spare_size": g["spare_size"],
        "pages_per_block": g["pages_per_block"], "total_blocks": g["total_blocks"],
        "op_page_read": ops["page_read"], "op_read_cache": ops["read_cache"]["x1"],
        "op_get_feat": ops["get_feature"], "op_set_feat": ops["set_feature"],
        "op_status_addr": fam["feature_addrs"]["status"], "op_cfg_addr": fam["feature_addrs"]["config"],
        "ecc_en_bit": ecc_en_bit, "ecc_shift": shift, "ecc_mask": mask, "ecc_map": list(emap),
        "status2_reg": s2,
        "id_method": _ID_METHOD[rid["method"]], "id_n_bytes": rid["id_bytes"],
        "qe_addr": qe.get("feature_addr", 0) if qe.get("has") else 0,
        "qe_bit": qe.get("bit", 0) if qe.get("has") else 0,
        "read_mode": _READ_MODE[chip["read_mode"]], "vcc_mv": chip["vcc_mv"],
        "bbm_off": bbm["offset"], "bbm_len": bbm["len"], "bbm_good": bbm["good"],
        "oob_free_n": len(oob["free_regions"]), "oob_ecc_n": len(oob["ecc_regions"]),
        "oob_free": _regions_to_pairs(oob["free_regions"]),
        "oob_ecc": _regions_to_pairs(oob["ecc_regions"]),
    }


def pack(flat):
    return struct.pack(
        PACK_FORMAT,
        flat["name"].encode()[:23].ljust(24, b"\x00"),
        flat["page_size"], flat["spare_size"], flat["pages_per_block"], flat["total_blocks"],
        flat["op_page_read"], flat["op_read_cache"], flat["op_get_feat"], flat["op_set_feat"],
        flat["op_status_addr"], flat["op_cfg_addr"],
        flat["ecc_en_bit"], flat["ecc_shift"], flat["ecc_mask"], *flat["ecc_map"],
        flat["status2_reg"], flat["id_method"], flat["id_n_bytes"],
        flat["qe_addr"], flat["qe_bit"], flat["read_mode"], flat["vcc_mv"],
        flat["bbm_off"], flat["bbm_len"], flat["bbm_good"],
        flat["oob_free_n"], flat["oob_ecc_n"], *flat["oob_free"], *flat["oob_ecc"],
    )


def candidates(db, mfr, dev):
    return [c for c in db.chips.values()
            if c["id"]["mfr"] == mfr and c["id"]["dev"] == dev]


def resolve(db, mfr, dev, dev2=None, cached_name=None):
    cands = candidates(db, mfr, dev)
    if not cands:
        raise RefError(f"no chip for id {mfr:#04x} {dev:#04x}")
    if len(cands) == 1:
        return cands[0]
    if dev2 is not None:                       # rung 1: extra ID byte
        narrowed = [c for c in cands if c["id"].get("dev2") == dev2]
        if len(narrowed) == 1:
            return narrowed[0]
        if narrowed:
            cands = narrowed
    if cached_name is not None:                # rung 3: cached/user choice
        for c in cands:
            if c["name"] == cached_name:
                return c
    # rung 2 (ONFI) is a stub — fail closed with the candidate list
    raise AmbiguousID(cands)
