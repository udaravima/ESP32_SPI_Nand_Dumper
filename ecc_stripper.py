"""Strip spare/OOB from a raw NAND dump into a main-area image.

Geometry comes from the dump's .meta.json sidecar (written by dump.py),
or from --page-size/--spare-size/--pages-per-block overrides.

Bad blocks (first spare byte != good marker on a block's first page) are
replaced with 0xFF padding so filesystem offsets stay aligned.

Usage:
    python3 ecc_stripper.py raw.bin clean.bin --meta raw.bin.meta.json
    python3 ecc_stripper.py raw.bin clean.bin --page-size 2176 --spare-size 128 --pages-per-block 64
"""
import argparse
import json
import os


def load_geometry(meta_path):
    with open(meta_path) as f:
        return json.load(f)["geometry"]


def strip(in_path, out_path, page_size, spare_size, pages_per_block,
          bad_mark=0x00, good=0xFF):
    main = page_size - spare_size
    total_pages = os.path.getsize(in_path) // page_size
    bad_blocks = []
    with open(in_path, "rb") as raw, open(out_path, "wb") as clean:
        for idx in range(total_pages):
            page = raw.read(page_size)
            block = idx // pages_per_block
            if idx % pages_per_block == 0 and page[main] != good:
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
    if a.meta:
        g = load_geometry(a.meta)
        ps, ss, ppb = g["page_size"], g["spare_size"], g["pages_per_block"]
    else:
        if None in (a.page_size, a.spare_size, a.pages_per_block):
            ap.error("provide --meta OR all of --page-size/--spare-size/--pages-per-block")
        ps, ss, ppb = a.page_size, a.spare_size, a.pages_per_block
    bad = strip(a.input, a.output, ps, ss, ppb)
    print(f"[*] {len(bad)} bad block(s): {bad}")
    print(f"[*] wrote {a.output}")


if __name__ == "__main__":
    main()
