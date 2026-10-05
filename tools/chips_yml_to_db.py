"""One-shot converter: a v3 chips.yml -> v4 db/chips/*.yml entries.

v4 moved the chip registry to db/ (family -> profile -> chip). A v3 entry has
identity and geometry but no vendor knowledge (ECC decode, QE bit, OOB layout),
so you name the profile that carries it:

    python3 tools/chips_yml_to_db.py my_chips.yml --profile micron
    python3 tools/chipdb.py            # then validate

Existing db/chips/<name>.yml files are never overwritten.
"""
import argparse
import os
import sys

import yaml

try:
    from tools import chipdb
except ImportError:            # run as a script
    import chipdb


def convert(legacy, profile, datasheet="UNCITED: add the datasheet path or URL"):
    """legacy chips.yml `chips:` mapping -> {name: v4 chip entry}."""
    out = {}
    for name, c in legacy.items():
        out[name] = {
            "id": {"mfr": c["mfr_id"], "dev": c["dev_id"], "dev2": None, "onfi": None},
            "family": "spi-nand",
            "profile": profile,
            "geometry": {k: c[k] for k in ("page_size", "spare_size", "pages_per_block",
                                           "total_blocks")} | {"planes": c.get("planes", 1)},
            "read_mode": "single",
            "vcc_mv": c.get("vcc_mv", 3300),
            "resident": False,
            "datasheet": datasheet,
            "notes": c.get("notes", ""),
        }
    return out


def _dump(name, entry):
    def h(v):
        return "null" if v is None else "0x%02X" % v
    i, g = entry["id"], entry["geometry"]
    return (f"{name}:\n"
            f"  id: {{mfr: {h(i['mfr'])}, dev: {h(i['dev'])}, dev2: null, onfi: null}}\n"
            f"  family: {entry['family']}\n"
            f"  profile: {entry['profile']}\n"
            f"  geometry: {{page_size: {g['page_size']}, spare_size: {g['spare_size']}, "
            f"pages_per_block: {g['pages_per_block']}, total_blocks: {g['total_blocks']}, "
            f"planes: {g['planes']}}}\n"
            f"  read_mode: {entry['read_mode']}\n"
            f"  vcc_mv: {entry['vcc_mv']}\n"
            f"  resident: false\n"
            f"  datasheet: {yaml.safe_dump(entry['datasheet']).splitlines()[0]}\n"
            f"  notes: {yaml.safe_dump(entry['notes']).splitlines()[0]}\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("chips_yml")
    ap.add_argument("--profile", required=True, help="db/profiles entry for these chips")
    ap.add_argument("--db", default=chipdb.DB_DIR)
    a = ap.parse_args(argv)
    with open(a.chips_yml) as fh:
        legacy = yaml.safe_load(fh)["chips"]
    for name, entry in convert(legacy, a.profile).items():
        path = os.path.join(a.db, "chips", f"{name}.yml")
        if os.path.exists(path):
            print(f"[=] {name}: {path} exists, skipped")
            continue
        with open(path, "w") as fh:
            fh.write(_dump(name, entry))
        print(f"[+] {name} -> {path} (cite its datasheet before opening a PR)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
