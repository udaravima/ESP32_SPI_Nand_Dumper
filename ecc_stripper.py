"""Strip spare/OOB from a raw NAND dump into a main-area image.

Geometry comes from the dump's .meta.json sidecar (written by dump.py),
or from --page-size/--spare-size/--pages-per-block overrides.

The bad-block marker (offset, length, good value, which pages to check) is
resolved from the dump's profile via `chipdb`, when --meta is given and the
chip is present in db/. Otherwise it falls back to the legacy default: offset
0, length 1, good 0xFF, checking only each block's first page.

Usage:
    python3 ecc_stripper.py raw.bin clean.bin --meta raw.bin.meta.json
    python3 ecc_stripper.py raw.bin clean.bin --page-size 2176 --spare-size 128 --pages-per-block 64
"""
import argparse
import json
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))


def resolve_bbm(meta):
    """Re-resolve the chip's bad-block-marker spec from the dump metadata via chipdb.
    Returns {offset, len, good, pages} or None if the chip is not in db/."""
    g = meta.get("geometry", {})
    if "mfr_id" not in g or "dev_id" not in g:
        return None
    try:
        import chipdb
        db = chipdb.load()
        chip = chipdb.resolve(db, g["mfr_id"], g["dev_id"])
        return chipdb.get(db, chip["name"])["profile"]["oob_layout"]["bbm"]
    except Exception:
        return None


def strip(in_path, out_path, page_size, spare_size, pages_per_block, bbm=None):
    if bbm is None:
        bbm = {"offset": 0, "len": 1, "good": 0xFF, "pages": ["first"]}
    main = page_size - spare_size
    off, blen, good = bbm["offset"], bbm["len"], bbm["good"]
    check_first = "first" in bbm.get("pages", ["first"])
    total_pages = os.path.getsize(in_path) // page_size
    bad_blocks = []
    with open(in_path, "rb") as raw, open(out_path, "wb") as clean:
        for idx in range(total_pages):
            page = raw.read(page_size)
            block = idx // pages_per_block
            if check_first and idx % pages_per_block == 0:
                marker = page[main + off: main + off + blen]
                if any(b != good for b in marker):
                    bad_blocks.append(block)
            if block in bad_blocks:
                clean.write(bytes([good]) * main)
            else:
                clean.write(page[:main])
    return bad_blocks


def main():
    ap = argparse.ArgumentParser(description="Strip spare/OOB from a raw NAND dump")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--meta", help="path to <dump>.meta.json")
    ap.add_argument("--page-size", type=int)
    ap.add_argument("--spare-size", type=int)
    ap.add_argument("--pages-per-block", type=int)
    a = ap.parse_args()
    bbm = None
    if a.meta:
        with open(a.meta) as f:
            meta = json.load(f)
        g = meta.get("geometry", {})
        ps, ss, ppb = g["page_size"], g["spare_size"], g["pages_per_block"]
        bbm = resolve_bbm(meta)
    else:
        if None in (a.page_size, a.spare_size, a.pages_per_block):
            ap.error("provide --meta OR all of --page-size/--spare-size/--pages-per-block")
        ps, ss, ppb = a.page_size, a.spare_size, a.pages_per_block
    bad = strip(a.input, a.output, ps, ss, ppb, bbm=bbm)
    print(f"[*] {len(bad)} bad block(s): {bad}")
    print(f"[*] wrote {a.output}")


if __name__ == "__main__":
    main()
