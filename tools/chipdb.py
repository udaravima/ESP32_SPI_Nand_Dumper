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
