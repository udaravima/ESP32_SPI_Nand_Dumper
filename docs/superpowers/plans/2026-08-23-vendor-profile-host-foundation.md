# Vendor/Family Profile — Host Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the host-side chip database, the flat `active_profile_t` wire contract, and the native cross-language tests that lock host and device to the same struct — with zero change to device behavior.

**Architecture:** A three-layer YAML DB (`db/families`, `db/profiles`, `db/chips`) is loaded, validated, and *flattened* by one host module (`tools/chipdb.py`) into a single packed struct. A new pure-C module (`src/nand_profile.*`) defines that struct and a data-driven ECC decoder. A generator emits the flattened struct both as a C golden blob (for a native unpack-and-assert test) and as a resident C array — but nothing on the device *includes* the resident array yet, so this stage is additive only. Stages 2 (device read-path refactor) and 3 (push protocol) are separate follow-on plans.

**Tech Stack:** Python 3 + PyYAML + pytest (host tools); C11 + PlatformIO Unity on the `native` env (pure logic); `struct.pack`/`__attribute__((packed))` for the byte-identical wire contract.

**Spec:** [docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md](../specs/2026-08-23-vendor-profile-architecture-design.md) — this plan implements **Stage 1** of § 10 ("host DB + validation + tests, no device change"). Read § 3 (data model), § 4.1 (flat struct), § 4.2 (decoder), § 6 (validation tiers), § 9 (testing).

## Global Constraints

- **Commit permission is per-action.** The user's global `CLAUDE.md` overrides this plan's `git commit` steps: **do the work, then stop and ask before every commit**. Approval for one commit does not carry to the next. The commit step in each task is a checkpoint, not a license.
- **Zero device behavior change this stage.** The existing `chips.yml` → `gen_chips.py` → `nand_chips_generated.h` → `CHIPS[]` path stays intact and is not modified. New artifacts are additive. Do not touch `src/main.cpp`, `src/nand_driver.*`, or `src/nand_ecc.h` in this stage.
- **Byte-identical wire contract.** The C `active_profile_t` and the Python packer agree on field order, standard sizes, little-endian byte order, and name padding. Enforced by `static_assert(sizeof(active_profile_t) == 110)` in C and the golden-blob cross-check.
- **Fail-closed validation.** Every rejection in `chipdb.validate()` raises a specific exception; nothing is "loaded anyway." Unnamed ECC-map entries decode to `UNCOR`.
- **`schema_ver` starts at 1.** Any change to the flat-struct layout bumps it (later stages).
- **DB layout is `db/families/`, `db/profiles/`, `db/chips/`** — one YAML file each, structured for later extraction to a git submodule.
- **Native env facts:** pure modules must be Arduino/ESP-IDF-free and added to `build_src_filter` in `[env:native]`; tests are Unity, one directory per suite under `test/`.

---

### Task 1: Flat `active_profile_t` struct + severity enum (the wire contract)

Defines the single struct every later task packs into or reads from. Locks `sizeof` at compile time so any field/order/padding drift fails the build, not the device.

**Files:**
- Create: `src/nand_profile.h`
- Create: `src/nand_profile.cpp`
- Modify: `platformio.ini:31` (add `nand_profile.cpp` to the native `build_src_filter`)
- Test: `test/test_profile/test_profile.cpp`

**Interfaces:**
- Produces: `typedef struct { ... } active_profile_t;` (packed, `sizeof == 110`); `typedef enum { NAND_SEV_OK=0, NAND_SEV_CORRECTED=1, NAND_SEV_CORRECTED_REFRESH=2, NAND_SEV_UNCORRECTABLE=3 } nand_severity_t;`; `#define NAND_PROFILE_SIZE 110`.

- [ ] **Step 1: Write the failing test**

`test/test_profile/test_profile.cpp`:
```c
#include <unity.h>
#include "nand_profile.h"

void test_active_profile_is_110_bytes(void) {
  // The wire contract: host packs 110 bytes, device reads 110 bytes.
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)sizeof(active_profile_t));
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)NAND_PROFILE_SIZE);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_active_profile_is_110_bytes);
  return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_profile`
Expected: FAIL — `nand_profile.h` not found / does not compile.

- [ ] **Step 3: Write minimal implementation**

`src/nand_profile.h`:
```c
#ifndef NAND_PROFILE_H
#define NAND_PROFILE_H
#include <stdint.h>

#define NAND_PROFILE_SIZE 110

typedef enum {
  NAND_SEV_OK = 0,
  NAND_SEV_CORRECTED = 1,
  NAND_SEV_CORRECTED_REFRESH = 2,
  NAND_SEV_UNCORRECTABLE = 3,
} nand_severity_t;

// Flat, packed wire struct. Host flattens the 3 YAML layers into this; the
// device holds one copy in RAM. Packed so the Python '<' packer (no alignment
// padding) matches byte-for-byte; GCC emits safe byte-wise member access.
typedef struct __attribute__((packed)) {
  char     name[24];                       // NUL-terminated; strlen <= 23
  uint32_t page_size, spare_size, pages_per_block, total_blocks;
  uint8_t  op_page_read, op_read_cache, op_get_feat, op_set_feat, op_status_addr, op_cfg_addr;
  uint8_t  ecc_en_bit;                     // config (B0h) bit toggling on-die ECC
  uint8_t  ecc_shift, ecc_mask;
  uint8_t  ecc_map[16];                    // field value -> nand_severity_t; unmapped => UNCOR
  uint8_t  status2_reg;                    // 0=none; 0x30 Winbond / 0xF0 GigaDevice (deferred)
  uint8_t  id_method, id_n_bytes;          // 0=addr,1=dummy; ID byte count
  uint8_t  qe_addr, qe_bit;                // 0 if none
  uint8_t  read_mode;
  uint16_t vcc_mv;
  uint8_t  bbm_off, bbm_len, bbm_good;
  uint8_t  oob_free_n, oob_ecc_n;          // valid region counts, each <= 4
  uint16_t oob_free[8];                    // up to 4 x (off,len)
  uint16_t oob_ecc[8];                     // up to 4 x (off,len)
} active_profile_t;

_Static_assert(sizeof(active_profile_t) == NAND_PROFILE_SIZE,
               "active_profile_t layout drifted from the 110-byte wire contract");

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status);

#endif // NAND_PROFILE_H
```

`src/nand_profile.cpp` (decoder body filled in Task 2; a stub keeps the link green now):
```c
#include "nand_profile.h"

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status) {
  (void)p; (void)status;
  return NAND_SEV_UNCORRECTABLE; // conservative stub; real decode in Task 2
}
```

`platformio.ini` — extend the native filter (append `+<nand_profile.cpp>`):
```ini
build_src_filter = -<*> +<nand_chips.cpp> +<dump_header.cpp> +<config_store.cpp> +<sys_info.cpp> +<nand_profile.cpp>
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native -f test_profile`
Expected: PASS. If `sizeof` != 110, the `_Static_assert` fails the compile with a clear message — recount the layout in the spec § 4.1.

- [ ] **Step 5: Commit** *(ask first — see Global Constraints)*

```bash
git add src/nand_profile.h src/nand_profile.cpp platformio.ini test/test_profile/test_profile.cpp
git commit -m "feat(profile): flat active_profile_t wire struct + severity enum"
```

---

### Task 2: Data-driven ECC decoder + full scheme table test

Replaces the fixed `if uncorrectable / if refresh` ladder with one indexed lookup. The test exercises every named scheme, including the 4-bit-field schemes that drove the `ecc_map[16]` fix.

**Files:**
- Modify: `src/nand_profile.cpp`
- Test: `test/test_profile/test_profile.cpp:` (add cases)

**Interfaces:**
- Consumes: `active_profile_t`, `nand_severity_t` (Task 1).
- Produces: `nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status)` — real body.

- [ ] **Step 1: Write the failing test**

Add to `test/test_profile/test_profile.cpp` (and add the `RUN_TEST` lines in `main`):
```c
static active_profile_t mk(uint8_t shift, uint8_t mask, const uint8_t map[16]) {
  active_profile_t p; // zeroed fields we don't touch don't matter here
  p.ecc_shift = shift; p.ecc_mask = mask;
  for (int i = 0; i < 16; i++) p.ecc_map[i] = map[i];
  return p;
}

void test_decode_generic2(void) {
  // [5:4], mask 0x3: 0=OK 1=CORR 2=UNCOR 3=UNCOR
  const uint8_t m[16] = {0,1,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3};
  active_profile_t p = mk(4, 0x3, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_OK,            nand_profile_severity(&p, 0x00));
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED,     nand_profile_severity(&p, 0x10)); // field 1
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x20)); // field 2
}

void test_decode_micron3(void) {
  // [6:4], mask 0x7: 0=OK 1=CORR 2=UNCOR 3=REFRESH 5=REFRESH
  const uint8_t m[16] = {0,1,3,2, 3,2,3,3, 3,3,3,3, 3,3,3,3};
  active_profile_t p = mk(4, 0x7, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE,        nand_profile_severity(&p, 0x20)); // field 2
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH,    nand_profile_severity(&p, 0x30)); // field 3
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH,    nand_profile_severity(&p, 0x50)); // field 5
}

void test_decode_xtx4_reaches_index_15(void) {
  // [7:4], mask 0xF, uncorrectable at field 0xF — the Wall-1 out-of-bounds case
  uint8_t m[16]; m[0]=0; for (int i=1;i<15;i++) m[i]=1; m[15]=3;
  active_profile_t p = mk(4, 0xF, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0xF0)); // field 15
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED,     nand_profile_severity(&p, 0x50)); // field 5
}

void test_decode_xtx_g0xa(void) {
  // [5:2], mask 0xF, uncorrectable at field 8, refresh at 12 (mainline encoding)
  uint8_t m[16]; for (int i=0;i<16;i++) m[i]=1; m[0]=0; m[8]=3; m[12]=2;
  active_profile_t p = mk(2, 0xF, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE,     nand_profile_severity(&p, 8u<<2)); // field 8
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH, nand_profile_severity(&p, 12u<<2)); // field 12
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_profile`
Expected: FAIL — the stub returns `UNCORRECTABLE` for everything, so `test_decode_generic2` fails on the OK/CORRECTED cases.

- [ ] **Step 3: Write minimal implementation**

Replace the body in `src/nand_profile.cpp`:
```c
#include "nand_profile.h"

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status) {
  uint8_t field = (uint8_t)((status >> p->ecc_shift) & p->ecc_mask);
  return (nand_severity_t)p->ecc_map[field]; // field is 0..15; ecc_map is 16 wide
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native -f test_profile`
Expected: PASS (all decode cases).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add src/nand_profile.cpp test/test_profile/test_profile.cpp
git commit -m "feat(profile): pure-data ECC severity decoder"
```

---

### Task 3: `db/` YAML layers + `chipdb.load()`

Migrates the two chips out of `chips.yml` into the three-layer DB and gives `chipdb.py` a loader that resolves every `chip → profile → family` reference (dangling refs fail loud).

**Files:**
- Create: `db/families/spi-nand.yml`, `db/profiles/micron.yml`, `db/profiles/dosilicon.yml`, `db/chips/MT29F2G01ABAGD.yml`, `db/chips/DS35Q1GA.yml`
- Create: `tools/chipdb.py`
- Test: `tests/test_chipdb.py`

**Interfaces:**
- Produces: `chipdb.load(root="db") -> DB` where `DB` has `.families: dict`, `.profiles: dict`, `.chips: dict` (each keyed by `name`); `chipdb.resolve_refs(db)` raises `chipdb.RefError` on a dangling `family`/`profile`. `chipdb.get(db, chip_name) -> dict` returns the chip merged with its family and profile under keys `family`/`profile`.

- [ ] **Step 1: Write the failing test**

`tests/test_chipdb.py`:
```python
import pytest
import chipdb

def test_load_resolves_two_chips():
    db = chipdb.load()
    assert set(db.chips) == {"MT29F2G01ABAGD", "DS35Q1GA"}
    ds = chipdb.get(db, "DS35Q1GA")
    assert ds["id"]["mfr"] == 0xE5 and ds["id"]["dev"] == 0x71
    assert ds["profile"]["ecc"]["scheme"] == "generic2"
    assert ds["family"]["feature_addrs"]["config"] == 0xB0

def test_dangling_profile_ref_raises(tmp_path):
    (tmp_path / "families").mkdir(); (tmp_path / "profiles").mkdir(); (tmp_path / "chips").mkdir()
    (tmp_path / "families" / "spi-nand.yml").write_text("name: spi-nand\n")
    (tmp_path / "chips" / "X.yml").write_text(
        "name: X\nfamily: spi-nand\nprofile: nope\nid: {mfr: 1, dev: 2}\n")
    with pytest.raises(chipdb.RefError):
        chipdb.resolve_refs(chipdb.load(root=str(tmp_path)))
```

Add the import paths — create `tests/conftest.py` if it does not exist:
```python
import os, sys
_here = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(_here, "..", "tools"))   # chipdb, gen_profiles
sys.path.insert(0, os.path.join(_here, ".."))            # project root (existing tests' convention)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/test_chipdb.py -v`
Expected: FAIL — `ModuleNotFoundError: chipdb` (module not yet created).

- [ ] **Step 3: Write minimal implementation**

Create the DB files.

`db/families/spi-nand.yml`:
```yaml
name: spi-nand
opcodes:
  reset: 0xFF
  read_id: 0x9F
  get_feature: 0x0F
  set_feature: 0x1F
  page_read: 0x13
  read_cache: {x1: 0x0B, x2: 0x3B, x4: 0x6B}
  program_load: {x1: 0x02, x4: 0x32}
  program_exec: 0x10
  block_erase: 0xD8
  write_enable: 0x06
  write_disable: 0x04
feature_addrs: {block_lock: 0xA0, config: 0xB0, status: 0xC0}
status_bits: {oip: 0, wel: 1, erase_fail: 2, prog_fail: 3}
config_ecc_en_bit: 4
address_model: {row_bits: 24, read_dummy_bytes: 1, col_bits: 12}
read_id_default: {method: dummy, id_bytes: 2}
```

`db/profiles/dosilicon.yml`:
```yaml
name: dosilicon
datasheet: "DS35x1GAxxx (Dosilicon); matches mainline Linux spinand dosilicon driver"
ecc: {scheme: generic2, strength: 4, status2_reg: null}
qe: {has: true, feature_addr: 0xB0, bit: 0x01}
oob_layout:
  # Region byte-offsets are consumed only in Stage 2 (stripper/BBT). Leaving them
  # EMPTY here keeps the golden blob a correct regression target and avoids baking
  # in unverified offsets. The real per-vendor layout (Dosilicon is 6-free/8-ECC,
  # stride 14) is authored + checked against mainline dosilicon.c in Stage 2.
  free_regions: []
  ecc_regions:  []
  bbm: {off: 0, len: 2, good: 0xFF, pages: [first]}
config_ecc_en_bit: null
```

`db/profiles/micron.yml`:
```yaml
name: micron
datasheet: "MT29F2G01ABAGD (Micron), 8-bit/512 on-die ECC"
ecc: {scheme: micron3, strength: 8, status2_reg: null}
qe: {has: false}
oob_layout:
  free_regions: []          # see dosilicon.yml note — real MT29F layout lands in Stage 2
  ecc_regions:  []
  bbm: {off: 0, len: 1, good: 0xFF, pages: [first]}
config_ecc_en_bit: null
```

`db/chips/DS35Q1GA.yml`:
```yaml
name: DS35Q1GA
id: {mfr: 0xE5, dev: 0x71, dev2: null, onfi: null}
family: spi-nand
profile: dosilicon
geometry: {page_size: 2112, spare_size: 64, pages_per_block: 64, total_blocks: 1024, planes: 1}
read_mode: single
vcc_mv: 3300
read_id: {method: dummy, id_bytes: 2}
datasheet: "docs/datasheets/DS35x1GAxxx_SPI_NAND.pdf"
notes: "off a Huawei ONT; boot region blocks 0-7 raw; 1Gb SLC FORESEE/Dosilicon"
resident: true
overrides: {}
```

`db/chips/MT29F2G01ABAGD.yml`:
```yaml
name: MT29F2G01ABAGD
id: {mfr: 0x2C, dev: 0x24, dev2: null, onfi: null}
family: spi-nand
profile: micron
geometry: {page_size: 2176, spare_size: 128, pages_per_block: 64, total_blocks: 2048, planes: 2}
read_mode: single
vcc_mv: 3300
datasheet: "MT29F2G01ABAGD datasheet (untracked, local)"
notes: "2Gb SLC, 8-bit/512 on-die ECC, 2 planes x 1024 blocks"
resident: true
overrides: {}
```

`tools/chipdb.py`:
```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/test_chipdb.py -v`
Expected: PASS (both tests).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add db/ tools/chipdb.py tests/test_chipdb.py tests/conftest.py
git commit -m "feat(chipdb): three-layer db/ + loader with ref resolution"
```

---

### Task 4: `chipdb.validate()` — tiers 2/3 + scheme expansion + host capacity check

Ports the spec's fail-closed checks to the host: structural bounds, semantic geometry, the market-plausibility capacity window (device-side no longer carries it), and expansion of the named ECC `scheme` into a fully-populated 16-entry map.

**Files:**
- Modify: `tools/chipdb.py`
- Test: `tests/test_chipdb.py` (add cases)

**Interfaces:**
- Consumes: `get(db, name)` (Task 3).
- Produces: `chipdb.SCHEMES: dict[str, tuple[int,int,list[int]]]` (scheme → `(shift, mask, map16)`); `chipdb.expand_scheme(name) -> (shift, mask, map16)` raising `ValidationError` on unknown scheme; `chipdb.validate(chip) -> None` raising `ValidationError` on any failed check.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_chipdb.py`:
```python
def test_scheme_expands_to_16_entries_uncor_default():
    shift, mask, m = chipdb.expand_scheme("generic2")
    assert (shift, mask) == (4, 0x3)
    assert len(m) == 16
    assert m[2] == chipdb.UNCOR          # field 2 = uncorrectable
    assert all(v == chipdb.UNCOR for v in m[4:])  # unnamed => UNCOR

def test_validate_accepts_both_shipped_chips():
    db = chipdb.load()
    for name in db.chips:
        chipdb.validate(chipdb.get(db, name))  # must not raise

def test_validate_rejects_spare_ge_page():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["spare_size"] = c["geometry"]["page_size"]
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_non_power_of_two_ppb():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["pages_per_block"] = 63
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_capacity_out_of_window():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["geometry"]["total_blocks"] = 1        # ~2 Mb, below 512 Mb floor
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_overlong_name():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["name"] = "X" * 24                      # 24 chars, no room for NUL in name[24]
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_too_many_oob_regions():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["profile"]["oob_layout"]["free_regions"] = [[0, 1]] * 5   # > 4 would truncate
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/test_chipdb.py -k "scheme or validate" -v`
Expected: FAIL — `AttributeError: module 'chipdb' has no attribute 'expand_scheme'`.

- [ ] **Step 3: Write minimal implementation**

Append to `tools/chipdb.py`:
```python
OK, CORR, REFRESH, UNCOR = 0, 1, 2, 3

def _map16(named, default=UNCOR):
    m = [default] * 16
    for i, v in named.items():
        m[i] = v
    return m

# scheme -> (ecc_shift, ecc_mask, 16-entry severity map)
SCHEMES = {
    "generic2": (4, 0x3, _map16({0: OK, 1: CORR, 2: UNCOR, 3: UNCOR})),
    "micron3":  (4, 0x7, _map16({0: OK, 1: CORR, 2: UNCOR, 3: REFRESH, 5: REFRESH})),
    "gd_uc":    (4, 0x7, _map16({0: OK, 1: CORR, 2: CORR, 3: CORR,
                                 4: CORR, 5: REFRESH, 6: REFRESH, 7: UNCOR})),
    "xtx4":     (4, 0xF, _map16({**{i: CORR for i in range(1, 15)}, 0: OK, 15: UNCOR})),
    "xtx_g0xa": (2, 0xF, _map16({**{i: CORR for i in range(1, 16)}, 0: OK, 8: UNCOR, 12: REFRESH})),
}

# main-area capacity window: 512 Mb .. 8 Gb, expressed in bytes (64 MiB .. 1 GiB)
_CAP_MIN = 64 * 1024 * 1024
_CAP_MAX = 1024 * 1024 * 1024


def expand_scheme(name):
    if name not in SCHEMES:
        raise ValidationError(f"unknown ECC scheme {name!r}")
    return SCHEMES[name]


def validate(chip):
    g = chip["geometry"]
    name = chip["name"]
    if len(name) > 23:
        raise ValidationError(f"{name!r}: name exceeds 23 chars (no room for NUL in name[24])")
    if g["spare_size"] >= g["page_size"]:
        raise ValidationError(f"{name}: spare_size must be < page_size")
    ppb = g["pages_per_block"]
    if ppb <= 0 or (ppb & (ppb - 1)) != 0:
        raise ValidationError(f"{name}: pages_per_block must be a power of two")
    if g["total_blocks"] <= 0:
        raise ValidationError(f"{name}: total_blocks must be > 0")
    main = g["page_size"] - g["spare_size"]
    total_main = main * ppb * g["total_blocks"]
    if total_main > 0xFFFFFFFF:
        raise ValidationError(f"{name}: capacity overflows uint32")
    if not (_CAP_MIN <= total_main <= _CAP_MAX):
        raise ValidationError(f"{name}: main capacity {total_main} outside 512 Mb..8 Gb window")
    shift, mask, _m = expand_scheme(chip["profile"]["ecc"]["scheme"])
    if shift > 7 or mask not in (0x1, 0x3, 0x7, 0xF):
        raise ValidationError(f"{name}: bad ecc shift/mask {shift}/{mask}")
    oob = chip["profile"]["oob_layout"]
    bbm = oob["bbm"]
    if bbm["off"] + bbm["len"] > g["spare_size"]:
        raise ValidationError(f"{name}: bbm exceeds spare_size")
    # The flat struct caps OOB at 4 regions each (8 uint16 = 4 pairs); reject
    # rather than let _regions_to_pairs silently truncate a longer list.
    if len(oob["free_regions"]) > 4:
        raise ValidationError(f"{name}: oob free_regions exceeds 4")
    if len(oob["ecc_regions"]) > 4:
        raise ValidationError(f"{name}: oob ecc_regions exceeds 4")
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/test_chipdb.py -v`
Expected: PASS (all validation cases).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add tools/chipdb.py tests/test_chipdb.py
git commit -m "feat(chipdb): fail-closed validation + scheme->16-entry ecc_map"
```

---

### Task 5: `chipdb.flatten()` + `chipdb.pack()` — the byte-identical packer

Turns a resolved chip into the flat field dict, then into exactly 110 bytes with the field order and little-endian layout the C struct expects. This is the host half of the wire contract.

**Files:**
- Modify: `tools/chipdb.py`
- Test: `tests/test_chipdb.py` (add cases)

**Interfaces:**
- Consumes: `get`, `validate`, `expand_scheme` (Tasks 3–4).
- Produces: `chipdb.flatten(chip) -> dict` (flat field values); `chipdb.pack(flat) -> bytes` (length 110, little-endian, matching `active_profile_t`); `chipdb.PACK_FORMAT` (the `struct` format string); `chipdb.PROFILE_SIZE == 110`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_chipdb.py`:
```python
import struct

def test_pack_is_110_bytes():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "DS35Q1GA"))
    blob = chipdb.pack(flat)
    assert len(blob) == 110 == chipdb.PROFILE_SIZE

def test_pack_ds35_known_fields():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "DS35Q1GA"))
    assert flat["name"] == "DS35Q1GA"
    assert flat["page_size"] == 2112
    assert flat["ecc_shift"] == 4 and flat["ecc_mask"] == 0x3
    assert flat["ecc_map"][2] == chipdb.UNCOR
    assert flat["qe_addr"] == 0xB0 and flat["qe_bit"] == 0x01
    assert flat["bbm_off"] == 0 and flat["bbm_good"] == 0xFF
    # name round-trips through the fixed 24-byte field, NUL-padded
    blob = chipdb.pack(flat)
    assert blob[:24] == b"DS35Q1GA".ljust(24, b"\x00")

def test_micron_has_no_qe():
    db = chipdb.load()
    flat = chipdb.flatten(chipdb.get(db, "MT29F2G01ABAGD"))
    assert flat["qe_addr"] == 0 and flat["qe_bit"] == 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/test_chipdb.py -k "pack or micron_has_no_qe" -v`
Expected: FAIL — `AttributeError: module 'chipdb' has no attribute 'flatten'`.

- [ ] **Step 3: Write minimal implementation**

Append to `tools/chipdb.py`:
```python
PROFILE_SIZE = 110
# Matches active_profile_t (packed, little-endian). Field order is load-bearing.
PACK_FORMAT = "<24s IIII BBBBBB B BB 16B B BB BB B H BBB BB 8H 8H"

_READ_MODE = {"single": 0, "quad": 1}
_ID_METHOD = {"addr": 0, "dummy": 1}


def _regions_to_pairs(regions):
    flat = []
    for off, ln in regions:
        flat += [off, ln]
    flat += [0] * (16 - len(flat))   # pad to 8 (off,len) pairs
    return flat[:16]


def flatten(chip):
    validate(chip)
    g, fam, prof = chip["geometry"], chip["family"], chip["profile"]
    shift, mask, emap = expand_scheme(prof["ecc"]["scheme"])
    ecc_en_bit = prof.get("config_ecc_en_bit")
    if ecc_en_bit is None:
        ecc_en_bit = fam["config_ecc_en_bit"]
    qe = prof.get("qe", {})
    rid = chip.get("read_id") or fam["read_id_default"]
    oob = prof["oob_layout"]
    bbm = oob["bbm"]
    s2 = prof["ecc"].get("status2_reg") or 0
    ops = fam["opcodes"]
    return {
        "name": chip["name"],
        "page_size": g["page_size"], "spare_size": g["spare_size"],
        "pages_per_block": g["pages_per_block"], "total_blocks": g["total_blocks"],
        "op_page_read": ops["page_read"], "op_read_cache": ops["read_cache"]["x1"],
        "op_get_feat": ops["get_feature"], "op_set_feat": ops["set_feature"],
        "op_status_addr": fam["feature_addrs"]["status"], "op_cfg_addr": fam["feature_addrs"]["config"],
        "ecc_en_bit": ecc_en_bit, "ecc_shift": shift, "ecc_mask": mask, "ecc_map": list(emap),
        "status2_reg": s2,
        "id_method": _ID_METHOD[rid["method"]], "id_n_bytes": rid["id_bytes"],
        "qe_addr": qe.get("feature_addr", 0) if qe.get("has") else 0,
        "qe_bit": qe.get("bit", 0) if qe.get("has") else 0,
        "read_mode": _READ_MODE[chip["read_mode"]], "vcc_mv": chip["vcc_mv"],
        "bbm_off": bbm["off"], "bbm_len": bbm["len"], "bbm_good": bbm["good"],
        "oob_free_n": len(oob["free_regions"]), "oob_ecc_n": len(oob["ecc_regions"]),
        "oob_free": _regions_to_pairs(oob["free_regions"]),
        "oob_ecc": _regions_to_pairs(oob["ecc_regions"]),
    }


def pack(flat):
    return struct.pack(
        PACK_FORMAT,
        flat["name"].encode()[:23].ljust(24, b"\x00"),
        flat["page_size"], flat["spare_size"], flat["pages_per_block"], flat["total_blocks"],
        flat["op_page_read"], flat["op_read_cache"], flat["op_get_feat"], flat["op_set_feat"],
        flat["op_status_addr"], flat["op_cfg_addr"],
        flat["ecc_en_bit"], flat["ecc_shift"], flat["ecc_mask"], *flat["ecc_map"],
        flat["status2_reg"], flat["id_method"], flat["id_n_bytes"],
        flat["qe_addr"], flat["qe_bit"], flat["read_mode"], flat["vcc_mv"],
        flat["bbm_off"], flat["bbm_len"], flat["bbm_good"],
        flat["oob_free_n"], flat["oob_ecc_n"], *flat["oob_free"], *flat["oob_ecc"],
    )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/test_chipdb.py -v`
Expected: PASS. If `struct.error` on length, `PACK_FORMAT` and the argument order have drifted — count fields against `active_profile_t`.

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add tools/chipdb.py tests/test_chipdb.py
git commit -m "feat(chipdb): flatten + byte-identical 110-byte packer"
```

---

### Task 6: `tools/gen_profiles.py` — resident header + golden blob

One generator, two outputs from the same flattener: the C resident array (`nand_profiles_generated.h`, not yet included by any device TU) and the golden blob header the native cross-check consumes. Wired as a PlatformIO pre-hook so both stay fresh.

**Files:**
- Create: `tools/gen_profiles.py`
- Modify: `platformio.ini:12` (add `pre:tools/gen_profiles.py`)
- Test: `tools/tests/test_gen_profiles.py`

**Interfaces:**
- Consumes: `chipdb.load/get/flatten/pack` (Tasks 3–5).
- Produces: `gen_profiles.render_resident(db) -> str` (C header text; defines `PROFILES[]` and `PROFILES_COUNT`); `gen_profiles.render_golden(db, chip_name) -> str` (C header text; defines `GOLDEN_<CHIP>_BLOB[]` and `GOLDEN_<CHIP>_LEN`); `gen_profiles.generate()` writes `src/nand_profiles_generated.h` and `test/test_profile/golden_ds35.h`.

- [ ] **Step 1: Write the failing test**

`tools/tests/test_gen_profiles.py`:
```python
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import chipdb, gen_profiles

def test_resident_header_has_both_chips():
    db = chipdb.load()
    hdr = gen_profiles.render_resident(db)
    assert "PROFILES_COUNT" in hdr
    assert hdr.count("{") >= 2          # two initializers
    assert "DS35Q1GA" in hdr and "MT29F2G01ABAGD" in hdr

def test_golden_blob_is_110_bytes():
    db = chipdb.load()
    hdr = gen_profiles.render_golden(db, "DS35Q1GA")
    assert "GOLDEN_DS35Q1GA_BLOB" in hdr
    # exactly 110 comma-separated byte literals
    body = hdr[hdr.index("{") + 1: hdr.index("}")]
    assert len([b for b in body.split(",") if b.strip()]) == 110
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tools/tests/test_gen_profiles.py -v`
Expected: FAIL — `ModuleNotFoundError: gen_profiles`.

- [ ] **Step 3: Write minimal implementation**

`tools/gen_profiles.py`:
```python
"""Generate the resident active_profile_t array and the golden test blob."""
import os
import chipdb

HERE = os.path.dirname(__file__)
RESIDENT_OUT = os.path.join(HERE, "..", "src", "nand_profiles_generated.h")
GOLDEN_OUT = os.path.join(HERE, "..", "test", "test_profile", "golden_ds35.h")


def _c_initializer(flat):
    def arr(vals):
        return "{" + ",".join(str(v) for v in vals) + "}"
    return (
        '{ "%s", %d,%d,%d,%d, %d,%d,%d,%d,%d,%d, %d, %d,%d, %s, %d, %d,%d, %d,%d, %d, %d, '
        '%d,%d,%d, %d,%d, %s, %s }'
    ) % (
        flat["name"], flat["page_size"], flat["spare_size"], flat["pages_per_block"],
        flat["total_blocks"], flat["op_page_read"], flat["op_read_cache"], flat["op_get_feat"],
        flat["op_set_feat"], flat["op_status_addr"], flat["op_cfg_addr"], flat["ecc_en_bit"],
        flat["ecc_shift"], flat["ecc_mask"], arr(flat["ecc_map"]), flat["status2_reg"],
        flat["id_method"], flat["id_n_bytes"], flat["qe_addr"], flat["qe_bit"],
        flat["read_mode"], flat["vcc_mv"], flat["bbm_off"], flat["bbm_len"], flat["bbm_good"],
        flat["oob_free_n"], flat["oob_ecc_n"], arr(flat["oob_free"]), arr(flat["oob_ecc"]),
    )


def render_resident(db):
    rows = []
    for name, c in sorted(db.chips.items()):
        if c.get("resident"):
            rows.append("  " + _c_initializer(chipdb.flatten(chipdb.get(db, name))))
    return (
        "// AUTO-GENERATED by tools/gen_profiles.py. DO NOT EDIT.\n"
        "#pragma once\n#include \"nand_profile.h\"\n\n"
        "static const active_profile_t PROFILES[] = {\n" + ",\n".join(rows) + "\n};\n"
        "static const unsigned PROFILES_COUNT = sizeof(PROFILES)/sizeof(PROFILES[0]);\n"
    )


def render_golden(db, chip_name):
    blob = chipdb.pack(chipdb.flatten(chipdb.get(db, chip_name)))
    sym = "GOLDEN_" + chip_name.upper()
    body = ",".join(str(b) for b in blob)
    return (
        "// AUTO-GENERATED by tools/gen_profiles.py. DO NOT EDIT.\n"
        "#pragma once\n#include <stdint.h>\n\n"
        f"static const uint8_t {sym}_BLOB[] = {{{body}}};\n"
        f"static const unsigned {sym}_LEN = sizeof({sym}_BLOB);\n"
    )


def generate():
    db = chipdb.load()
    os.makedirs(os.path.dirname(GOLDEN_OUT), exist_ok=True)
    with open(RESIDENT_OUT, "w") as f:
        f.write(render_resident(db))
    with open(GOLDEN_OUT, "w") as f:
        f.write(render_golden(db, "DS35Q1GA"))
    print(f"[gen_profiles] wrote {RESIDENT_OUT} and {GOLDEN_OUT}")


try:
    Import("env")  # type: ignore  # noqa: F821 — injected by PlatformIO/SCons
    try:
        import yaml  # noqa: F401
    except ImportError:
        env.Execute("$PYTHONEXE -m pip install pyyaml")  # type: ignore  # noqa: F821
    import sys
    sys.path.insert(0, HERE)
    generate()
except NameError:
    pass

if __name__ == "__main__":
    generate()
```

`platformio.ini` — add the second pre-hook (keep `gen_chips.py`):
```ini
extra_scripts = pre:tools/gen_chips.py, pre:tools/gen_profiles.py
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tools/tests/test_gen_profiles.py -v && python3 tools/gen_profiles.py`
Expected: PASS, and the two generated headers appear (`src/nand_profiles_generated.h`, `test/test_profile/golden_ds35.h`).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add tools/gen_profiles.py tools/tests/test_gen_profiles.py platformio.ini \
        src/nand_profiles_generated.h test/test_profile/golden_ds35.h
git commit -m "feat(gen): resident active_profile_t header + golden blob generator"
```

---

### Task 7: Native golden-blob cross-check — the host/device seam

The highest-risk test: the Python-packed DS35 bytes are memcpy'd into the C `active_profile_t` and every field asserted. If the Python packer and the C struct disagree by even one byte, this fails. Also asserts the resident array populated.

**Files:**
- Modify: `test/test_profile/test_profile.cpp` (add cases + includes)

**Interfaces:**
- Consumes: `active_profile_t` (Task 1), `GOLDEN_DS35Q1GA_BLOB`/`_LEN` and `PROFILES[]`/`PROFILES_COUNT` from the generated headers (Task 6).

- [ ] **Step 1: Write the failing test**

Add includes at the top of `test/test_profile/test_profile.cpp`:
```c
#include <string.h>
#include "golden_ds35.h"
#include "nand_profiles_generated.h"
```

Add cases (and their `RUN_TEST` lines in `main`):
```c
void test_golden_blob_unpacks_to_ds35(void) {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(active_profile_t), GOLDEN_DS35Q1GA_LEN);
  active_profile_t p;
  memcpy(&p, GOLDEN_DS35Q1GA_BLOB, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p.name);
  TEST_ASSERT_EQUAL_UINT32(2112u, p.page_size);
  TEST_ASSERT_EQUAL_UINT32(64u, p.spare_size);
  TEST_ASSERT_EQUAL_UINT32(64u, p.pages_per_block);
  TEST_ASSERT_EQUAL_UINT32(1024u, p.total_blocks);
  TEST_ASSERT_EQUAL_UINT8(4, p.ecc_shift);
  TEST_ASSERT_EQUAL_UINT8(0x3, p.ecc_mask);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, p.ecc_map[2]);
  TEST_ASSERT_EQUAL_UINT8(0xB0, p.qe_addr);
  TEST_ASSERT_EQUAL_UINT8(0x01, p.qe_bit);
  TEST_ASSERT_EQUAL_UINT8(0x00, p.bbm_off);
  TEST_ASSERT_EQUAL_UINT8(0xFF, p.bbm_good);
  TEST_ASSERT_EQUAL_UINT16(3300u, p.vcc_mv);
  // decoder runs against the unpacked profile
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x20));
}

void test_resident_array_has_two_chips(void) {
  TEST_ASSERT_EQUAL_UINT32(2u, PROFILES_COUNT);
}
```

- [ ] **Step 2: Run test to verify it fails**

First ensure the generated headers exist: `python3 tools/gen_profiles.py`
Run: `pio test -e native -f test_profile`
Expected: initially FAIL if the generated headers are stale/missing, or PASS once regenerated — if any field mismatches, the specific `TEST_ASSERT` names the drifted field (that is the seam this task protects).

- [ ] **Step 3: Write minimal implementation**

No new production code — this task *is* the cross-check. If a field fails, fix the disagreement at its source: the `PACK_FORMAT`/argument order in `chipdb.pack` (Task 5) or the struct field order in `nand_profile.h` (Task 1). Regenerate (`python3 tools/gen_profiles.py`) and re-run.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tools/gen_profiles.py && pio test -e native -f test_profile`
Expected: PASS (all struct, decoder, golden, and resident cases).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add test/test_profile/test_profile.cpp
git commit -m "test(profile): golden-blob host/device cross-check + resident count"
```

---

### Task 8: `chipdb.resolve()` — ID-collision disambiguation ladder

Shared JEDEC IDs are common. `resolve()` returns the single chip when `(mfr, dev, dev2?)` is unambiguous, or raises with the candidate list — the host-side half of the spec's § 5 ladder. Stage 1 covers the `dev2` and cached-choice rungs; ONFI is a stub.

**Files:**
- Modify: `tools/chipdb.py`
- Test: `tests/test_chipdb.py` (add cases)

**Interfaces:**
- Consumes: `load` (Task 3).
- Produces: `chipdb.candidates(db, mfr, dev) -> list[chip]`; `chipdb.resolve(db, mfr, dev, dev2=None, cached_name=None) -> chip` raising `AmbiguousID` (with `.candidates`) when it cannot narrow to one.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_chipdb.py` (add `from pathlib import Path` to the imports at the top of the file):
```python
# Resolve the real db/ relative to this test file, not the CWD pytest ran from.
DB_DIR = Path(__file__).resolve().parent.parent / "db"

def _twin_db(tmp_path):
    for d in ("families", "profiles", "chips"):
        (tmp_path / d).mkdir()
    (tmp_path / "families" / "spi-nand.yml").write_text(
        (DB_DIR / "families" / "spi-nand.yml").read_text())
    (tmp_path / "profiles" / "dosilicon.yml").write_text(
        (DB_DIR / "profiles" / "dosilicon.yml").read_text())
    base = (DB_DIR / "chips" / "DS35Q1GA.yml").read_text()
    (tmp_path / "chips" / "A.yml").write_text(
        base.replace("name: DS35Q1GA", "name: TWIN_A").replace("dev2: null", "dev2: 0x01"))
    (tmp_path / "chips" / "B.yml").write_text(
        base.replace("name: DS35Q1GA", "name: TWIN_B").replace("dev2: null", "dev2: 0x02"))
    return chipdb.load(root=str(tmp_path))

def test_resolve_unique_id():
    db = chipdb.load()
    assert chipdb.resolve(db, 0xE5, 0x71)["name"] == "DS35Q1GA"

def test_resolve_by_dev2(tmp_path):
    db = _twin_db(tmp_path)
    assert chipdb.resolve(db, 0xE5, 0x71, dev2=0x02)["name"] == "TWIN_B"

def test_resolve_ambiguous_raises_with_candidates(tmp_path):
    db = _twin_db(tmp_path)
    with pytest.raises(chipdb.AmbiguousID) as e:
        chipdb.resolve(db, 0xE5, 0x71)
    assert {c["name"] for c in e.value.candidates} == {"TWIN_A", "TWIN_B"}

def test_resolve_ambiguous_honors_cached_choice(tmp_path):
    db = _twin_db(tmp_path)
    assert chipdb.resolve(db, 0xE5, 0x71, cached_name="TWIN_A")["name"] == "TWIN_A"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/test_chipdb.py -k resolve -v`
Expected: FAIL — `AttributeError: module 'chipdb' has no attribute 'resolve'`.

- [ ] **Step 3: Write minimal implementation**

Append to `tools/chipdb.py`:
```python
def candidates(db, mfr, dev):
    return [c for c in db.chips.values()
            if c["id"]["mfr"] == mfr and c["id"]["dev"] == dev]


def resolve(db, mfr, dev, dev2=None, cached_name=None):
    cands = candidates(db, mfr, dev)
    if not cands:
        raise RefError(f"no chip for id {mfr:#04x} {dev:#04x}")
    if len(cands) == 1:
        return cands[0]
    if dev2 is not None:                       # rung 1: extra ID byte
        narrowed = [c for c in cands if c["id"].get("dev2") == dev2]
        if len(narrowed) == 1:
            return narrowed[0]
        if narrowed:
            cands = narrowed
    if cached_name is not None:                # rung 3: cached/user choice
        for c in cands:
            if c["name"] == cached_name:
                return c
    # rung 2 (ONFI) is a stub — fail closed with the candidate list
    raise AmbiguousID(cands)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/test_chipdb.py -v`
Expected: PASS (full suite).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add tools/chipdb.py tests/test_chipdb.py
git commit -m "feat(chipdb): ID-collision disambiguation ladder (dev2 + cached)"
```

---

## Self-Review

**1. Spec coverage (Stage 1 scope only):**

| Spec item (§) | Task |
|---|---|
| Flat `active_profile_t` (§4.1) | 1 |
| `ecc_map[16]` Wall-1 fix | 1, 2 |
| Pure-data decoder (§4.2) | 2 |
| Named-scheme expansion incl. `xtx_g0xa` (§3.2) | 2, 4 |
| Three-layer `db/` (§3, §8) | 3 |
| Ref-integrity / fail-closed validation (§6 tier 2/3) | 4 |
| `name` length guard, capacity host-side (§6 smaller corrections) | 4 |
| `read_id` at chip layer w/ family default (§3.1/§3.3, Wall 2) | 3, 5 |
| OOB region lists (§3.2, Wall 3) | 3, 5 |
| Byte-identical flatten+pack (§8) | 5 |
| `gen_profiles.py` resident header (§4.3) | 6 |
| Golden-blob cross-check + `static_assert` (§9) | 1, 6, 7 |
| ECC decoder table test (§9) | 2 |
| Verification-pipeline unit rejections (§9) | 4 |
| Disambiguation ladder (§5, §9) | 8 |

Deferred to Stages 2–3 (correctly out of this plan): device read-path rewiring (`nand_driver.*`, `main.cpp`, delete `nand_ecc.h`), `nand_read_id()` widening, push protocol / two-phase arm, profile-aware `ecc_stripper.py`, real-`target/*.bin` characterization (local), removal of the legacy `chips.yml`/`gen_chips.py` path, **real per-vendor OOB `free/ecc_regions` layouts** (empty in Stage 1), and a **`CONTRIBUTING.md` update** documenting `db/` authoring (it still describes `chips.yml`; skipping it in Stage 2 would leave two chip-add workflows, neither documented for `db/`). STATUS2 exact-count stays stubbed (`status2_reg` carried, unused).

**2. Placeholder scan:** No "TBD/handle errors/similar to Task N" — every code step is literal. The Task 1 decoder stub is intentional (replaced with a real body and its own failing test in Task 2), not a placeholder.

**3. Type consistency:** `active_profile_t` field names/order identical across `nand_profile.h` (Task 1), `PACK_FORMAT` (Task 5), `_c_initializer` (Task 6), and the golden asserts (Task 7). `flatten()` keys (Task 5) are the exact keys `pack()` and `_c_initializer()` read. `SCHEMES`/`expand_scheme` (Task 4) consumed unchanged by `flatten` (Task 5). `AmbiguousID.candidates` (Task 3 definition) used in Task 8 tests.

**Known soft spots to verify at implementation time (named, not hidden):**
- `PACK_FORMAT` (110) vs the C `sizeof` (110): the golden test (Task 7) is the guard; if Task 1's `_Static_assert` and Task 5's `struct.error` both pass but Task 7 mismatches, a *field order* (not size) drift is the cause.
- OOB `free/ecc_regions` are intentionally **empty** in both Stage-1 profiles (they pack as zeros). The design-plan review caught that the earlier representative offsets were wrong — Dosilicon is 6-free/8-ECC at stride 14 (not 16), and the Micron list was a placeholder. Keeping them empty means the golden blob stays a correct regression target instead of pinning unverified bytes; the real per-vendor layouts are authored and checked against the mainline `dosilicon.c`/`micron.c` ooblayouts in Stage 2, where the stripper first consumes them.
- Micron `micron3` map treats reserved field values 4/6/7 as `UNCOR` (conservative). Correct for a read-integrity tool; revisit if a Micron refresh tier needs finer reporting.
