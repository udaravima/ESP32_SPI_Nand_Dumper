"""Verify and repair NAND dumps using the proto-v2 per-page CRC verdicts.

Every proto-v2 dump carries a `<dump>.badpages.json` sidecar listing the pages
whose CRC seal failed on arrival, and a `<dump>.meta.json` with the geometry.
This tool uses those verdicts — which is strictly better than blind byte-majority,
because a page that PASSED its CRC in one dump is known-good and can be taken
verbatim.

    # health of a single dump
    python3 verify_dump.py dump.bin

    # repair across several dumps of the same chip
    python3 verify_dump.py a.bin b.bin c.bin -o repaired.bin

For each page the repairer prefers a copy that passed its CRC; only where EVERY
dump flagged the page bad does it fall back to byte-wise majority voting.
"""
import argparse
import json
import os
import sys
from collections import Counter


def read_badpages_set(dump_path):
    """Set of page indices that failed their CRC seal (empty if no sidecar)."""
    try:
        with open(dump_path + ".badpages.json") as f:
            return set(json.load(f).get("bad_pages", []))
    except (OSError, ValueError):
        return set()


def read_meta(dump_path):
    """The dump's .meta.json as a dict (empty if absent/corrupt)."""
    try:
        with open(dump_path + ".meta.json") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def choose_page_sources(total_pages, badpages_sets):
    """Per page, the index of the first dump whose CRC passed (page not in its
    bad set), or None when every dump flagged that page bad."""
    return [next((i for i, bad in enumerate(badpages_sets) if p not in bad), None)
            for p in range(total_pages)]


def majority_bytes(variants):
    """Byte-wise majority across equal-length pages; ties resolve to the first
    variant, so a 2-way split is deterministic rather than arbitrary."""
    n = len(variants[0])
    out = bytearray(n)
    for i in range(n):
        col = [v[i] for v in variants]
        counts = Counter(col)
        top = max(counts.values())
        out[i] = next(b for b in col if counts[b] == top)   # first variant wins ties
    return bytes(out)


def repair(dump_paths, out_path, page_size, total_pages):
    """Write a repaired image; return (taken_good, majority_used)."""
    badsets = [read_badpages_set(d) for d in dump_paths]
    sources = choose_page_sources(total_pages, badsets)
    taken_good = majority_used = 0
    handles = [open(d, "rb") for d in dump_paths]
    try:
        with open(out_path, "wb") as out:
            for p in range(total_pages):
                variants = [fh.read(page_size) for fh in handles]
                src = sources[p]
                if src is not None:
                    out.write(variants[src])
                    taken_good += 1
                else:
                    out.write(majority_bytes(variants))
                    majority_used += 1
    finally:
        for fh in handles:
            fh.close()
    return taken_good, majority_used


def report_health(dump_path):
    """Print one dump's integrity summary from its sidecars."""
    meta = read_meta(dump_path)
    bad = read_badpages_set(dump_path)
    size = os.path.getsize(dump_path) if os.path.exists(dump_path) else 0
    print(f"  {os.path.basename(dump_path)}: {size:,} bytes")
    if meta:
        print(f"    proto v{meta.get('proto_version')} | page_crc={meta.get('page_crc')} "
              f"| truncated={meta.get('truncated')} | bad_pages={meta.get('bad_page_count', len(bad))}")
    else:
        print("    (no .meta.json sidecar)")
    if bad:
        preview = sorted(bad)[:10]
        print(f"    failed pages: {preview}{' ...' if len(bad) > 10 else ''}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Verify/repair NAND dumps via per-page CRC verdicts.")
    ap.add_argument("dumps", nargs="+", help="one or more <dump>.bin files")
    ap.add_argument("-o", "--out", help="write a repaired image (needs 2+ dumps)")
    ap.add_argument("--page-size", type=int, help="override page size (else from .meta.json)")
    args = ap.parse_args(argv)

    print("[*] Dump health:")
    for d in args.dumps:
        report_health(d)

    if not args.out:
        return
    if len(args.dumps) < 2:
        print("[!] Need at least 2 dumps to repair."); sys.exit(1)

    sizes = [os.path.getsize(d) for d in args.dumps]
    if len(set(sizes)) != 1:
        print(f"[!] Dumps differ in size {sizes}; align/truncate them first."); sys.exit(1)

    meta = read_meta(args.dumps[0])
    page_size = args.page_size or meta.get("geometry", {}).get("page_size")
    if not page_size:
        print("[!] Unknown page size — pass --page-size."); sys.exit(1)
    if sizes[0] % page_size:
        print(f"[!] File size {sizes[0]} not a multiple of page size {page_size}."); sys.exit(1)
    total_pages = sizes[0] // page_size

    good, maj = repair(args.dumps, args.out, page_size, total_pages)
    print(f"\n[*] Repaired -> {args.out}")
    print(f"[*] {good}/{total_pages} pages taken from a CRC-good copy; "
          f"{maj} needed majority voting (all copies flagged bad).")


if __name__ == "__main__":
    main()
