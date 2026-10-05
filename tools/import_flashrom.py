"""Seed db/chips/spi-nor/ from flashrom's SPI NOR chip table.

flashrom (https://flashrom.org, GPL-2.0-or-later) keeps the largest public
table of SPI NOR parts. This tool reads a local flashrom checkout and writes
the *facts* we need for reading a chip into our own YAML: JEDEC ID, size,
supply voltage, how a >16 MiB part is addressed, and whether flashrom users
have reported a successful read. No flashrom code is copied; every generated
file names the flashrom commit and source file each entry came from.

    git clone --depth 1 https://github.com/flashrom/flashrom.git /tmp/flashrom
    python3 tools/import_flashrom.py /tmp/flashrom            # rewrite db/chips/spi-nor/
    python3 tools/import_flashrom.py /tmp/flashrom --dry-run  # report only

What is kept, and why:
- Only chips flashrom probes with RDID (9Fh, 3 ID bytes) and reads with the
  plain SPI read. REMS/RES-only parts, AT45DB DataFlash and parts behind a
  4-byte ID need other wire sequences and are reported as skipped.
- Entries that share one JEDEC ID and size are merged into one chip (the
  others become `aliases`). For reading they are the same chip, and one entry
  per ID keeps the device's ID lookup unambiguous. Where the merged entries
  disagree, the merge keeps the conservative choice (lowest voltage, the
  addressing mode every member supports).
- A chip defined by hand anywhere else under db/chips/ wins: its ID is not
  imported, so curated corrections survive a re-import.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chipdb  # noqa: E402

OUT_DIR = os.path.join(chipdb.DB_DIR, "chips", "spi-nor")
GEN_PREFIX = "flashrom-"
SOURCE_URL = "https://github.com/flashrom/flashrom/blob/{rev}/flashchips/{file}"

# JEDEC manufacturer -> NOR vendor profile (db/profiles/nor-*.yml). The profile
# only records where the vendor keeps its quad-enable bit; anything else uses
# nor-generic.
VENDOR_PROFILES = {0xEF: "nor-winbond", 0xC2: "nor-macronix", 0xC8: "nor-gigadevice"}

# feature_bits we act on (include/flash.h)
F_4BA_ENTER = "FEATURE_4BA_ENTER"
F_4BA_ENTER_WREN = "FEATURE_4BA_ENTER_WREN"
F_4BA_READ = "FEATURE_4BA_READ"
F_ADDR_2BYTE = "FEATURE_ADDR_2BYTE"
# Composite macros, expanded so a flag test sees the bits they stand for.
COMPOSITE = {
    "FEATURE_4BA": {F_4BA_ENTER, "FEATURE_4BA_EAR_C5C8", F_4BA_READ,
                    "FEATURE_4BA_FAST_READ", "FEATURE_4BA_WRITE"},
    "FEATURE_4BA_WREN": {F_4BA_ENTER_WREN, "FEATURE_4BA_EAR_C5C8", F_4BA_READ,
                         "FEATURE_4BA_FAST_READ", "FEATURE_4BA_WRITE"},
    "FEATURE_4BA_EAR7": {"FEATURE_4BA_ENTER_EAR7", "FEATURE_4BA_EAR_C5C8", F_4BA_READ,
                         "FEATURE_4BA_FAST_READ", "FEATURE_4BA_WRITE"},
    "FEATURE_4BA_NATIVE": {F_4BA_READ, "FEATURE_4BA_FAST_READ", "FEATURE_4BA_WRITE"},
    "FEATURE_4BA_EAR_ANY": {"FEATURE_4BA_EAR_C5C8", "FEATURE_4BA_EAR_1716"},
}
# TEST_* macros whose read result is OK.
READ_OK_MACROS = {"TEST_OK_PR", "TEST_OK_PRE", "TEST_OK_PREW", "TEST_OK_PREWB"}
ADDR4_RANK = {"native": 3, "enter": 2, "enter_wren": 1}   # merge keeps the lowest


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def parse_defines(header_text):
    """`#define NAME 0x1234` constants from include/flashchips.h."""
    out = {}
    for name, val in re.findall(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", header_text):
        out[name] = int(val, 0)
    return out


def split_entries(text):
    """Top-level `{ ... }` blocks of one flashchips/*.c file (comments removed)."""
    text = strip_comments(text)
    entries, depth, start = [], 0, None
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                entries.append(text[start:i + 1])
                start = None
    return entries


def _field(entry, name, pattern=r"([^,\n]+)"):
    m = re.search(r"\.%s\s*=\s*%s" % (name, pattern), entry)
    return m.group(1).strip() if m else None


def _product(expr):
    """`2048` or `2 * 1024` -> int (flashrom writes some sizes as products)."""
    out = 1
    for n in re.findall(r"\d+", expr or "0"):
        out *= int(n)
    return out if expr else 0


def parse_entry(entry, defines):
    """One flashrom chip entry -> dict of the raw facts we use (or None)."""
    name = _field(entry, "name", r'"([^"]*)"')
    if name is None:
        return None
    feat = _field(entry, "feature_bits", r"([^,]+(?:\|[^,]+)*)") or ""
    bits = set()
    for tok in re.findall(r"\w+", feat):
        bits |= COMPOSITE.get(tok, {tok})
    tested = _field(entry, "tested", r"(TEST_\w+|\{[^}]*\})") or ""
    # The tested struct literal has its own .probe/.read members; drop it so
    # they can't be mistaken for the chip's probe and read functions.
    entry = re.sub(r"\.tested\s*=\s*\{[^}]*\}", "", entry)
    read_ok = tested in READ_OK_MACROS or bool(re.search(r"\.read\s*=\s*OK", tested))
    volt = re.search(r"\.voltage\s*=\s*\{\s*(\d+)\s*,\s*(\d+)\s*\}", entry)

    def ident(key):
        v = _field(entry, key, r"(\w+)")
        if v is None:
            return None
        return int(v, 0) if re.match(r"^(0x[0-9A-Fa-f]+|\d+)$", v) else defines.get(v)

    return {
        "vendor": _field(entry, "vendor", r'"([^"]*)"') or "",
        "name": name,
        "bustype": _field(entry, "bustype") or "",
        "probe": _field(entry, "probe", r"(\w+)") or "",
        "read": _field(entry, "read", r"(\w+)") or "",
        "mfr": ident("manufacture_id"),
        "model": ident("model_id"),
        "size_kib": _product(_field(entry, "total_size", r"([\d\s*]+)")),
        "bits": bits,
        "read_ok": read_ok,
        "voltage": (int(volt.group(1)), int(volt.group(2))) if volt else None,
    }


def skip_reason(c):
    """Why a parsed entry can't be read by this firmware (None = importable)."""
    if "BUS_SPI" not in c["bustype"]:
        return "not SPI"
    if c["probe"] != "PROBE_SPI_RDID":
        return f"probe {c['probe']}"
    if c["read"] != "SPI_CHIP_READ":
        return f"read {c['read']}"
    if c["mfr"] is None or c["model"] is None:
        return "unresolved ID macro"
    if c["mfr"] > 0xFF or c["model"] > 0xFFFF or c["model"] in (0xFFFF, 0xFFFE):
        return "not a plain 3-byte JEDEC ID"
    if F_ADDR_2BYTE in c["bits"]:
        return "2-byte addressing"
    size = c["size_kib"]
    if size < 4 or size & (size - 1):
        return f"size {size} KiB"
    if size > 16384 and addr4_mode(c) is None:
        return "over 16 MiB with no 4-byte read or B7h entry (EAR only)"
    return None


def addr4_mode(c):
    """How a part above 16 MiB reaches its upper half; 'none' for <= 16 MiB."""
    if c["size_kib"] <= 16384:
        return "none"
    if F_4BA_READ in c["bits"]:
        return "native"       # 13h/6Ch take a 4-byte address, no mode change
    if F_4BA_ENTER in c["bits"]:
        return "enter"        # B7h switches to 4-byte addressing (volatile)
    if F_4BA_ENTER_WREN in c["bits"]:
        return "enter_wren"   # WREN then B7h
    return None


def vcc_class(voltage):
    """flashrom's supply range -> the vcc_mv the DB records. 3300 means the
    ESP32's 3.3 V I/O is in range; anything lower needs a level shifter."""
    if voltage is None:
        return 3300
    lo, hi = voltage
    if hi >= 3300:
        return 3300
    return 1800 if hi <= 2000 else 2500


def short_name(name):
    first = name.split("/")[0].strip()
    first = re.sub(r"[^A-Za-z0-9._()+-]", "", first)
    return first[:chipdb.NAME_MAX]


def merge_group(members):
    """Entries sharing (mfr, model, size) -> one chip record."""
    names = []
    for m in members:
        for part in m["name"].split("/"):
            part = part.strip()
            if part and part not in names:
                names.append(part)
    modes = [addr4_mode(m) for m in members]
    head = members[0]
    return {
        "name": short_name(head["name"]),
        "aliases": [n for n in names if n != short_name(head["name"])],
        "vendor": head["vendor"],
        "mfr": head["mfr"], "model": head["model"], "size_kib": head["size_kib"],
        "addr4": min(modes, key=lambda m: ADDR4_RANK.get(m, 0)),
        "vcc_mv": min(vcc_class(m["voltage"]) for m in members),
        "read_ok": any(m["read_ok"] for m in members),
        "files": sorted({m["file"] for m in members}),
    }


def curated_ids(db_root):
    """JEDEC IDs of spi-nor chips defined by hand (outside generated files)."""
    import yaml
    out = set()
    for f in glob.glob(os.path.join(db_root, "chips", "**", "*.yml"), recursive=True):
        if os.path.basename(f).startswith(GEN_PREFIX):
            continue
        with open(f) as fh:
            for body in (yaml.safe_load(fh) or {}).values():
                if body.get("family") == "spi-nor":
                    i = body["id"]
                    out.add((i["mfr"], (i["dev"] << 8) | (i.get("dev2") or 0)))
    return out


def collect(flashrom_dir, db_root=chipdb.DB_DIR):
    """Parse flashrom -> (chips by output file, skipped [(name, reason)])."""
    with open(os.path.join(flashrom_dir, "include", "flashchips.h")) as fh:
        defines = parse_defines(fh.read())
    parsed, skipped = [], []
    for path in sorted(glob.glob(os.path.join(flashrom_dir, "flashchips", "*.c"))):
        with open(path) as fh:
            text = fh.read()
        for e in split_entries(text):
            c = parse_entry(e, defines)
            if c is None:
                continue
            c["file"] = os.path.basename(path)
            why = skip_reason(c)
            if why:
                if "BUS_SPI" in c["bustype"]:
                    skipped.append((c["name"], why))
                continue
            parsed.append(c)

    groups = {}
    for c in parsed:
        groups.setdefault((c["mfr"], c["model"], c["size_kib"]), []).append(c)
    hand = curated_ids(db_root)
    chips, seen = [], set()
    for key, members in groups.items():
        if (key[0], key[1]) in hand:
            skipped.append((members[0]["name"], "defined by hand in db/chips/"))
            continue
        chip = merge_group(members)
        base, n = chip["name"], 2
        while chip["name"] in seen:          # same part name, different ID/size
            suffix = f"-{n}"
            chip["name"] = base[:chipdb.NAME_MAX - len(suffix)] + suffix
            n += 1
        seen.add(chip["name"])
        chips.append(chip)

    by_file = {}
    for chip in chips:
        by_file.setdefault(chip["files"][0], []).append(chip)
    return by_file, skipped


def git_rev(flashrom_dir):
    try:
        return subprocess.check_output(["git", "-C", flashrom_dir, "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _yaml_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_file(src_file, chips, rev):
    lines = [
        f"# AUTO-GENERATED by tools/import_flashrom.py from flashrom flashchips/{src_file}",
        f"# (flashrom commit {rev}, GPL-2.0-or-later). Chip facts only (JEDEC ID, size,",
        "# voltage, 4-byte addressing, flashrom read test status), each with a link to",
        "# the entry's source. Do not edit: re-run the importer. To correct a chip, define",
        "# it in its own db/chips/ file; a hand-written entry wins over an imported one.",
    ]
    for c in chips:
        dev, dev2 = c["model"] >> 8, c["model"] & 0xFF
        src = " ".join(SOURCE_URL.format(rev=rev, file=f) for f in c["files"])
        lines += [
            f"{_yaml_str(c['name'])}:",
            f"  id: {{mfr: 0x{c['mfr']:02X}, dev: 0x{dev:02X}, dev2: 0x{dev2:02X}}}",
            "  family: spi-nor",
            f"  profile: {VENDOR_PROFILES.get(c['mfr'], 'nor-generic')}",
            f"  geometry: {{size_kib: {c['size_kib']}}}",
            f"  addr4: {c['addr4']}",
            f"  vcc_mv: {c['vcc_mv']}",
            # Resident (compiled into the firmware): flashrom users have read
            # it, and the ESP32's 3.3 V I/O is in range.
            f"  resident: {'true' if c['read_ok'] and c['vcc_mv'] == 3300 else 'false'}",
            f"  source: {_yaml_str(src)}",
        ]
        if c["aliases"]:
            lines.append("  aliases: [" + ", ".join(_yaml_str(a) for a in c["aliases"]) + "]")
        lines.append(f"  notes: {_yaml_str(c['vendor'] + '; flashrom read test: ' + ('OK' if c['read_ok'] else 'not reported'))}")
    return "\n".join(lines) + "\n"


def out_name(src_file):
    return GEN_PREFIX + os.path.splitext(src_file)[0].replace("_", "-") + ".yml"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Import SPI NOR chip facts from flashrom.")
    ap.add_argument("flashrom", help="path to a flashrom checkout")
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verbose", "-v", action="store_true", help="list skipped entries")
    a = ap.parse_args(argv)

    by_file, skipped = collect(a.flashrom)
    rev = git_rev(a.flashrom)
    total = sum(len(v) for v in by_file.values())
    resident = sum(1 for v in by_file.values() for c in v
                   if c["read_ok"] and c["vcc_mv"] == 3300)
    print(f"[+] {total} SPI NOR chips from {len(by_file)} flashrom files "
          f"({resident} resident), {len(skipped)} entries skipped")
    if a.verbose:
        for name, why in skipped:
            print(f"    skip {name}: {why}")
    if a.dry_run:
        return 0
    os.makedirs(a.out, exist_ok=True)
    for old in glob.glob(os.path.join(a.out, GEN_PREFIX + "*.yml")):
        os.remove(old)
    for src_file, chips in sorted(by_file.items()):
        with open(os.path.join(a.out, out_name(src_file)), "w") as fh:
            fh.write(render_file(src_file, chips, rev))
    print(f"[+] wrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
