"""Check a dump for the missing plane-select bug on 2-plane chips (MT29F2G01).

Before the plane-select fix the firmware read every page from cache with
column 0, i.e. plane 0. On a 2-plane die an odd block's PAGE READ loads plane
1's cache, so the cache read returned plane 0's cache instead — which still
holds the LAST page of the preceding even block. The signature in an affected
dump is therefore exact: every page of odd block 2k+1 is byte-identical to the
last page of block 2k.

    python3 tools/check_planes.py target/dump.bin            # geometry from .meta.json
    python3 tools/check_planes.py dump.bin --page-size 2176 --pages-per-block 64

Exit status: 0 = no odd block shows the signature, 1 = affected, 2 = usage error.
Blocks where the reference page is erased (all 0xFF) cannot be told apart from
a genuinely erased odd block and are reported as inconclusive.
"""
import argparse
import json
import mmap
import sys


def classify_odd_blocks(data, page_size, pages_per_block):
    """Return (affected, clean, inconclusive) lists of odd block indices."""
    block_bytes = page_size * pages_per_block
    total_blocks = len(data) // block_bytes
    erased = b"\xFF" * page_size
    affected, clean, inconclusive = [], [], []
    for blk in range(1, total_blocks, 2):
        ref_off = blk * block_bytes - page_size       # last page of block blk-1
        ref = data[ref_off:ref_off + page_size]
        base = blk * block_bytes
        mirrors = all(data[base + p * page_size: base + (p + 1) * page_size] == ref
                      for p in range(pages_per_block))
        if not mirrors:
            clean.append(blk)
        elif ref == erased:
            inconclusive.append(blk)
        else:
            affected.append(blk)
    return affected, clean, inconclusive


def geometry_from_meta(dump_path):
    try:
        with open(dump_path + ".meta.json") as f:
            g = json.load(f).get("geometry", {})
        return g.get("page_size"), g.get("pages_per_block")
    except (OSError, ValueError):
        return None, None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--page-size", type=int, help="total bytes/page incl. spare")
    ap.add_argument("--pages-per-block", type=int)
    a = ap.parse_args(argv)

    ps, ppb = geometry_from_meta(a.dump)
    ps, ppb = a.page_size or ps, a.pages_per_block or ppb
    if not ps or not ppb:
        print("[!] No geometry: pass --page-size/--pages-per-block or keep the .meta.json")
        return 2

    with open(a.dump, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        affected, clean, inconclusive = classify_odd_blocks(data, ps, ppb)

    total = len(affected) + len(clean) + len(inconclusive)
    print(f"[*] Odd blocks checked: {total}")
    print(f"    mirror previous block's last page (affected): {len(affected)}")
    print(f"    independent data (clean):                     {len(clean)}")
    print(f"    erased, can't tell (inconclusive):            {len(inconclusive)}")
    if affected:
        print(f"[!] AFFECTED — first odd blocks: {affected[:10]}. "
              "Re-dump with firmware that sets plane select.")
        return 1
    print("[+] No plane-select signature found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
