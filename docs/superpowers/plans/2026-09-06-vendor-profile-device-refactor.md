# Vendor/Profile Architecture — Stage 2: Device Read-Path Refactor (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the device's detection and read path onto the Stage-1 `active_profile_t`/`PROFILES[]` system — delivering the DS35 ECC-decode and Quad-Enable fixes on real silicon — while keeping the legacy `chips.yml`→`CHIPS[]` path as a dormant compiled fallback.

**Architecture:** The device reads the JEDEC ID, looks it up in a new ID-indexed resident profile table (`nand_profile_lookup`), and drives geometry, ECC-severity decoding, and the QE write from the resolved `active_profile_t`. If no profile matches, it falls back to the untouched legacy `nand_chip_t` path; if neither matches, the existing manual-geometry raw-dump path is preserved. Host side: the `ecc_stripper` becomes profile-aware (re-resolving the profile from the dump metadata), and the Stage-1 review's carry-over hardening lands in `chipdb.py`.

**Tech Stack:** C11 + PlatformIO/Arduino (ESP32 firmware), PlatformIO Unity (native pure-logic tests), Python 3 + PyYAML + pytest (host tools).

**Spec:** [docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md](../specs/2026-08-23-vendor-profile-architecture-design.md) — this plan implements **Stage 2** of § 10 ("device resident-table + read-path refactor"), read against §§ 4.1, 4.2, 4.3, 7. Stage 1 (host foundation) is merged.

## Global Constraints

- **Commit permission is per-action (STRICT).** The user's global `CLAUDE.md` requires explicit permission before EVERY `git commit` / `git push`. This OVERRIDES the `git commit` step in each task below: do the work, run the tests, then **stop and ask** before committing. Approval for one commit never carries to the next. There is no standing auto-commit grant this run.
- **Keep the legacy path as a compiled fallback — do NOT delete it.** `chips.yml`, `tools/gen_chips.py`, `src/nand_chips_generated.h`, `nand_chip_t`, `nand_chip_lookup`, and `src/nand_ecc.h` all stay. The profile path is PRIMARY; the legacy `nand_chip_t` path is the fallback for a chip in `chips.yml` but not `db/`; the manual-geometry path stays for unknown chips. (Both current chips are in `db/`, so the profile path is always exercised and the legacy branch is dormant safety.)
- **The 110-byte wire contract is frozen.** `active_profile_t` must stay exactly 110 bytes (`static_assert`-pinned) and must NOT gain `mfr`/`dev` fields. The ID→profile mapping lives in a SEPARATE parallel `PROFILE_IDS[]` table in the generated header.
- **Silicon verification is DEFERRED.** No hardware bench this stage. Device changes are verified by (a) native Unity tests of the pure helpers and (b) a clean `pio run` firmware build for BOTH `esp32dev` and `esp32-s3-devkitc-1`. On-silicon DS35 behavior (ECC-on uncorrectable reporting, quad self-test) is flagged pending — a bench item, like the existing T14.
- **`main.cpp` is not host-testable.** It is excluded from the native `build_src_filter` (Arduino-dependent). Device tasks that touch it are gated by the firmware build compiling+linking and by the native tests of the pure helpers they call — not by a red-green host test. Push all decidable logic into pure, host-tested helpers.
- **Additive, not breaking → stays under CHANGELOG `[Unreleased]`.** Because the fallback is kept, this is a minor/additive change (targets v3.2.0 at release); do not bump the hardcoded version strings or cut a release here.
- **Tests run via the `lab/venv` interpreter:** `lab/venv/bin/python -m pytest ...`. Native: `pio test -e native`. The full pre-Stage-2 baseline is 53 pytest + 21 native passing.

---

### Task 1: `PROFILE_IDS[]` resident ID table + `nand_profile_lookup()`

The piece that makes resident detection possible: a JEDEC-ID → `active_profile_t` lookup. Because `active_profile_t` carries no ID, the generator emits a parallel `PROFILE_IDS[]` (same order as `PROFILES[]`), and a new pure function searches it. This is the fully host-testable foundation the device wiring (Tasks 2–3) stands on.

**Files:**
- Modify: `tools/gen_profiles.py` (emit `PROFILE_IDS[]` alongside `PROFILES[]`)
- Create: `src/nand_profile_lookup.cpp` (mirrors `nand_chips.cpp`)
- Modify: `src/nand_profile.h` (declare `nand_profile_lookup`)
- Modify: `platformio.ini:31` (add `+<nand_profile_lookup.cpp>` to the native `build_src_filter`)
- Modify: `test/test_profile/test_profile.cpp` (lookup tests)
- Modify: `tools/tests/test_gen_profiles.py` (assert the ID table is emitted)
- Regenerated: `src/nand_profiles_generated.h`

**Interfaces:**
- Consumes: `active_profile_t` (Stage 1, `nand_profile.h`); `chipdb.get`/`chipdb.flatten` (Stage 1).
- Produces: `const active_profile_t *nand_profile_lookup(uint8_t mfr, uint8_t dev);` (returns `NULL`/`0` if no resident profile matches). Generated header now also defines `static const struct { uint8_t mfr, dev, dev2; } PROFILE_IDS[]` parallel to `PROFILES[]`.

- [ ] **Step 1: Write the failing test (Python — the generator emits the ID table)**

Add to `tools/tests/test_gen_profiles.py`:
```python
def test_resident_header_has_id_table():
    db = chipdb.load()
    hdr = gen_profiles.render_resident(db)
    assert "PROFILE_IDS" in hdr
    # DS35 (0xE5,0x71) and MT29F (0x2C,0x24) ids present as byte literals
    assert "0xE5" in hdr and "0x71" in hdr
    assert "0x2C" in hdr and "0x24" in hdr
```

- [ ] **Step 2: Run it to verify it fails**

Run: `lab/venv/bin/python -m pytest tools/tests/test_gen_profiles.py::test_resident_header_has_id_table -v`
Expected: FAIL — no `PROFILE_IDS` in the header yet.

- [ ] **Step 3: Implement the generator change**

In `tools/gen_profiles.py`, replace `render_resident` with a version that emits both arrays in the same iteration order:
```python
def render_resident(db):
    prof_rows, id_rows = [], []
    for name, c in sorted(db.chips.items()):
        if c.get("resident"):
            chip = chipdb.get(db, name)
            prof_rows.append("  " + _c_initializer(chipdb.flatten(chip)))
            idd = chip["id"]
            id_rows.append("  { 0x%02X, 0x%02X, 0x%02X }"
                           % (idd["mfr"], idd["dev"], idd.get("dev2") or 0))
    return (
        "// AUTO-GENERATED by tools/gen_profiles.py. DO NOT EDIT.\n"
        "#pragma once\n#include \"nand_profile.h\"\n\n"
        "static const active_profile_t PROFILES[] = {\n" + ",\n".join(prof_rows) + "\n};\n"
        "static const struct { uint8_t mfr, dev, dev2; } PROFILE_IDS[] = {\n"
        + ",\n".join(id_rows) + "\n};\n"
        "static const unsigned PROFILES_COUNT = sizeof(PROFILES)/sizeof(PROFILES[0]);\n"
    )
```

- [ ] **Step 4: Regenerate and verify the Python test passes**

Run: `lab/venv/bin/python tools/gen_profiles.py && lab/venv/bin/python -m pytest tools/tests/test_gen_profiles.py -v`
Expected: PASS. Confirm `src/nand_profiles_generated.h` now contains `PROFILE_IDS[]` with both chips' ids in the same order as `PROFILES[]`.

- [ ] **Step 5: Write the failing native test (the C lookup)**

Add to `test/test_profile/test_profile.cpp` (and add the `RUN_TEST` lines in `main`):
```c
void test_profile_lookup_finds_ds35(void) {
  const active_profile_t *p = nand_profile_lookup(0xE5, 0x71);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p->name);
  TEST_ASSERT_EQUAL_UINT32(2112u, p->page_size);
  TEST_ASSERT_EQUAL_UINT8(0xB0, p->qe_addr);   // the profile that carries the DS35 QE fix
}

void test_profile_lookup_finds_micron(void) {
  const active_profile_t *p = nand_profile_lookup(0x2C, 0x24);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("MT29F2G01ABAGD", p->name);
}

void test_profile_lookup_unknown_is_null(void) {
  TEST_ASSERT_NULL(nand_profile_lookup(0x00, 0x00));
}
```

- [ ] **Step 6: Run the native test to verify it fails**

Run: `pio test -e native -f test_profile`
Expected: FAIL to compile/link — `nand_profile_lookup` undefined.

- [ ] **Step 7: Implement the lookup**

Add the declaration to `src/nand_profile.h` (just before `#endif`):
```c
// Resident profile lookup by JEDEC id (searches the generated PROFILE_IDS table).
// Returns NULL if no resident profile matches. dev2 disambiguation is Stage 3.
const active_profile_t *nand_profile_lookup(uint8_t mfr, uint8_t dev);
```

Create `src/nand_profile_lookup.cpp` (mirrors `nand_chips.cpp`):
```c
#include "nand_profile.h"
#include "nand_profiles_generated.h"

const active_profile_t *nand_profile_lookup(uint8_t mfr, uint8_t dev) {
  for (unsigned i = 0; i < PROFILES_COUNT; i++) {
    if (PROFILE_IDS[i].mfr == mfr && PROFILE_IDS[i].dev == dev) {
      return &PROFILES[i];
    }
  }
  return 0;
}
```

Add it to the native `build_src_filter` in `platformio.ini` (append, preserving existing entries):
```ini
build_src_filter = -<*> +<nand_chips.cpp> +<dump_header.cpp> +<config_store.cpp> +<sys_info.cpp> +<nand_profile.cpp> +<nand_profile_lookup.cpp>
```

- [ ] **Step 8: Run native + full suites to verify green**

Run: `pio test -e native -f test_profile` → all pass (10 tests: the 7 from Stage 1 + 3 new).
Run: `lab/venv/bin/python -m pytest -q` → no regression (was 53).
Expected: both PASS.

- [ ] **Step 9: Commit** *(ask first — see Global Constraints)*

```bash
git add tools/gen_profiles.py src/nand_profile.h src/nand_profile_lookup.cpp \
        platformio.ini test/test_profile/test_profile.cpp tools/tests/test_gen_profiles.py \
        src/nand_profiles_generated.h
git commit -m "feat(profile): PROFILE_IDS table + nand_profile_lookup by JEDEC id"
```

---

### Task 2: `main.cpp` — profile-first detection + geometry pre-fill

Detection tries the profile table first, falls back to the legacy chip table, then to manual geometry. This is a `main.cpp` change (not host-testable): the gate is a clean firmware build plus the Task-1 native tests of `nand_profile_lookup`. Behavior on silicon is bench-deferred.

**Files:**
- Modify: `src/main.cpp` (add `#include "nand_profile.h"`, `g_profile` global, detection branch, `show_menu` display)

**Interfaces:**
- Consumes: `nand_profile_lookup` (Task 1); existing `nand_chip_lookup`, `log2_int`, the `cfg_*` globals.
- Produces: file-scope `static const active_profile_t *g_profile` set during `setup()`, read by Tasks 3.

- [ ] **Step 1: Add the include and global**

At the top of `src/main.cpp`, add to the includes:
```c
#include "nand_profile.h"
```
With the other detected-chip globals (near `static const nand_chip_t *g_chip = NULL;`):
```c
static const active_profile_t *g_profile = NULL;   // primary: resident profile match
```

- [ ] **Step 2: Rewrite the detection block in `setup()`**

Replace the current detection block (the `g_chip = nand_chip_lookup(...)` block and its geometry pre-fill) with profile-first resolution:
```c
  g_chip_id = nand_read_id();
  uint8_t mfr = g_chip_id >> 8, dev = g_chip_id & 0xFF;
  g_profile = nand_profile_lookup(mfr, dev);
  if (g_profile) {
    Serial.printf("[*] Detected %s (0x%02X 0x%02X) [profile]\n", g_profile->name, mfr, dev);
    cfg_page_size       = (int)g_profile->page_size;
    cfg_spare_size      = (int)g_profile->spare_size;
    cfg_pages_per_block = (int)g_profile->pages_per_block;
    cfg_total_blocks    = (int)g_profile->total_blocks;
    cfg_page_addr_bits  = log2_int((int)g_profile->pages_per_block);
    cfg_ecc_on          = false;   // global policy OFF/raw; profiles carry no per-chip default
  } else if ((g_chip = nand_chip_lookup(mfr, dev)) != NULL) {
    Serial.printf("[*] Detected %s (0x%02X 0x%02X) [legacy]\n", g_chip->name, mfr, dev);
    cfg_page_size       = g_chip->page_size;
    cfg_spare_size      = g_chip->spare_size;
    cfg_pages_per_block = g_chip->pages_per_block;
    cfg_total_blocks    = g_chip->total_blocks;
    cfg_page_addr_bits  = g_chip->page_addr_bits;
    cfg_bad_mark        = g_chip->bad_block_mark;
    cfg_ecc_on          = g_chip->ecc_default_on;
  } else {
    Serial.printf("[!] Unknown chip 0x%02X 0x%02X — using manual defaults\n", mfr, dev);
  }
```

- [ ] **Step 3: Update `show_menu()` to display the profile name**

Replace the detected-chip line in `show_menu()`:
```c
  if (g_profile)   Serial.printf("  Detected: %s (0x%02X 0x%02X)\n",
                                 g_profile->name, g_chip_id >> 8, g_chip_id & 0xFF);
  else if (g_chip) Serial.printf("  Detected: %s (0x%02X 0x%02X)\n",
                                 g_chip->name, g_chip_id >> 8, g_chip_id & 0xFF);
  else             Serial.printf("  Detected: UNKNOWN (0x%02X 0x%02X) — manual geometry\n",
                                 g_chip_id >> 8, g_chip_id & 0xFF);
```

- [ ] **Step 4: Build both firmware targets to verify it compiles and links**

Run: `pio run -e esp32dev && pio run -e esp32-s3-devkitc-1`
Expected: both SUCCESS. `main.cpp` compiles with the new profile-first block; no unresolved symbols (`nand_profile_lookup` links from `nand_profile_lookup.cpp`).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add src/main.cpp
git commit -m "feat(device): profile-first chip detection with legacy fallback"
```

---

### Task 3: `main.cpp` — QE write + ECC-severity accounting via the profile (the DS35 fixes)

The payoff: when a profile matched, drive the Quad-Enable write and the ECC-status decode from `active_profile_t` — fixing DS35's dead QE bit (`B0[0]`) and its wrong ECC scheme (`generic2` `[5:4]`, not Micron `[6:4]`). The legacy branch is preserved exactly. Gated by the firmware build; runtime correctness is bench-deferred but the decode logic itself is proven by the Task-1/Stage-1 native `nand_profile_severity` tests.

**Files:**
- Modify: `src/main.cpp` (`apply_runtime_settings` QE block; `cmd_dump` ECC accounting block)

**Interfaces:**
- Consumes: `g_profile` (Task 2); `nand_profile_severity`, `NAND_SEV_*` (`nand_profile.h`, Stage 1); `nand_get_feature`/`nand_set_feature`/`NAND_FEATURE_STATUS`/`NAND_FEATURE_CONFIG` (`nand_driver.h`); legacy `nand_get_ecc_status`/`nand_ecc_uncorrectable`/`nand_ecc_refresh_recommended` (`nand_ecc.h`) for the fallback.

- [ ] **Step 1: Rewrite the QE block in `apply_runtime_settings()`**

Replace the `if (g_chip && g_chip->has_qe_bit) ...` QE write (inside the `if (cfg_read_mode == NAND_READ_QUAD)` block) with:
```c
    if (g_profile) {
      if (g_profile->qe_addr)   // 0 = no QE bit (e.g. Micron); non-zero for DS35/Winbond/GigaDevice
        nand_set_feature(g_profile->qe_addr,
                         nand_get_feature(g_profile->qe_addr) | g_profile->qe_bit);
    } else if (g_chip && g_chip->has_qe_bit) {
      nand_set_feature(g_chip->qe_feature_addr,
                       nand_get_feature(g_chip->qe_feature_addr) | g_chip->qe_bit);
    }
```

- [ ] **Step 2: Rewrite the ECC-accounting block in `cmd_dump()`**

Replace the `if (cfg_ecc_on) { uint8_t eccs = nand_get_ecc_status(); ... }` block with a profile-first branch. **Note:** the profile decoder takes the RAW `C0h` status byte (`nand_get_feature`), not `nand_get_ecc_status()` (which pre-masks to 3 bits and would double-shift):
```c
      if (cfg_ecc_on) {
        if (g_profile) {
          uint8_t raw = nand_get_feature(NAND_FEATURE_STATUS);
          nand_severity_t sev = nand_profile_severity(g_profile, raw);
          if (sev == NAND_SEV_UNCORRECTABLE) {
            eccUncorrectable++;
            if (eccUncorrectable <= 20)
              Serial.printf("[ECC] UNCORRECTABLE page %u (block %d, page %d)\n",
                            pagesDone, block, page);
          } else if (sev == NAND_SEV_CORRECTED_REFRESH) {
            eccRefresh++;
          }
        } else {
          uint8_t eccs = nand_get_ecc_status();
          if (nand_ecc_uncorrectable(eccs)) {
            eccUncorrectable++;
            if (eccUncorrectable <= 20)
              Serial.printf("[ECC] UNCORRECTABLE page %u (block %d, page %d)\n",
                            pagesDone, block, page);
          } else if (nand_ecc_refresh_recommended(eccs)) {
            eccRefresh++;
          }
        }
      }
```

- [ ] **Step 3: Build both firmware targets**

Run: `pio run -e esp32dev && pio run -e esp32-s3-devkitc-1`
Expected: both SUCCESS. Confirm `nand_profile_severity` and the `NAND_SEV_*` enum resolve, and `nand_ecc.h` is still included (the fallback uses it).

- [ ] **Step 4: Sanity-check the full native + pytest suites (unchanged, but confirm no accidental breakage)**

Run: `pio test -e native` → all pass. `lab/venv/bin/python -m pytest -q` → no regression.
Expected: PASS (these tasks did not touch pure modules, so counts are unchanged from Task 1).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add src/main.cpp
git commit -m "fix(device): DS35 QE + ECC decode via profile (data-driven ECCS/QE)"
```

---

### Task 4: `chipdb.py` hardening — the Stage-1 review carry-overs

Close the five Minors the Stage-1 whole-branch review parked: OOB region-bounds validation (the spec §6 gap), byte-accurate name length, `get()` deep-copy, negative tests for the defensive branches, and a `SCHEMA_VER` constant co-located with the wire struct. All host-side, fully TDD-able.

**Files:**
- Modify: `tools/chipdb.py`
- Modify: `tests/test_chipdb.py`

**Interfaces:**
- Consumes: existing `chipdb.load/get/validate/expand_scheme/PROFILE_SIZE` (Stage 1).
- Produces: `chipdb.SCHEMA_VER == 1`; `validate()` additionally rejects out-of-spare OOB regions and byte-over-length names; `get()` returns deep-copied `family`/`profile`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_chipdb.py`:
```python
def test_schema_ver_is_one():
    assert chipdb.SCHEMA_VER == 1

def test_validate_rejects_oob_region_past_spare():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    sp = c["geometry"]["spare_size"]
    c["profile"]["oob_layout"]["free_regions"] = [[sp - 1, 4]]   # off+len > spare
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_validate_rejects_multibyte_name_over_23_bytes():
    db = chipdb.load()
    c = chipdb.get(db, "DS35Q1GA")
    c["name"] = "é" * 22          # 22 chars but 44 UTF-8 bytes
    with pytest.raises(chipdb.ValidationError):
        chipdb.validate(c)

def test_get_returns_independent_profile_copy():
    db = chipdb.load()
    a = chipdb.get(db, "DS35Q1GA")
    a["profile"]["ecc"]["scheme"] = "MUTATED"
    b = chipdb.get(db, "DS35Q1GA")
    assert b["profile"]["ecc"]["scheme"] == "generic2"   # not leaked via shared ref

def test_expand_scheme_unknown_raises():
    with pytest.raises(chipdb.ValidationError):
        chipdb.expand_scheme("no_such_scheme")
```

- [ ] **Step 2: Run to verify they fail**

Run: `lab/venv/bin/python -m pytest tests/test_chipdb.py -k "schema_ver or oob_region_past or multibyte or independent_profile or unknown_raises" -v`
Expected: FAIL — `SCHEMA_VER` missing; region-past-spare currently accepted; multibyte name accepted; mutation leaks; (unknown-scheme already raises — that one may pass, which is fine, it documents the defensive branch).

- [ ] **Step 3: Implement the hardening**

In `tools/chipdb.py`:

Add the constant near `PROFILE_SIZE = 110`:
```python
SCHEMA_VER = 1   # bumps when the flat active_profile_t layout changes (Stage 3 push envelope stamps it)
```

In `validate()`, replace the name check to count bytes and add the region-bounds check. Change the name guard from `len(name) > 23` to:
```python
    if len(name.encode()) > 23:
        raise ValidationError(f"{name!r}: name exceeds 23 bytes (no room for NUL in name[24])")
```
And after the existing `oob free_regions/ecc_regions > 4` count checks, add:
```python
    for label, regs in (("free", oob["free_regions"]), ("ecc", oob["ecc_regions"])):
        for off, ln in regs:
            if off + ln > g["spare_size"]:
                raise ValidationError(f"{name}: oob {label} region [{off},{ln}] exceeds spare_size")
```

In `get()`, deep-copy the spliced sub-dicts so a caller mutating them cannot corrupt the shared DB. Add `import copy` at the top, and change the two assignments:
```python
    c["family"] = copy.deepcopy(db.families[c["family"]])
    c["profile"] = copy.deepcopy(db.profiles[c["profile"]])
```

- [ ] **Step 4: Run to verify green**

Run: `lab/venv/bin/python -m pytest tests/test_chipdb.py -v` → all pass.
Run: `lab/venv/bin/python -m pytest -q` → no regression.
Expected: PASS.

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add tools/chipdb.py tests/test_chipdb.py
git commit -m "harden(chipdb): oob-bounds + byte-length + get() deepcopy + SCHEMA_VER"
```

---

### Task 5: `ecc_stripper.py` — profile-aware bad-block detection

Generalize the last hardcoded host assumption. Today the stripper flags a bad block with `page[main] != 0xFF` on each block's first page — Micron-shaped constants. Make it re-resolve the profile from the dump's `meta.json` (`mfr_id`/`dev_id` are already there, no `dump.py` change) and use the profile's `bbm {offset, len, good, pages}`. For the two current chips the `bbm` is `{offset:0, len:2, good:0xFF, pages:[first]}`, which reproduces today's behavior byte-for-byte — so the existing `target/*.bin` output is a characterization anchor.

**Files:**
- Modify: `ecc_stripper.py`
- Modify: `tests/test_ecc_stripper.py`

**Interfaces:**
- Consumes: `chipdb.load/resolve` (Stage 1); the `meta.json` `geometry` block (has `mfr_id`/`dev_id`).
- Produces: `strip()` accepts an optional `bbm` dict `{offset, len, good, pages}`; a helper `resolve_bbm(meta) -> dict|None` that re-resolves via `chipdb`. When no profile resolves, `strip()` uses the current default (`offset=0, len=1, good=0xFF, pages=["first"]`), preserving existing behavior.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_ecc_stripper.py` (import `chipdb` via the conftest path if needed):
```python
def test_resolve_bbm_from_meta_ds35():
    import ecc_stripper
    meta = {"geometry": {"mfr_id": 0xE5, "dev_id": 0x71,
                         "page_size": 2112, "spare_size": 64, "pages_per_block": 64}}
    bbm = ecc_stripper.resolve_bbm(meta)
    assert bbm is not None
    assert bbm["offset"] == 0 and bbm["good"] == 0xFF

def test_resolve_bbm_unknown_chip_is_none():
    import ecc_stripper
    meta = {"geometry": {"mfr_id": 0x00, "dev_id": 0x00}}
    assert ecc_stripper.resolve_bbm(meta) is None

def test_strip_flags_bad_block_at_profile_offset(tmp_path):
    import ecc_stripper
    # 4 bytes main + 4 bytes spare, 2 pages/block, 2 blocks. bbm offset 1 in spare.
    ps, ss, ppb = 8, 4, 2
    good, offbyte = 0xFF, 1
    page_ok  = b"\xAA\xAA\xAA\xAA" + b"\xFF\xFF\xFF\xFF"
    page_bad = b"\xBB\xBB\xBB\xBB" + b"\xFF\x00\xFF\xFF"  # spare[offset=1] != good
    raw = tmp_path / "raw.bin"
    raw.write_bytes(page_ok + page_ok + page_bad + page_ok)     # block1 page0 marks bad
    out = tmp_path / "clean.bin"
    bad = ecc_stripper.strip(str(raw), str(out), ps, ss, ppb,
                             bbm={"offset": offbyte, "len": 1, "good": good, "pages": ["first"]})
    assert bad == [1]                       # block 1 flagged bad
    assert out.read_bytes()[8:12] == b"\xFF\xFF\xFF\xFF"   # block1 main replaced with 0xFF pad
```

- [ ] **Step 2: Run to verify they fail**

Run: `lab/venv/bin/python -m pytest tests/test_ecc_stripper.py -k "resolve_bbm or profile_offset" -v`
Expected: FAIL — `resolve_bbm` missing and `strip()` has no `bbm` parameter.

- [ ] **Step 3: Implement profile-aware stripping**

In `ecc_stripper.py`, add profile resolution and thread `bbm` through `strip()`. Add near the top:
```python
import os, sys
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
```
Change `strip()` to accept and use `bbm` (default reproduces current behavior):
```python
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
```
In `main()`, after loading geometry from the meta, resolve the bbm and pass it:
```python
    bbm = None
    if a.meta:
        with open(a.meta) as f:
            meta = json.load(f)
        bbm = resolve_bbm(meta)
    bad = strip(a.input, a.output, ps, ss, ppb, bbm=bbm)
```
(Keep the existing geometry-loading logic; only add the `bbm` resolution and pass-through. `resolve_bbm` returning `None` means `strip()` uses its behavior-preserving default.)

- [ ] **Step 4: Run to verify green + no regression on existing stripper tests**

Run: `lab/venv/bin/python -m pytest tests/test_ecc_stripper.py -v` → all pass (existing + new).
Run: `lab/venv/bin/python -m pytest -q` → no regression.
Expected: PASS. The existing stripper tests still pass because the default `bbm` reproduces the old constants (`offset 0, len 1, good 0xFF, first page`).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add ecc_stripper.py tests/test_ecc_stripper.py
git commit -m "feat(stripper): profile-aware bad-block detection (bbm from db via meta)"
```

---

### Task 6: Documentation — device is now profile-first

Update the docs to reflect that the device path is profile-first (with the legacy fallback), and that `db/` is now the live device path — closing the Stage-1 deferral of the `CONTRIBUTING.md` `db/` authoring guide.

**Files:**
- Modify: `CHANGELOG.md` (extend the `[Unreleased]` block)
- Modify: `CONTRIBUTING.md` (db/ authoring is now the device path; chips.yml is the fallback)
- Modify: `docs/DEVELOPER_GUIDE.md` (the "Vendor/profile architecture" section: device is now wired; boot flow updated)

**Interfaces:** Documentation only — no code interfaces.

- [ ] **Step 1: Update `CHANGELOG.md`**

Extend the existing `## [Unreleased]` `### Added`/add a `### Changed` with a Stage-2 entry:
```markdown
### Changed
- **Device read path is now profile-first (Stage 2).** The firmware detects the chip
  against the `db/` profile table (`nand_profile_lookup` over `PROFILE_IDS[]`/`PROFILES[]`)
  and drives geometry, ECC-status decoding, and the Quad-Enable write from the resolved
  `active_profile_t`. The legacy `chips.yml` → `CHIPS[]` path is kept as a compiled
  fallback (used only for a chip present in `chips.yml` but not `db/`); the unknown-chip
  manual-geometry raw dump is unchanged.
  - **Dosilicon DS35x1GA fixes land on-device:** correct 2-bit `[5:4]` ECC-status decode
    (was mis-read with Micron's 3-bit `[6:4]` scheme) and the Quad-Enable bit at `B0[0]`
    (was never set). On-silicon verification is pending a hardware bench.
- **`ecc_stripper.py` is profile-aware:** re-resolves the chip from the dump metadata and
  uses the profile's bad-block-marker spec (offset/length/polarity/page) instead of the
  hardcoded first-spare-byte check. Behavior is unchanged for the current chips.
```

- [ ] **Step 2: Update `CONTRIBUTING.md`**

Replace the Stage-1 "heads up — a layered profile database is being built" note with a statement that `db/` is now the device path, and add a short "add a chip to `db/`" pointer while keeping the `chips.yml` fallback documented. Concretely, change the blockquote note to:
```markdown
> **The device now detects chips from the `db/` profile database.** Add a new chip as a
> `db/chips/<PART>.yml` entry referencing a `db/profiles/<vendor>.yml` (see the
> [Developer Guide](docs/DEVELOPER_GUIDE.md#vendorprofile-architecture-stage-1) for the
> layer model), then run `pio run` to regenerate the resident table. The legacy
> `chips.yml` below still works as a compiled fallback for chips not yet in `db/`.
```

- [ ] **Step 3: Update `docs/DEVELOPER_GUIDE.md`**

In the "Vendor/profile architecture" section, change the "Stage 1 is host-side and additive — nothing here is on the device boot path yet" sentence to reflect Stage 2:
```markdown
**As of Stage 2 the device boot path is profile-first.** `setup()` reads the JEDEC id,
calls `nand_profile_lookup` (the generated `PROFILE_IDS[]` → `PROFILES[]` table), and
drives geometry, the ECC-status decode (`nand_profile_severity`), and the Quad-Enable
write from the resolved `active_profile_t`. A chip not in `db/` falls back to the legacy
`nand_chip_lookup`/`CHIPS[]` path; an unknown chip still gets manual geometry. The
`chips.yml` → `CHIPS[]` path is retained as that fallback.
```
Also update the **Boot flow** line in the Architecture section to mention `nand_profile_lookup` runs before `nand_chip_lookup`.

- [ ] **Step 4: Verify the docs render (anchors/links resolve) and are accurate**

Run: `grep -n 'nand_profile_lookup\|profile-first\|PROFILE_IDS' CHANGELOG.md CONTRIBUTING.md docs/DEVELOPER_GUIDE.md`
Expected: the new content is present; no dangling anchor (the `#vendorprofile-architecture-stage-1` anchor still exists in the Developer Guide). USER_GUIDE.md and QUICKSTART.md are intentionally NOT changed (no user-facing operational change).

- [ ] **Step 5: Commit** *(ask first)*

```bash
git add CHANGELOG.md CONTRIBUTING.md docs/DEVELOPER_GUIDE.md
git commit -m "docs: device read path is profile-first (Stage 2)"
```

---

## Deferred to later stages (named, not dropped)

- **Retiring the legacy path** (`chips.yml`/`gen_chips.py`/`CHIPS[]`/`nand_chip_t`/`nand_ecc.h`) and the v4.0.0 bump — held until the profile path is bench-verified on silicon (the user chose keep-fallback).
- **`nand_read_id()` widening to ≥3 bytes** and device-side `dev2` disambiguation — belongs with Stage 3's push protocol, where collisions actually arise; no resident collision exists today.
- **Driver opcode parameterization** (`nand_driver.cpp` reading opcodes from `active`) — a seam for future opcode-divergent families; current opcodes are identical, so wiring it now adds risk with no benefit (spec §7 calls it "the seam," not a fix).
- **STATUS2 exact-count path** (`status2_reg`, GD/Winbond) — stubbed `CORRECTED`, as in Stage 1.
- **`dump.py` writing profile identity into `*.meta.json`** — the stripper re-resolves from `mfr/dev` instead, so this is a self-describing-dump nicety, not required.
- **On-silicon DS35 bench** — the one thing the native tests + firmware build cannot cover: flash, dump DS35 with ECC on, confirm correct uncorrectable-page reporting and that the quad self-test passes.

## Self-Review

**1. Spec coverage (Stage 2 scope, § 7 + § 4.3):**

| Spec item | Task |
|---|---|
| Resident ID → profile lookup (§ 4.3 "resolve in resident[]") | 1 |
| Read path detects via profile, geometry from `active` (§ 7) | 2 |
| ECC decode data-driven; DS35 `generic2` fix (§ 7) | 3 |
| Quad-enable from `active.qe_*`; DS35 QE fix (§ 7) | 3 |
| Keep legacy path as fallback (user decision, overrides § 10 "retire") | 2, 3 |
| Bad-block detection uses `active.bbm` (§ 7) — host stripper | 5 |
| Stage-1 review carry-overs (oob bounds, name bytes, deepcopy, SCHEMA_VER) | 4 |
| `CONTRIBUTING.md` for `db/` (Stage-1 deferred) | 6 |

Deliberately deferred (with the user's keep-fallback / defer-silicon decisions): legacy-path retirement, `nand_read_id` widening, driver opcode parameterization, STATUS2, on-silicon bench — all listed above.

**2. Placeholder scan:** No "TBD/handle errors/similar to Task N" — every code step is literal. Device tasks (2, 3) are honestly gated on a firmware build rather than a host red-green, because `main.cpp` is not host-compilable; this is stated in the Global Constraints, not hidden.

**3. Type consistency:** `nand_profile_lookup(uint8_t, uint8_t) -> const active_profile_t*` is declared (Task 1 `nand_profile.h`), defined (Task 1 `nand_profile_lookup.cpp`), and consumed (Task 2 `main.cpp`) identically. `g_profile` (Task 2) is read in Task 3. `PROFILE_IDS[]`/`PROFILES[]` co-emitted in order (Task 1) and searched together (Task 1 lookup). `nand_profile_severity`/`NAND_SEV_*` reused from Stage 1 unchanged. `resolve_bbm`/`strip(..., bbm=...)` defined and consumed within Task 5. `chipdb.SCHEMA_VER`/`validate`/`get` (Task 4) match their Stage-1 call sites.

**Known soft spots (named):**
- Tasks 2–3 change device behavior with no automated runtime test; the firmware-build gate proves it compiles/links, and the pure decode/lookup it relies on is unit-tested, but correct on-silicon ECC/QE behavior is only provable on the bench (deferred by user decision).
- The profile path forces `cfg_ecc_on = false` at detection (global OFF/raw policy) because `active_profile_t` carries no per-chip ECC-default; the legacy path preserved `g_chip->ecc_default_on` (both current chips are `off`, so no behavior change). Intentional and consistent with the spec's default-raw policy.
- `cfg_bad_mark` is left unset on the profile path; it is already dead on-device (bad-block handling is host-side in the stripper), so this has no functional effect. Noted so a reviewer doesn't read it as an omission.
