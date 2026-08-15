# NAND Dumper Generalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the ESP32 SPI NAND dumper chip-agnostic (JEDEC-ID auto-detect from a community-editable registry), correct on 2 Gbit parts, and buildable for both ESP32-classic and ESP32-S3 — while fixing the show-stopper bugs that currently corrupt a 2 Gbit dump.

**Architecture:** A build-time Python hook turns `chips.yml` into a compiled C table. Firmware reads the chip's JEDEC ID, looks up geometry + capabilities, and drives a runtime-parameterized read path. Pure logic (lookup, row-address, wire header) is isolated into ESP-IDF-free modules so it can be unit-tested on the host; hardware-coupled code is compile-checked and bench-verified. A 32-byte geometry header sent before the dump lets the PC tools self-configure, eliminating the page-size mismatch class of bug.

**Tech Stack:** C++ (Arduino/ESP-IDF, PlatformIO), Python 3 (PyYAML, pytest), PlatformIO `native` (Unity) test env.

**Spec:** [docs/superpowers/specs/2026-08-15-nand-dumper-generalization-design.md](../specs/2026-08-15-nand-dumper-generalization-design.md)

## Global Constraints

- **Read-only.** No PROGRAM/ERASE/PROTECT commands anywhere.
- **ECC default is OFF/raw.** `ecc_default_on = false` for both shipped chips.
- **Row address is `uint32_t` end-to-end.** Never `uint16_t` — that is the headline bug.
- **MT29F2G01ABAGD reference geometry (verbatim):** `mfr_id=0x2C`, `dev_id=0x24`, `page_size=2176`, `spare_size=128`, `pages_per_block=64`, `total_blocks=2048`, `page_addr_bits=6`, `bad_block_mark=0x00`, `has_qe_bit=false`, `vcc_mv=3300`.
- **SPI host per target:** classic → `SPI3_HOST`; S3 → `SPI2_HOST`. Never `SPI0/SPI1`.
- **Forbidden pins:** classic GPIO6–11 (internal flash), GPIO16/17 (PSRAM modules); S3 GPIO26–37 (flash/PSRAM), plus strapping (0/3/45/46), USB (19/20), UART0 (43/44).
- **Wire header:** 32 bytes, little-endian, magic `"NANDMP"`, proto version `1`, trailing CRC32 that matches Python `zlib.crc32`.
- **Commit after every green step.** Do not batch.

---

## File Structure

**New — pure/testable (no ESP-IDF deps):**
- `src/nand_chips.h` — `nand_chip_t` struct + `nand_chip_lookup()` declaration.
- `src/nand_chips.cpp` — lookup implementation; includes the generated table.
- `src/nand_chips_generated.h` — **generated + committed** `CHIPS[]` array.
- `src/nand_addr.h` — `nand_row_addr()` pure inline.
- `src/dump_header.h` / `src/dump_header.cpp` — geometry struct + pack/CRC32.

**New — registry & tooling:**
- `chips.yml` — the chip registry (contributor surface).
- `tools/gen_chips.py` — validates `chips.yml`, emits the generated header.
- `tools/tests/test_gen_chips.py` — generator tests.
- `tests/test_dump_header.py` — Python golden cross-check of the wire header.
- `tests/test_dump_client.py` — `dump.py` header parse / metadata tests.
- `tests/test_ecc_stripper.py` — stripper geometry tests.
- `tests/test_binary_compare_fix.py` — majority-vote tests.
- `requirements-dev.txt` — `pyyaml`, `pytest`.

**New — firmware config & native tests:**
- `src/board_pins.h` — per-target pins + `NAND_SPI_HOST`.
- `test/test_pure/test_pure.cpp` — Unity tests for lookup/addr/header.

**Modified:**
- `platformio.ini` — dual firmware envs + `native` env + generator hook.
- `src/nand_driver.h` / `src/nand_driver.cpp` — runtime geometry, `uint32_t` row, ECC toggle, ECC-status mask, verify fix, quad gating + self-test.
- `src/main.cpp` — boot/detect/menu restructure; geometry header emit.
- `src/wifi_transport.h` / `src/wifi_transport.cpp` — send header helper.
- `dump.py`, `ecc_stripper.py` — self-configure from header/sidecar.
- `README.md` (+ new `CONTRIBUTING.md`).

**Removed:** `src/config.yml` (superseded by `chips.yml`), `src/quad.cpp.bk` (obsolete).

---

## Task 1: Chip registry + code generator (Python)

**Files:**
- Create: `chips.yml`, `tools/gen_chips.py`, `tools/tests/test_gen_chips.py`, `requirements-dev.txt`
- Generate: `src/nand_chips_generated.h`

**Interfaces:**
- Produces: `render_header(chips: dict) -> str`, `validate(chips: dict) -> None` (raises `ValueError`), `load_chips(path) -> dict`, `generate(yml_path, out_path)`. The generated `CHIPS[]` field order is exactly: `name, mfr_id, dev_id, page_size, spare_size, pages_per_block, total_blocks, page_addr_bits, bad_block_mark, has_qe_bit, qe_feature_addr, qe_bit, ecc_default_on, vcc_mv`.

- [ ] **Step 1: Create `requirements-dev.txt`**

```
pyyaml
pytest
```

Install: `pip install -r requirements-dev.txt`

- [ ] **Step 2: Write the failing generator tests**

Create `tools/tests/test_gen_chips.py`:

```python
import math
import pytest
from tools.gen_chips import validate, render_header

BASE = {
    "mfr_id": 0x2C, "dev_id": 0x24, "page_size": 2176, "spare_size": 128,
    "pages_per_block": 64, "total_blocks": 2048, "bad_block_mark": 0x00,
    "has_qe_bit": False, "ecc_default": "off", "vcc_mv": 3300,
}

def test_validate_accepts_good_chip():
    validate({"MT29F2G01ABAGD": dict(BASE)})  # no raise

def test_validate_rejects_spare_ge_page():
    bad = dict(BASE); bad["spare_size"] = 2176
    with pytest.raises(ValueError, match="spare_size"):
        validate({"X": bad})

def test_validate_rejects_non_power_of_two_ppb():
    bad = dict(BASE); bad["pages_per_block"] = 60
    with pytest.raises(ValueError, match="pages_per_block"):
        validate({"X": bad})

def test_validate_rejects_duplicate_ids():
    with pytest.raises(ValueError, match="duplicate"):
        validate({"A": dict(BASE), "B": dict(BASE)})

def test_validate_requires_qe_fields_when_has_qe_bit():
    q = dict(BASE); q["has_qe_bit"] = True
    with pytest.raises(ValueError, match="qe_feature_addr"):
        validate({"X": q})

def test_render_emits_row_with_derived_page_addr_bits():
    out = render_header({"MT29F2G01ABAGD": dict(BASE)})
    # page_addr_bits = log2(64) = 6, ecc off -> false
    assert '{ "MT29F2G01ABAGD", 0x2C, 0x24, 2176, 128, 64, 2048, 6,' in out
    assert "false" in out and "3300" in out
    assert "CHIPS_COUNT" in out
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `python -m pytest tools/tests/test_gen_chips.py -v`
Expected: FAIL — `ModuleNotFoundError: tools.gen_chips`. (Create empty `tools/__init__.py` and `tools/tests/__init__.py` if needed so the import path resolves; run pytest from repo root.)

- [ ] **Step 4: Implement `tools/gen_chips.py`**

```python
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
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `python -m pytest tools/tests/test_gen_chips.py -v`
Expected: PASS (6 tests).

- [ ] **Step 6: Create `chips.yml`**

```yaml
# NAND chip registry. Add your chip here, run `pio run`, open a PR.
# See CONTRIBUTING.md for how to read the JEDEC id off the serial log.
chips:
  MT29F2G01ABAGD:
    mfr_id:  0x2C
    dev_id:  0x24
    page_size:       2176    # total bytes/page (main + spare)
    spare_size:      128
    pages_per_block: 64
    total_blocks:    2048
    bad_block_mark:  0x00
    has_qe_bit:      false    # Micron: quad needs no enable bit
    ecc_default:     off      # global policy is off/raw
    vcc_mv:          3300
    notes: "2Gb SLC, 8-bit/512 on-die ECC, 2 planes x 1024 blocks"

  DS35Q1GA:
    mfr_id:  0xE5             # PLACEHOLDER — confirm from 9Fh / DS35 datasheet
    dev_id:  0x71             # PLACEHOLDER — confirm from silicon
    page_size:       2112
    spare_size:      64
    pages_per_block: 64
    total_blocks:    1024
    bad_block_mark:  0x00
    has_qe_bit:      false
    ecc_default:     off
    vcc_mv:          3300
    notes: "1Gb SLC (FORESEE/DoSilicon)"
```

- [ ] **Step 7: Generate the header and commit**

```bash
python tools/gen_chips.py chips.yml src/nand_chips_generated.h
git add chips.yml tools/gen_chips.py tools/__init__.py tools/tests/ \
        requirements-dev.txt src/nand_chips_generated.h
git commit -m "feat: add chips.yml registry and code generator"
```

---

## Task 2: Firmware chip lookup + native test env

**Files:**
- Create: `src/nand_chips.h`, `src/nand_chips.cpp`, `test/test_pure/test_pure.cpp`
- Modify: `platformio.ini` (add `native` env)

**Interfaces:**
- Consumes: `CHIPS[]`, `CHIPS_COUNT` from `src/nand_chips_generated.h` (Task 1).
- Produces: `nand_chip_t` struct; `const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id)` (returns `NULL` if not found).

- [ ] **Step 1: Create `src/nand_chips.h`**

```c
#ifndef NAND_CHIPS_H
#define NAND_CHIPS_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { NAND_READ_SINGLE = 0, NAND_READ_QUAD = 1 } nand_read_mode_t;

typedef struct {
  const char *name;
  uint8_t  mfr_id, dev_id;
  uint16_t page_size;        // total bytes/page (main + spare)
  uint16_t spare_size;
  uint16_t pages_per_block;
  uint16_t total_blocks;
  uint8_t  page_addr_bits;   // log2(pages_per_block)
  uint8_t  bad_block_mark;
  bool     has_qe_bit;
  uint8_t  qe_feature_addr;
  uint8_t  qe_bit;
  bool     ecc_default_on;
  uint16_t vcc_mv;
} nand_chip_t;

const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id);

#endif // NAND_CHIPS_H
```

- [ ] **Step 2: Write the failing Unity test**

Create `test/test_pure/test_pure.cpp`:

```c
#include <unity.h>
#include "nand_chips.h"

void test_lookup_finds_micron(void) {
  const nand_chip_t *c = nand_chip_lookup(0x2C, 0x24);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_UINT16(2176, c->page_size);
  TEST_ASSERT_EQUAL_UINT16(2048, c->total_blocks);
  TEST_ASSERT_EQUAL_UINT8(6, c->page_addr_bits);
  TEST_ASSERT_FALSE(c->ecc_default_on);
}

void test_lookup_returns_null_for_unknown(void) {
  TEST_ASSERT_NULL(nand_chip_lookup(0x00, 0x00));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_finds_micron);
  RUN_TEST(test_lookup_returns_null_for_unknown);
  return UNITY_END();
}
```

- [ ] **Step 3: Add the `native` env to `platformio.ini`**

Append:

```ini
[env:native]
platform = native
test_framework = unity
; -<*> excludes the Arduino/ESP-IDF sources (main/driver/wifi) that can't
; build on host; only the ESP-IDF-free pure modules are compiled.
build_src_filter = -<*> +<nand_chips.cpp> +<dump_header.cpp>
build_flags = -I src
```

- [ ] **Step 4: Run the test to verify it fails**

Run: `pio test -e native -f test_pure`
Expected: FAIL/link error — `nand_chip_lookup` undefined.

- [ ] **Step 5: Implement `src/nand_chips.cpp`**

```c
#include "nand_chips.h"
#include "nand_chips_generated.h"

const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id) {
  for (unsigned i = 0; i < CHIPS_COUNT; i++) {
    if (CHIPS[i].mfr_id == mfr_id && CHIPS[i].dev_id == dev_id) {
      return &CHIPS[i];
    }
  }
  return 0;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f test_pure`
Expected: PASS (2 tests).

- [ ] **Step 7: Commit**

```bash
git add src/nand_chips.h src/nand_chips.cpp platformio.ini test/test_pure/
git commit -m "feat: chip lookup by JEDEC id with native unit test"
```

---

## Task 3: Row-address computation (kills the uint16 overflow)

**Files:**
- Create: `src/nand_addr.h`
- Modify: `test/test_pure/test_pure.cpp`

**Interfaces:**
- Produces: `uint32_t nand_row_addr(uint32_t block, uint32_t page, uint8_t page_addr_bits)`.

- [ ] **Step 1: Add failing tests to `test/test_pure/test_pure.cpp`**

Add these functions and `RUN_TEST` lines (include `#include "nand_addr.h"` at top):

```c
void test_row_addr_block1024_does_not_alias_zero(void) {
  // The headline bug: 1024<<6 = 65536 overflows uint16_t to 0.
  TEST_ASSERT_EQUAL_UINT32(65536u, nand_row_addr(1024, 0, 6));
}

void test_row_addr_last_page_of_2gbit(void) {
  // block 2047, page 63 -> 2047*64 + 63 = 131071 (17 bits)
  TEST_ASSERT_EQUAL_UINT32(131071u, nand_row_addr(2047, 63, 6));
}

void test_row_addr_masks_page_field(void) {
  TEST_ASSERT_EQUAL_UINT32((5u << 6) | 3u, nand_row_addr(5, 3, 6));
}
```

Add to `main()`:
```c
  RUN_TEST(test_row_addr_block1024_does_not_alias_zero);
  RUN_TEST(test_row_addr_last_page_of_2gbit);
  RUN_TEST(test_row_addr_masks_page_field);
```

- [ ] **Step 2: Run to verify failure**

Run: `pio test -e native -f test_pure`
Expected: FAIL — `nand_row_addr` undefined.

- [ ] **Step 3: Implement `src/nand_addr.h`**

```c
#ifndef NAND_ADDR_H
#define NAND_ADDR_H
#include <stdint.h>

// Linear page (row) address: block concatenated with the page field.
// page_addr_bits = log2(pages_per_block). Return is 32-bit — NEVER narrow to 16.
static inline uint32_t nand_row_addr(uint32_t block, uint32_t page,
                                     uint8_t page_addr_bits) {
  uint32_t page_mask = (1u << page_addr_bits) - 1u;
  return (block << page_addr_bits) | (page & page_mask);
}

#endif // NAND_ADDR_H
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_pure`
Expected: PASS (5 tests total).

- [ ] **Step 5: Commit**

```bash
git add src/nand_addr.h test/test_pure/test_pure.cpp
git commit -m "feat: 32-bit row-address helper (fixes 2Gbit aliasing)"
```

---

## Task 4: Geometry wire header (firmware pack + Python cross-check)

**Files:**
- Create: `src/dump_header.h`, `src/dump_header.cpp`, `tests/test_dump_header.py`
- Modify: `test/test_pure/test_pure.cpp`

**Interfaces:**
- Produces (C): `dump_geometry_t` struct; `void dump_header_pack(uint8_t buf[32], const dump_geometry_t *g)`; `uint32_t dump_crc32(const uint8_t *data, unsigned len)`.
- Produces (Python): the same 32-byte layout, parsed with `struct` format `"<6sBBHHHHIBBBBII"` and `zlib.crc32` over bytes 0..27.

- [ ] **Step 1: Add failing Unity test for pack + CRC round-trip**

Add to `test/test_pure/test_pure.cpp` (include `#include "dump_header.h"` and `#include <string.h>`):

```c
void test_header_pack_layout_and_crc(void) {
  dump_geometry_t g = {0};
  g.page_size = 2176; g.spare_size = 128; g.pages_per_block = 64;
  g.total_blocks = 2048; g.total_pages = 2048u * 64u;
  g.total_bytes = 2048u * 64u * 2176u;
  g.mfr_id = 0x2C; g.dev_id = 0x24; g.page_addr_bits = 6;
  g.flags = 0x00; // ecc off, single, no verify

  uint8_t buf[32];
  dump_header_pack(buf, &g);

  TEST_ASSERT_EQUAL_UINT8_ARRAY("NANDMP", buf, 6);
  TEST_ASSERT_EQUAL_UINT8(1, buf[6]);               // proto version
  TEST_ASSERT_EQUAL_UINT16(2176, buf[8] | (buf[9] << 8)); // page_size LE
  // CRC over bytes 0..27 lands in bytes 28..31
  uint32_t crc = dump_crc32(buf, 28);
  uint32_t stored = buf[28] | (buf[29] << 8) | (buf[30] << 16) | ((uint32_t)buf[31] << 24);
  TEST_ASSERT_EQUAL_UINT32(crc, stored);
}
```

Add `RUN_TEST(test_header_pack_layout_and_crc);` to `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `pio test -e native -f test_pure`
Expected: FAIL — `dump_header_pack` / `dump_crc32` undefined.

- [ ] **Step 3: Implement `src/dump_header.h`**

```c
#ifndef DUMP_HEADER_H
#define DUMP_HEADER_H
#include <stdint.h>

#define DUMP_HEADER_SIZE   32
#define DUMP_PROTO_VERSION 1
#define DUMP_FLAG_ECC_ON   0x01
#define DUMP_FLAG_QUAD     0x02
#define DUMP_FLAG_VERIFY   0x04

typedef struct {
  uint16_t page_size, spare_size, pages_per_block, total_blocks;
  uint32_t total_pages, total_bytes;
  uint8_t  mfr_id, dev_id, page_addr_bits, flags;
} dump_geometry_t;

uint32_t dump_crc32(const uint8_t *data, unsigned len);
void dump_header_pack(uint8_t buf[DUMP_HEADER_SIZE], const dump_geometry_t *g);

#endif // DUMP_HEADER_H
```

- [ ] **Step 4: Implement `src/dump_header.cpp`**

```c
#include "dump_header.h"
#include <string.h>

// Bitwise CRC32 (reflected, poly 0xEDB88420) — matches Python zlib.crc32.
uint32_t dump_crc32(const uint8_t *data, unsigned len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88420u & (-(int32_t)(crc & 1)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

void dump_header_pack(uint8_t buf[DUMP_HEADER_SIZE], const dump_geometry_t *g) {
  memset(buf, 0, DUMP_HEADER_SIZE);
  memcpy(buf, "NANDMP", 6);
  buf[6] = DUMP_PROTO_VERSION;
  buf[7] = g->flags;
  put16(buf + 8,  g->page_size);
  put16(buf + 10, g->spare_size);
  put16(buf + 12, g->pages_per_block);
  put16(buf + 14, g->total_blocks);
  put32(buf + 16, g->total_pages);
  buf[20] = g->mfr_id;
  buf[21] = g->dev_id;
  buf[22] = g->page_addr_bits;
  buf[23] = 0; // reserved
  put32(buf + 24, g->total_bytes);
  put32(buf + 28, dump_crc32(buf, 28));
}
```

- [ ] **Step 5: Run to verify pass**

Run: `pio test -e native -f test_pure`
Expected: PASS (6 tests total).

- [ ] **Step 6: Write the Python cross-check test**

Create `tests/test_dump_header.py`:

```python
import struct, zlib

FMT = "<6sBBHHHHIBBBBII"

def parse_header(buf):
    assert len(buf) == 32
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, page_addr_bits, _res, total_bytes, crc) = struct.unpack(FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver == 1, "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=page_addr_bits, flags=flags,
                total_bytes=total_bytes)

def build_header(**g):
    body = struct.pack(FMT[:-1], b"NANDMP", 1, g["flags"], g["page_size"],
                       g["spare_size"], g["pages_per_block"], g["total_blocks"],
                       g["total_pages"], g["mfr_id"], g["dev_id"],
                       g["page_addr_bits"], 0, g["total_bytes"])
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)

def test_roundtrip_micron():
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    assert parse_header(build_header(**g)) == g

def test_rejects_bad_crc():
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    buf = bytearray(build_header(**g)); buf[8] ^= 0xFF
    import pytest
    with pytest.raises(AssertionError, match="crc"):
        parse_header(bytes(buf))
```

> Note: `parse_header`/`build_header` are the reference implementation `dump.py` will import in Task 10. Placing them here first keeps the wire format test-locked.

- [ ] **Step 7: Run Python test to verify pass**

Run: `python -m pytest tests/test_dump_header.py -v`
Expected: PASS (2 tests).

- [ ] **Step 8: Commit**

```bash
git add src/dump_header.h src/dump_header.cpp test/test_pure/test_pure.cpp tests/test_dump_header.py
git commit -m "feat: 32-byte geometry wire header, C pack + Python cross-check"
```

---

## Task 5: Board pins + dual-target build config

**Files:**
- Create: `src/board_pins.h`
- Modify: `platformio.ini`
- Remove: `src/quad.cpp.bk`, `src/config.yml`

**Interfaces:**
- Produces: `NAND_PIN_CLK/D0/D1/D2/D3/CS` and `NAND_SPI_HOST` macros, per target.

- [ ] **Step 1: Create `src/board_pins.h`**

```c
#ifndef BOARD_PINS_H
#define BOARD_PINS_H
#include "driver/spi_master.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3 FSPI (SPI2) IOMUX pins. GPIO9-14 are plain I/O — not flash/PSRAM
  // (those are GPIO26-37), not strapping/USB/UART. Per S3 datasheet Table 2-1.
  #define NAND_PIN_CLK  12
  #define NAND_PIN_D0   11
  #define NAND_PIN_D1   13
  #define NAND_PIN_D2   14
  #define NAND_PIN_D3    9
  #define NAND_PIN_CS   10
  #define NAND_SPI_HOST SPI2_HOST
#else
  // ESP32-classic VSPI (SPI3) IOMUX pins.
  #define NAND_PIN_CLK  18
  #define NAND_PIN_D0   23
  #define NAND_PIN_D1   19
  #define NAND_PIN_D2   22
  #define NAND_PIN_D3   21
  #define NAND_PIN_CS    5
  #define NAND_SPI_HOST SPI3_HOST
#endif

#endif // BOARD_PINS_H
```

- [ ] **Step 2: Rewrite `platformio.ini` for dual targets + generator hook**

```ini
[env]
platform = espressif32
framework = arduino
monitor_speed = 115200
extra_scripts = pre:tools/gen_chips.py

[env:esp32dev]
board = esp32dev

[env:esp32-s3-devkitc-1]
board = esp32-s3-devkitc-1

[env:native]
platform = native
test_framework = unity
build_src_filter = -<*> +<nand_chips.cpp> +<dump_header.cpp>
build_flags = -I src
```

- [ ] **Step 3: Remove obsolete files**

```bash
git rm src/quad.cpp.bk src/config.yml
```

- [ ] **Step 4: Verify native tests still build/pass (no firmware regressions)**

Run: `pio test -e native -f test_pure`
Expected: PASS (6 tests). (Firmware envs are compile-checked in Task 6 once the driver uses these macros.)

- [ ] **Step 5: Commit**

```bash
git add src/board_pins.h platformio.ini
git commit -m "feat: per-target board pins and dual-target build config"
```

---

## Task 6: Driver refactor — runtime geometry, ECC, verify, ECC-status

**Files:**
- Modify: `src/nand_driver.h`, `src/nand_driver.cpp`

**Interfaces:**
- Consumes: `nand_chip_t`/`nand_read_mode_t` (Task 2), `nand_row_addr` (Task 3), board macros (Task 5).
- Produces: `esp_err_t nand_init(const nand_config_t *cfg)`; `uint16_t nand_read_id()`; `void nand_set_ecc(bool on)`; `uint8_t nand_get_ecc_status()`; `void nand_page_read_to_cache(uint32_t row_addr)`; `void nand_read_cache(uint8_t *buf, int len)`; `bool nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size, int max_retries, uint32_t *retry_count)`.

> **Testing note:** SPI transactions require hardware. This task is verified by **compile-check both firmware envs** plus **bench verification** (Task 14). The pure logic it depends on is already unit-tested (Tasks 2-4).

- [ ] **Step 1: Update `src/nand_driver.h`**

Replace the geometry `#define`s and the `nand_read_mode_t` enum (now in `nand_chips.h`) and widen row addresses:

```c
#ifndef NAND_DRIVER_H
#define NAND_DRIVER_H
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "nand_chips.h"   // nand_read_mode_t
#include "board_pins.h"

// Feature register addresses
#define NAND_FEATURE_BLOCK_LOCK  0xA0
#define NAND_FEATURE_CONFIG      0xB0   // Configuration reg (ECC_EN bit4)
#define NAND_FEATURE_STATUS      0xC0
#define NAND_CONFIG_ECC_EN       0x10   // B0h bit4
#define NAND_STATUS_OIP          0x01

typedef struct {
  int pin_clk, pin_d0, pin_d1, pin_d2, pin_d3, pin_cs;
  int clock_hz;
  nand_read_mode_t read_mode;
} nand_config_t;

#define NAND_DEFAULT_CONFIG() { \
  .pin_clk = NAND_PIN_CLK, .pin_d0 = NAND_PIN_D0, .pin_d1 = NAND_PIN_D1, \
  .pin_d2 = NAND_PIN_D2, .pin_d3 = NAND_PIN_D3, .pin_cs = NAND_PIN_CS, \
  .clock_hz = 1000000, .read_mode = NAND_READ_SINGLE }

esp_err_t nand_init(const nand_config_t *config, int max_page_size);
void      nand_reset(void);
void      nand_wait_ready(void);
uint8_t   nand_get_feature(uint8_t addr);
void      nand_set_feature(uint8_t addr, uint8_t value);
void      nand_set_ecc(bool on);
uint8_t   nand_get_ecc_status(void);          // 3-bit field
uint16_t  nand_read_id(void);
void      nand_page_read_to_cache(uint32_t row_addr);
void      nand_read_cache(uint8_t *buf, int len);
void      nand_read_cache_single(uint8_t *buf, int len);
void      nand_read_cache_quad(uint8_t *buf, int len);
bool      nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size,
                                  int max_retries, uint32_t *retry_count);
nand_read_mode_t nand_get_read_mode(void);
void      nand_set_read_mode(nand_read_mode_t m);

#endif // NAND_DRIVER_H
```

- [ ] **Step 2: Rewrite the changed parts of `src/nand_driver.cpp`**

Key changes vs current code — apply each:

`nand_init` takes `max_page_size` and sizes buffers/transfer to it, uses `NAND_SPI_HOST`, and no longer touches any QE bit here:

```c
#include "nand_driver.h"
#include "nand_addr.h"
#include <string.h>
#include <Arduino.h>

static spi_device_handle_t s_spi;
static nand_read_mode_t s_read_mode = NAND_READ_SINGLE;
static uint8_t *s_verify_buf = NULL;

esp_err_t nand_init(const nand_config_t *config, int max_page_size) {
  s_read_mode = config->read_mode;

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = config->pin_d0;
  buscfg.miso_io_num = config->pin_d1;
  buscfg.sclk_io_num = config->pin_clk;
  buscfg.quadwp_io_num = config->pin_d2;
  buscfg.quadhd_io_num = config->pin_d3;
  buscfg.max_transfer_sz = max_page_size + 16;

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = config->clock_hz;
  devcfg.mode = 0;
  devcfg.spics_io_num = config->pin_cs;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_HALFDUPLEX;
  devcfg.command_bits = 8;
  devcfg.address_bits = 0;

  esp_err_t ret = spi_bus_initialize(NAND_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) return ret;
  ret = spi_bus_add_device(NAND_SPI_HOST, &devcfg, &s_spi);
  if (ret != ESP_OK) return ret;

  s_verify_buf = (uint8_t *)heap_caps_malloc(max_page_size, MALLOC_CAP_DMA);
  if (!s_verify_buf) return ESP_ERR_NO_MEM;

  nand_reset();
  nand_wait_ready();
  return ESP_OK;
}
```

ECC toggle + 3-bit status (replaces the old 2-bit mask and the OTP/QE defines):

```c
void nand_set_ecc(bool on) {
  uint8_t cfg = nand_get_feature(NAND_FEATURE_CONFIG);
  if (on)  cfg |= NAND_CONFIG_ECC_EN;
  else     cfg &= ~NAND_CONFIG_ECC_EN;
  nand_set_feature(NAND_FEATURE_CONFIG, cfg);
}

uint8_t nand_get_ecc_status(void) {
  return (nand_get_feature(NAND_FEATURE_STATUS) >> 4) & 0x07; // ECCS0..2
}

void nand_set_read_mode(nand_read_mode_t m) { s_read_mode = m; }
```

Widen `nand_page_read_to_cache` to `uint32_t` (24 bits on the wire already):

```c
void nand_page_read_to_cache(uint32_t row_addr) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR;
  t.base.cmd = 0x13;
  t.base.addr = row_addr & 0xFFFFFF; // 24-bit row (17 used)
  t.address_bits = 24;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}
```

Fix the verify loop so a matching first pair passes regardless of `max_retries`, and widen the signature:

```c
bool nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size,
                             int max_retries, uint32_t *retry_count) {
  nand_page_read_to_cache(row_addr);
  nand_wait_ready();
  nand_read_cache(buf, page_size);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    nand_read_cache(s_verify_buf, page_size);
    if (memcmp(buf, s_verify_buf, page_size) == 0) return true;
    if (retry_count) (*retry_count)++;
    Serial.printf("[!] SPI mismatch at row 0x%06X (retry %d)\n", row_addr, attempt + 1);
    memcpy(buf, s_verify_buf, page_size);
  }
  return false;
}
```

Keep `nand_reset`, `nand_get_feature`, `nand_set_feature`, `nand_wait_ready`, `nand_read_cache_single/quad`, `nand_read_cache`, `nand_read_id`, `nand_get_read_mode` as in the current file (they are already correct), but change any `uint16_t row_addr` parameter to `uint32_t` and delete the old `NAND_OTP_*` usage.

- [ ] **Step 3: Compile-check both firmware targets**

Run: `pio run -e esp32dev` then `pio run -e esp32-s3-devkitc-1`
Expected: both compile (main.cpp still references old symbols — if so, this task's compile-check is deferred to Task 7; at minimum `pio test -e native -f test_pure` must still PASS).

> If `main.cpp` breaks the firmware build here, that is expected — Task 7 rewrites it. Confirm `pio test -e native` is green and proceed.

- [ ] **Step 4: Commit**

```bash
git add src/nand_driver.h src/nand_driver.cpp
git commit -m "refactor: runtime geometry, uint32 row, ECC toggle, verify + ecc-status fixes"
```

---

## Task 7: Boot / detect / menu restructure in `main.cpp`

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `nand_chip_lookup`, `nand_read_id`, `nand_init(cfg, max_page)`, `nand_set_ecc`, `nand_row_addr`, `dump_geometry_t`/`dump_header_pack`.

> **Testing:** compile-check both firmware envs + bench (Task 14).

- [ ] **Step 1: Restructure `setup()` to detect-then-configure**

Replace the current `setup()` body so the order is: SPI up at 1 MHz → read ID → lookup → seed a runtime `nand_info` (geometry + `ecc_default_on`) → run the menu (pre-filled) → re-apply final clock/mode → allocate the page buffer to the **selected** `page_size` → WiFi → dump. Concretely, replace the NAND-init block:

```c
  // ---- Bring SPI up slow, detect the chip ----
  nand_config_t nand_cfg = NAND_DEFAULT_CONFIG();
  nand_cfg.clock_hz = 1000000;
  nand_cfg.read_mode = NAND_READ_SINGLE;
  if (nand_init(&nand_cfg, MAX_PAGE_SIZE) != ESP_OK) {
    Serial.println("[!] NAND init failed"); return;
  }
  uint16_t id = nand_read_id();
  const nand_chip_t *chip = nand_chip_lookup(id >> 8, id & 0xFF);
  if (chip) {
    Serial.printf("[*] Detected %s (0x%02X 0x%02X)\n", chip->name, id >> 8, id & 0xFF);
    cfg_page_size = chip->page_size;
    cfg_spare_size = chip->spare_size;
    cfg_pages_per_block = chip->pages_per_block;
    cfg_total_blocks = chip->total_blocks;
    cfg_page_addr_bits = chip->page_addr_bits;
    cfg_bad_mark = chip->bad_block_mark;
    cfg_ecc_on = chip->ecc_default_on;   // global policy: off/raw
  } else {
    Serial.printf("[!] Unknown chip 0x%02X 0x%02X — using manual defaults\n", id >> 8, id & 0xFF);
  }
```

Define `#define MAX_PAGE_SIZE 8192` near the top and add the new runtime config vars (`cfg_spare_size`, `cfg_page_addr_bits`, `cfg_bad_mark`, `cfg_ecc_on`).

- [ ] **Step 2: Add an ECC toggle to the menu**

Add a menu line and handler:

```c
  Serial.printf( "  [E] ECC on read:     %s\n", cfg_ecc_on ? "ON (corrected)" : "OFF (raw)");
```
```c
      case 'E':
        cfg_ecc_on = !cfg_ecc_on;
        Serial.printf("  ECC: %s\n", cfg_ecc_on ? "ON" : "OFF");
        break;
```

- [ ] **Step 3: Apply ECC + read mode before dumping, and fix the row formula**

After the menu returns, before the dump:

```c
  nand_set_read_mode(cfg_read_mode);
  nand_set_ecc(cfg_ecc_on);
```

In `cmd_dump`, replace the row computation:

```c
      uint32_t row = nand_row_addr(block, page, cfg_page_addr_bits);
```

and change the loop counters and `page_buf` allocation to use the selected `cfg_page_size` (already parameterized) — ensure `nand_read_page_verified(row, page_buf, cfg_page_size, cfg_max_retries, &retryCount)` is called with the `uint32_t row`.

- [ ] **Step 4: Compile-check both firmware targets**

Run: `pio run -e esp32dev` then `pio run -e esp32-s3-devkitc-1`
Expected: both compile cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat: auto-detect chip on boot, ECC toggle, 32-bit row in dump loop"
```

---

## Task 8: Quad self-test with automatic fallback

**Files:**
- Modify: `src/nand_driver.h`, `src/nand_driver.cpp`, `src/main.cpp`

**Interfaces:**
- Produces: `bool nand_quad_selftest(uint32_t probe_row, int page_size)` — returns `true` if a quad read of `probe_row` matches a single read; enables QE first only when the active chip has one.

> **Testing:** compile-check + bench (Task 14). The comparison logic is trivial; correctness is a hardware property.

- [ ] **Step 1: Add `nand_quad_selftest` to the driver**

```c
// Read probe_row via single and via quad; return true if they match.
bool nand_quad_selftest(uint32_t probe_row, int page_size) {
  nand_page_read_to_cache(probe_row);
  nand_wait_ready();
  nand_read_cache_single(s_verify_buf, page_size);
  static uint8_t *q = NULL;
  if (!q) q = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_DMA);
  if (!q) return false;
  nand_read_cache_quad(q, page_size);
  return memcmp(s_verify_buf, q, page_size) == 0;
}
```

Declare it in `nand_driver.h`.

- [ ] **Step 2: Gate quad selection in `main.cpp`**

After `nand_set_read_mode(cfg_read_mode)` and before dumping:

```c
  if (cfg_read_mode == NAND_READ_QUAD) {
    if (chip && chip->has_qe_bit)
      nand_set_feature(chip->qe_feature_addr,
                       nand_get_feature(chip->qe_feature_addr) | chip->qe_bit);
    uint32_t probe = nand_row_addr(1, 0, cfg_page_addr_bits);
    if (!nand_quad_selftest(probe, cfg_page_size)) {
      Serial.println("[!] Quad self-test FAILED — falling back to single x1");
      cfg_read_mode = NAND_READ_SINGLE;
      nand_set_read_mode(NAND_READ_SINGLE);
    } else {
      Serial.println("[+] Quad self-test passed");
    }
  }
```

- [ ] **Step 3: Compile-check both firmware targets**

Run: `pio run -e esp32dev && pio run -e esp32-s3-devkitc-1`
Expected: both compile.

- [ ] **Step 4: Commit**

```bash
git add src/nand_driver.h src/nand_driver.cpp src/main.cpp
git commit -m "feat: quad self-test with automatic fallback to single"
```

---

## Task 9: Emit geometry header before the dump

**Files:**
- Modify: `src/wifi_transport.h`, `src/wifi_transport.cpp`, `src/main.cpp`

**Interfaces:**
- Produces: `size_t wifi_transport_send(const uint8_t *, size_t)` already exists; reuse it to send the 32-byte header first.

> **Testing:** the header bytes are unit-tested (Task 4); this task is compile-check + bench.

- [ ] **Step 1: Build and send the header in `cmd_dump`**

Immediately after the `'G'` trigger and before the page loop:

```c
  dump_geometry_t geo = {0};
  geo.page_size = cfg_page_size;
  geo.spare_size = cfg_spare_size;
  geo.pages_per_block = cfg_pages_per_block;
  geo.total_blocks = cfg_total_blocks;
  geo.total_pages = (uint32_t)cfg_total_blocks * cfg_pages_per_block;
  geo.total_bytes = geo.total_pages * (uint32_t)cfg_page_size;
  geo.mfr_id = id >> 8; geo.dev_id = id & 0xFF;
  geo.page_addr_bits = cfg_page_addr_bits;
  geo.flags = (cfg_ecc_on ? DUMP_FLAG_ECC_ON : 0)
            | (cfg_read_mode == NAND_READ_QUAD ? DUMP_FLAG_QUAD : 0)
            | (cfg_verify ? DUMP_FLAG_VERIFY : 0);
  uint8_t hdr[DUMP_HEADER_SIZE];
  dump_header_pack(hdr, &geo);
  wifi_transport_send(hdr, sizeof(hdr));
```

Include `"dump_header.h"` in `main.cpp`. Make `id` visible to `cmd_dump` (pass it in or store as a static set in `setup()`).

- [ ] **Step 2: Compile-check both firmware targets**

Run: `pio run -e esp32dev && pio run -e esp32-s3-devkitc-1`
Expected: both compile.

- [ ] **Step 3: Commit**

```bash
git add src/wifi_transport.h src/wifi_transport.cpp src/main.cpp
git commit -m "feat: send 32-byte geometry header before the page stream"
```

---

## Task 10: `dump.py` — self-configure from header + sidecar metadata

**Files:**
- Modify: `dump.py`
- Create: `tests/test_dump_client.py`

**Interfaces:**
- Consumes: `parse_header` (wire format from Task 4).
- Produces: `recv_exact(sock, n) -> bytes`; `write_metadata(path, geom, chip_id, byte_count)`.

- [ ] **Step 1: Write failing tests**

Create `tests/test_dump_client.py`:

```python
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.test_dump_header import build_header
import dump

def test_parse_header_reads_geometry():
    buf = build_header(page_size=2176, spare_size=128, pages_per_block=64,
                       total_blocks=2048, total_pages=2048*64,
                       total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
                       page_addr_bits=6, flags=0)
    g = dump.parse_header(buf)
    assert g["total_bytes"] == 2048 * 64 * 2176
    assert g["page_size"] == 2176

def test_write_metadata_roundtrip(tmp_path):
    g = dict(page_size=2176, spare_size=128, pages_per_block=64, total_blocks=2048,
             total_pages=2048*64, total_bytes=2048*64*2176, mfr_id=0x2C, dev_id=0x24,
             page_addr_bits=6, flags=0)
    p = tmp_path / "d.bin"
    dump.write_metadata(str(p), g, byte_count=g["total_bytes"])
    meta = json.load(open(str(p) + ".meta.json"))
    assert meta["geometry"]["page_size"] == 2176
    assert meta["bytes_received"] == g["total_bytes"]
```

- [ ] **Step 2: Run to verify failure**

Run: `python -m pytest tests/test_dump_client.py -v`
Expected: FAIL — `dump.parse_header` / `write_metadata` missing.

- [ ] **Step 3: Refactor `dump.py`**

Move the wire helpers in and add metadata; drive the receive loop off the header. Add near the top:

```python
import struct, zlib, json, datetime

HEADER_FMT = "<6sBBHHHHIBBBBII"
HEADER_SIZE = 32

def parse_header(buf):
    assert len(buf) == HEADER_SIZE, "short header"
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, pab, _r, total_bytes, crc) = struct.unpack(HEADER_FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver == 1, "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=pab, flags=flags, total_bytes=total_bytes)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed mid-header")
        buf += chunk
    return buf

def write_metadata(out_path, geom, byte_count):
    meta = {
        "geometry": geom,
        "ecc_on": bool(geom["flags"] & 0x01),
        "quad": bool(geom["flags"] & 0x02),
        "verify": bool(geom["flags"] & 0x04),
        "bytes_received": byte_count,
        "timestamp": datetime.datetime.now().isoformat(),
    }
    with open(out_path + ".meta.json", "w") as f:
        json.dump(meta, f, indent=2)
```

Then in the main flow: after `sock.sendall(b'G')`, do `geom = parse_header(recv_exact(sock, HEADER_SIZE))`, set `TOTAL_BYTES = geom["total_bytes"]`, `PAGE_SIZE = geom["page_size"]`, replace the tangled progress condition with a simple `if current_page % PROGRESS_INTERVAL == 0:`, and after the loop call `write_metadata(OUTPUT_FILE, geom, bytes_received)`.

- [ ] **Step 4: Run to verify pass**

Run: `python -m pytest tests/test_dump_client.py -v`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add dump.py tests/test_dump_client.py
git commit -m "feat: dump.py self-configures from header, writes .meta.json"
```

---

## Task 11: `ecc_stripper.py` — parameterized geometry

**Files:**
- Modify: `ecc_stripper.py`
- Create: `tests/test_ecc_stripper.py`

**Interfaces:**
- Produces: `strip(in_path, out_path, page_size, spare_size, pages_per_block, bad_mark=0x00, good=0xFF) -> list[int]` (returns bad block numbers); `load_geometry(meta_path) -> dict`.

- [ ] **Step 1: Write failing tests**

Create `tests/test_ecc_stripper.py`:

```python
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import ecc_stripper

def _make_dump(path, page_size, spare_size, pages_per_block, blocks, bad_blocks):
    main = page_size - spare_size
    with open(path, "wb") as f:
        for b in range(blocks):
            for p in range(pages_per_block):
                page = bytearray(b"\xAA" * main + b"\xFF" * spare_size)
                if p == 0 and b in bad_blocks:
                    page[main] = 0x00  # bad-block marker at spare[0]
                f.write(page)

def test_strip_sizes_and_detects_bad(tmp_path):
    inp, out = str(tmp_path/"raw.bin"), str(tmp_path/"clean.bin")
    _make_dump(inp, 2176, 128, 64, blocks=4, bad_blocks={2})
    bad = ecc_stripper.strip(inp, out, 2176, 128, 64)
    assert bad == [2]
    assert os.path.getsize(out) == 4 * 64 * 2048       # main-only
    data = open(out, "rb").read()
    # block 2 padded with 0xFF
    blk2 = data[2*64*2048:(2*64+1)*2048]
    assert blk2 == b"\xFF" * 2048

def test_strip_2112_geometry(tmp_path):
    inp, out = str(tmp_path/"raw.bin"), str(tmp_path/"clean.bin")
    _make_dump(inp, 2112, 64, 64, blocks=2, bad_blocks=set())
    ecc_stripper.strip(inp, out, 2112, 64, 64)
    assert os.path.getsize(out) == 2 * 64 * 2048
```

- [ ] **Step 2: Run to verify failure**

Run: `python -m pytest tests/test_ecc_stripper.py -v`
Expected: FAIL — `strip` missing.

- [ ] **Step 3: Rewrite `ecc_stripper.py`**

```python
"""Strip spare/OOB from a raw NAND dump into a main-area image.

Geometry comes from the dump's .meta.json sidecar (written by dump.py),
or from --page-size/--spare-size/--pages-per-block overrides.
"""
import argparse, json, os

def load_geometry(meta_path):
    with open(meta_path) as f:
        g = json.load(f)["geometry"]
    return g

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
    ap = argparse.ArgumentParser()
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
        ps, ss, ppb = a.page_size, a.spare_size, a.pages_per_block
    bad = strip(a.input, a.output, ps, ss, ppb)
    print(f"[*] {len(bad)} bad block(s): {bad}")
    print(f"[*] wrote {a.output}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run to verify pass**

Run: `python -m pytest tests/test_ecc_stripper.py -v`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add ecc_stripper.py tests/test_ecc_stripper.py
git commit -m "feat: parameterized ecc_stripper driven by dump metadata"
```

---

## Task 12: Test coverage for `binary_compare_fix.py`

**Files:**
- Create: `tests/test_binary_compare_fix.py`

**Interfaces:**
- Consumes: existing `compare_and_fix(file_paths, output_path, report_path)`.

- [ ] **Step 1: Write the test**

```python
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "tools"))
import binary_compare_fix as bcf

def test_majority_vote_fixes_minority(tmp_path):
    a = tmp_path/"a.bin"; b = tmp_path/"b.bin"; c = tmp_path/"c.bin"
    a.write_bytes(b"\x01\x02\x03")
    b.write_bytes(b"\x01\xFF\x03")   # middle byte corrupted in one file
    c.write_bytes(b"\x01\x02\x03")
    out = tmp_path/"fixed.bin"; rep = tmp_path/"rep.txt"
    bcf.compare_and_fix([str(a), str(b), str(c)], str(out), str(rep))
    assert out.read_bytes() == b"\x01\x02\x03"
```

- [ ] **Step 2: Run to verify pass (no code change needed)**

Run: `python -m pytest tests/test_binary_compare_fix.py -v`
Expected: PASS. If the import of `binary_compare_fix` triggers `argparse`, confirm the module guards execution under `if __name__ == "__main__"` (it does).

- [ ] **Step 3: Commit**

```bash
git add tests/test_binary_compare_fix.py
git commit -m "test: majority-vote coverage for binary_compare_fix"
```

---

## Task 13: Documentation

**Files:**
- Modify: `README.md`
- Create: `CONTRIBUTING.md`

- [ ] **Step 1: Rewrite the chip-specific parts of `README.md`**

Replace the "DS35Q2GA" identity and mixed 2112/2176 numbers with the proven MT29F2G01 geometry; document: auto-detect, the ECC on/off toggle (default OFF/raw and why), the `chips.yml` registry, the S3 env (`pio run -e esp32-s3-devkitc-1`), level-shifter wiring for 1.8 V parts, the `.meta.json` sidecar, and `ecc_stripper.py --meta`. Correct the feature-register section: register B0h is the **Configuration** register (ECC_EN bit 4), Micron has **no QE bit**, and SET FEATURE needs **no WRITE ENABLE**.

- [ ] **Step 2: Write `CONTRIBUTING.md`**

```markdown
# Adding a NAND chip

1. Power up with your chip wired per the README. The boot log prints:
   `spi_nand_probe: mfr_id=0xNN, dev_id=0xNN` — that's the JEDEC id.
2. Add a block to `chips.yml` (copy an existing one). Fill page_size (main +
   spare), spare_size, pages_per_block, total_blocks, bad_block_mark, has_qe_bit,
   ecc_default, vcc_mv — all from the chip's datasheet.
3. `pio run -e esp32dev` regenerates `src/nand_chips_generated.h` and builds.
4. Verify a dump, then open a PR including the datasheet reference in `notes`.

Fields are validated at build time; a malformed entry fails the build with a
message naming the field.
```

- [ ] **Step 3: Verify the full test suite is green**

Run: `python -m pytest -q` and `pio test -e native`
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add README.md CONTRIBUTING.md
git commit -m "docs: reconcile chip identity, document registry/ECC/S3/metadata"
```

---

## Task 14: Bench verification (hardware — manual)

**Files:** none (verification checklist).

> This task cannot be automated. Run it on real hardware and record results.

- [ ] **Step 1:** Flash `esp32dev`, wire the MT29F2G01, boot. Confirm the log shows `Detected MT29F2G01ABAGD (0x2C 0x24)` and geometry `2048 blocks x 64 x 2176`.
- [ ] **Step 2:** Run `python dump.py`. Confirm it prints geometry parsed from the header and the expected total (`286,261,248` bytes = 2048×64×2176) and finishes without a size-mismatch warning.
- [ ] **Step 3:** Confirm `<dump>.meta.json` exists with `page_size=2176`, `ecc_on=false`.
- [ ] **Step 4:** Take **two** dumps at 1 MHz single mode; diff them (`cmp`) — expect identical or a handful of bytes (feeds `binary_compare_fix`).
- [ ] **Step 5:** `python ecc_stripper.py <dump>.bin clean.bin --meta <dump>.bin.meta.json` — confirm output size = `2048×64×2048` and any bad blocks are reported.
- [ ] **Step 6:** Toggle Quad in the menu; confirm the self-test either passes or logs a clean fallback to single (no aborted init).
- [ ] **Step 7 (if S3 available):** Flash `esp32-s3-devkitc-1`, wire per `board_pins.h`, repeat Steps 1–2.
- [ ] **Step 8:** Record results (chip id, sizes, retry counts, quad pass/fail) in the PR description.

---

## Self-Review Notes

- **Spec coverage:** registry+codegen (T1), lookup (T2), row-address fix (T3), wire header (T4), board/dual-target+forbidden-pins (T5), driver runtime geometry / uint32 row / ECC toggle / ecc-status mask / verify fix (T6), boot-detect+ECC menu (T7), quad self-test+fallback (T8), geometry handshake (T9), dump.py+metadata (T10), ecc_stripper (T11), compare tool tests (T12), README+CONTRIBUTING (T13), bench (T14). All spec sections mapped.
- **Open questions carried forward:** DS35 JEDEC id is a placeholder in `chips.yml` (flagged inline); S3 pins assume a WROOM-1-class module.
- **Deferred firmware compile-check:** Task 6 may leave the firmware build red until Task 7 lands; native tests stay green throughout. Do Tasks 6 and 7 back-to-back.
