"""Generate src/nand_chips_generated.h from chips.yml.

Runs standalone (`python tools/gen_chips.py chips.yml src/nand_chips_generated.h`)
and as a PlatformIO pre: hook (extra_scripts).
"""
import math

REQUIRED = ["mfr_id", "dev_id", "page_size", "spare_size", "pages_per_block",
            "total_blocks", "bad_block_mark", "has_qe_bit", "ecc_default", "vcc_mv"]


def validate(chips):
    seen = {}
    for name, c in chips.items():
        for k in REQUIRED:
            if k not in c:
                raise ValueError(f"{name}: missing required field '{k}'")
        if c["spare_size"] >= c["page_size"]:
            raise ValueError(f"{name}: spare_size must be < page_size")
        ppb = c["pages_per_block"]
        if ppb <= 0 or (ppb & (ppb - 1)) != 0:
            raise ValueError(f"{name}: pages_per_block must be a power of two")
        key = (c["mfr_id"], c["dev_id"])
        if key in seen:
            raise ValueError(f"{name}: duplicate JEDEC id, also used by {seen[key]}")
        seen[key] = name
        if c["has_qe_bit"]:
            for k in ("qe_feature_addr", "qe_bit"):
                if k not in c:
                    raise ValueError(f"{name}: has_qe_bit requires '{k}'")


def _row(name, c):
    ppb = c["pages_per_block"]
    bits = int(math.log2(ppb))
    ecc = "true" if str(c["ecc_default"]).lower() == "on" else "false"
    qe = "true" if c["has_qe_bit"] else "false"
    qa = c.get("qe_feature_addr", 0)
    qb = c.get("qe_bit", 0)
    return ('{ "%s", 0x%02X, 0x%02X, %d, %d, %d, %d, %d, 0x%02X, %s, 0x%02X, 0x%02X, %s, %d }'
            % (name, c["mfr_id"], c["dev_id"], c["page_size"], c["spare_size"],
               ppb, c["total_blocks"], bits, c["bad_block_mark"], qe, qa, qb,
               ecc, c["vcc_mv"]))


def render_header(chips):
    validate(chips)
    rows = ",\n  ".join(_row(n, c) for n, c in chips.items())
    return (
        "// AUTO-GENERATED from chips.yml by tools/gen_chips.py. DO NOT EDIT.\n"
        "#pragma once\n"
        '#include "nand_chips.h"\n\n'
        "static const nand_chip_t CHIPS[] = {\n  " + rows + "\n};\n"
        "static const unsigned CHIPS_COUNT = sizeof(CHIPS) / sizeof(CHIPS[0]);\n"
    )


def load_chips(path):
    import yaml
    with open(path) as f:
        doc = yaml.safe_load(f)
    return doc["chips"]


def generate(yml_path, out_path):
    header = render_header(load_chips(yml_path))
    with open(out_path, "w") as f:
        f.write(header)
    print(f"[gen_chips] wrote {out_path} ({len(load_chips(yml_path))} chips)")


# PlatformIO pre: hook entry point
try:
    Import("env")  # type: ignore  # noqa: F821 — injected by PlatformIO/SCons
    try:
        import yaml  # noqa: F401
    except ImportError:
        env.Execute("$PYTHONEXE -m pip install pyyaml")  # type: ignore # noqa: F821
    generate("chips.yml", "src/nand_chips_generated.h")
except NameError:
    pass

if __name__ == "__main__":
    import sys
    generate(sys.argv[1], sys.argv[2])
