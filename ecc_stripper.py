"""Strip spare/OOB from a raw NAND dump into a main-area image.

Geometry comes from the dump's .meta.json sidecar (written by dump.py),
or from --page-size/--spare-size/--pages-per-block overrides.

Bad blocks are replaced with 0xFF padding so filesystem offsets stay aligned.
Where the factory bad-block marker sits is vendor-specific, so it comes from
the chip's profile in db/ (offset, width, good value, and which pages of the
block carry it). The profile is the one named by --profile, else the one the
sidecar records, else the classic default: one byte at spare[0] of each
block's first page, good = 0xFF. Like the firmware, any marker byte that is not
the good value marks the block bad.

Usage:
    python3 ecc_stripper.py raw.bin clean.bin --meta raw.bin.meta.json
    python3 ecc_stripper.py raw.bin clean.bin --profile DS35Q1GA
    python3 ecc_stripper.py raw.bin clean.bin --page-size 2176 --spare-size 128 --pages-per-block 64
"""
import argparse
import json
import os

# Classic marker: spare[0] of the first page, 0xFF = good.
DEFAULT_BBM = {"offset": 0, "length": 1, "good": 0xFF, "pages": ("first",)}


def load_meta(meta_path):
    with open(meta_path) as f:
        return json.load(f)


def load_geometry(meta_path):
    return load_meta(meta_path)["geometry"]


def bbm_from_flat(flat):
    """The marker fields of a flattened chipdb profile, as a bbm dict."""
    pages = tuple(p for bit, p in ((0x1, "first"), (0x2, "second"), (0x4, "last"))
                  if flat["bbm_pages"] & bit)
    return {"offset": flat["bbm_off"], "length": flat["bbm_len"],
            "good": flat["bbm_good"], "pages": pages}


def load_profile(name, db_root=None):
    """Flatten chip `name` from db/. Raises chipdb.ChipDBError if unknown."""
    from tools import chipdb
    db = chipdb.load_db(db_root) if db_root else chipdb.load_db()
    flat = chipdb.flatten(db, name)
    chipdb.check_flat(flat)
    return flat


def marker_pages(bbm, pages_per_block):
    idx = {"first": 0, "second": 1, "last": pages_per_block - 1}
    return sorted({idx[p] for p in bbm["pages"] if idx[p] < pages_per_block})


def strip(in_path, out_path, page_size, spare_size, pages_per_block, bbm=None):
    """Write the main area of every page; pad bad blocks with 0xFF.
    Returns the list of bad block indexes."""
    bbm = bbm or DEFAULT_BBM
    main = page_size - spare_size
    if bbm["offset"] + bbm["length"] > spare_size:
        raise ValueError("bad-block marker lies outside the spare area")
    lo, hi = main + bbm["offset"], main + bbm["offset"] + bbm["length"]
    good = bytes([bbm["good"]]) * bbm["length"]
    check = marker_pages(bbm, pages_per_block)
    block_bytes = page_size * pages_per_block
    total_pages = os.path.getsize(in_path) // page_size
    bad_blocks = []
    with open(in_path, "rb") as raw, open(out_path, "wb") as clean:
        block = 0
        while block * pages_per_block < total_pages:
            n = min(pages_per_block, total_pages - block * pages_per_block)
            data = raw.read(n * page_size) if n < pages_per_block else raw.read(block_bytes)
            pages = [data[i * page_size:(i + 1) * page_size] for i in range(n)]
            # A marker on any checked page (e.g. "last") condemns the whole
            # block, so the block is read whole before any of it is written.
            if any(p < n and pages[p][lo:hi] != good for p in check):
                bad_blocks.append(block)
                clean.write(b"\xFF" * main * n)
            else:
                for pg in pages:
                    clean.write(pg[:main])
            block += 1
    return bad_blocks


def resolve_bbm(profile_name, meta, warn=print):
    """Pick the marker definition: --profile, then the sidecar's profile, then
    the default. Returns (bbm, flat or None, where it came from)."""
    name = profile_name or ((meta or {}).get("profile") or {}).get("name")
    if not name:
        return DEFAULT_BBM, None, "default"
    try:
        flat = load_profile(name)
    except ImportError:
        if profile_name:
            raise SystemExit("[!] --profile needs PyYAML: pip install pyyaml")
        warn("[~] PyYAML missing: using the default bad-block marker")
        return DEFAULT_BBM, None, "default"
    except ValueError as e:      # chipdb.ChipDBError
        if profile_name:
            raise SystemExit(f"[!] {e}")
        warn(f"[~] {name} is not in db/ ({e}); using the default bad-block marker")
        return DEFAULT_BBM, None, "default"
    return bbm_from_flat(flat), flat, f"profile {name}"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Strip spare/OOB from a raw NAND dump")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--meta", help="path to <dump>.meta.json")
    ap.add_argument("--profile", help="chip name in db/: its bad-block marker "
                    "(and geometry, when there is no sidecar) is used")
    ap.add_argument("--page-size", type=int)
    ap.add_argument("--spare-size", type=int)
    ap.add_argument("--pages-per-block", type=int)
    a = ap.parse_args(argv)
    meta = load_meta(a.meta) if a.meta else None
    bbm, flat, source = resolve_bbm(a.profile, meta)
    if meta:
        g = meta["geometry"]
        ps, ss, ppb = g["page_size"], g["spare_size"], g["pages_per_block"]
    elif None not in (a.page_size, a.spare_size, a.pages_per_block):
        ps, ss, ppb = a.page_size, a.spare_size, a.pages_per_block
    elif flat:
        ps, ss, ppb = flat["page_size"], flat["spare_size"], flat["pages_per_block"]
    else:
        ap.error("provide --meta, --profile, or all of "
                 "--page-size/--spare-size/--pages-per-block")
    if meta and meta.get("ecc_on"):
        print("[~] This dump was read with on-die ECC on. Factory markers are only "
              "reliable in raw (ECC off) dumps, so treat the bad-block list as a hint.")
    print(f"[*] Bad-block marker ({source}): {bbm['length']} byte(s) at spare+"
          f"{bbm['offset']}, good 0x{bbm['good']:02X}, pages {'/'.join(bbm['pages'])}")
    bad = strip(a.input, a.output, ps, ss, ppb, bbm)
    print(f"[*] {len(bad)} bad block(s): {bad}")
    print(f"[*] wrote {a.output}")


if __name__ == "__main__":
    main()
