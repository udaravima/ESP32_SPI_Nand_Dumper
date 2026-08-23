"""Load / validate / flatten the three-layer NAND chip database (db/)."""
import glob
import os
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
    if total_main > 0xFFFFFFFF:
        raise ValidationError(f"{name}: capacity overflows uint32")
    if not (_CAP_MIN <= total_main <= _CAP_MAX):
        raise ValidationError(f"{name}: main capacity {total_main} outside 512 Mb..8 Gb window")
    shift, mask, _m = expand_scheme(chip["profile"]["ecc"]["scheme"])
    if shift > 7 or mask not in (0x1, 0x3, 0x7, 0xF):
        raise ValidationError(f"{name}: bad ecc shift/mask {shift}/{mask}")
    oob = chip["profile"]["oob_layout"]
    bbm = oob["bbm"]
    if bbm[False] + bbm["len"] > g["spare_size"]:
        raise ValidationError(f"{name}: bbm exceeds spare_size")
    # The flat struct caps OOB at 4 regions each (8 uint16 = 4 pairs); reject
    # rather than let _regions_to_pairs silently truncate a longer list.
    if len(oob["free_regions"]) > 4:
        raise ValidationError(f"{name}: oob free_regions exceeds 4")
    if len(oob["ecc_regions"]) > 4:
        raise ValidationError(f"{name}: oob ecc_regions exceeds 4")
