# Vendor/Family Profile Architecture — Design

- **Date:** 2026-08-23
- **Status:** Approved design; stages 1 (host DB + `tools/chipdb.py`), 2 (resident table + read-path refactor) and 3 (push protocol + two-phase arm, `src/nand_session.h`) implemented
- **Revision:** r2 — folded in peer review (2026-08-23). Schema changes made
  while `schema_ver` is still unshipped: `ecc_map[16]` (out-of-bounds fix on the
  decode line), `read_id` moved to the chip layer, OOB uniform-sections →
  bounded region lists, STATUS2 `0x30` Winbond / `0xF0` GigaDevice split,
  market-plausibility capacity check moved host-side, restored `datasheet`/
  `notes` provenance, `name`-length guard, `dev2` ID-read widening.
- **Revision:** r3 — stage-1 implementation (2026-09-29). Flat-struct fields
  added while `schema_ver` 1 is still unshipped: `id_mfr/id_dev/id_dev2/id_flags`
  (the § 6 ID cross-check needs the expected ID *in* the blob), `planes` (the
  2-plane MT29F2G01 needs the plane-select bit on every cache read, fixed in
  PR #1), `op_read_cache_x4` (quad → single fallback needs both opcodes), and
  `bbm_pages` (bitmask for `bbm.pages`). YAML `bbm` keys are `offset`/`length`,
  because a bare `off` parses as a YAML 1.1 boolean. Authoritative layout: the
  `LAYOUT` table in `tools/chipdb.py` (120 bytes, naturally aligned; 128 bytes
  from schema v2, see `2026-10-05-spi-nor-design.md`).
- **Stage 3 notes** (2026-10-05): the arm is bound to the staged blob — `A`
  carries the blob's CRC32, and a mismatch disarms. Replies are `NRSP` frames
  (wire format in `docs/DEVELOPER_GUIDE.md`); a `G` while a push is unarmed is
  refused. The `dump.config.json` choice cache, the standalone NVS choice
  cache (menu `[C]`) and the profile-aware `ecc_stripper.py` (bad-block marker
  from the profile) are implemented.
- **Author:** Udara Vimarsha (with Claude)
- **Scope of this document:** Cycle 1 of a multi-cycle effort. The *schema* is
  designed for a full read+write programmer; this cycle *implements* the
  read-path foundation only. Write/erase, ONFI auto-detect, and new memory
  families are deferred to later cycles that this schema anticipates.

## 1. Problem & motivation

The firmware currently hardcodes one vendor's assumptions across the whole
codebase. Concretely, on the Dosilicon **DS35x1GA** (mfr `0xE5`, dev `0x71`,
2048+64, 4-bit/512B on-die ECC), three defects were found and verified against
the DS35x1GAxxx Rev.03 datasheet and captured dumps:

1. **ECC status decoded with the wrong scheme.** `nand_ecc.h` hardcodes the
   Micron MT29F 3-bit field `SR[6:4]`. The DS35 uses a 2-bit field `SR[5:4]`
   (bit 6 reserved). It happens to report *uncorrectable* correctly only
   because both encodings place that state at field-value `2`; the semantics
   (corrected tiers, refresh) are otherwise wrong.
2. **Quad-enable bit never set.** `chips.yml` marks DS35 `has_qe_bit: false`,
   but the datasheet and the Linux MTD driver both place QE at `B0[0]`. Quad
   reads silently fall back to single.
3. **OOB-blind stripping / bad-block detection.** `ecc_stripper.py` hardcodes
   the bad-block marker at OOB byte 0 with `!= 0xFF`, and has no notion of the
   per-vendor OOB layout.

The root cause is architectural: SPI NAND is only *partly* standardized. The
command skeleton is common, but **ECC-status semantics and OOB layout are
vendor-specific** and cannot live in shared code. Verified variance (Linux MTD
`spinand`, flashrom, SNANDer):

| Vendor            | ECC field | "uncorrectable" value |
|-------------------|-----------|-----------------------|
| Generic default   | `[5:4]`   | `0x2`                 |
| Dosilicon (DS35)  | `[5:4]`   | `0x2`                 |
| Micron (MT29F)    | `[6:4]`   | `0x2` (+ 4-6, 7-8 tiers) |
| GigaDevice (most) | `[5:4]`   | `0x2`                 |
| GigaDevice UC     | `[6:4]`   | `0x7`                 |
| XTX XT26G01C      | `[7:4]`   | `0xF`                 |

The same numeric value means different things across vendors, and both the
field width and the uncorrectable value vary. A single fixed decoder cannot
serve them.

The Dosilicon profile values used below (IDs `0xE5`/`0x71`, QE at `B0[0]`,
`generic2` ECC decode, 4×16 OOB with a 2-byte BBM at offset 0) match the
mainline Linux `spinand` Dosilicon driver byte-for-byte; the review reports that
driver was accepted into `nand/next` on 2026-01-19 (I have not re-verified the
exact commit this session). The local datasheet is Rev.03 while mainline cites
`DS35X1GAXXX_rev08` — reconcile the citation and drop the `PLACEHOLDER` ID
comments in `chips.yml` during migration (§ 10).

### Goals (cycle 1)

- A data-driven **family → profile → chip** model that groups vendor-specific
  configuration once and assigns it to chips by reference.
- A **hybrid** runtime: the host owns the full chip database; the device holds
  one resolved profile in RAM and runs the bulk loops autonomously (no
  per-page host chatter).
- A **verified, fail-closed** profile-push path (a malformed profile must never
  reach the read loop).
- **ID-collision disambiguation** (shared JEDEC IDs are common).
- Refactor the **read path** (ECC decode, QE, OOB strip, bad-block detection)
  to be profile-driven, fixing the three DS35 defects for all Dosilicon chips
  at once.

### Non-goals (deferred to later cycles)

- Write / erase / program (destructive; separate safety spec).
- ONFI parameter-page auto-detection (hook reserved: `id.onfi`).
- New memory families (SPI NOR, I2C/SPI EEPROM) — the schema anticipates them.
- Bad-block *policies* (Skip / Replace / Hard-Copy) and partition read/write.

## 2. Architecture decisions (resolved forks)

1. **Target:** design toward a full read+write universal programmer.
2. **Chip-knowledge location:** **hybrid** — host holds the full DB; the device
   holds one active profile *in RAM* and runs loops on-device. Standalone
   operation is preserved via a small resident subset compiled into firmware.
3. **Flattening:** **the host always flattens the 3 layers into one struct; the
   device never resolves.** The layered model is authoring-time only. At
   runtime the device sees a single flat `active_profile_t`.
4. **On-device ECC decode is pure data** — one generic decoder driven by
   `shift`/`mask`/`value-map`; no per-vendor code on the device.
5. **Cycle-1 build scope:** foundation + read refactor only (see Goals).

## 3. Data model — three layers

### 3.1 `family` (bus/command-set, memory-type-wide)

One `spi-nand` family today. Invariant across all vendors of the bus type.

```yaml
family spi-nand:
  opcodes:                     # write opcodes designed-in now, unused this cycle
    reset: 0xFF   read_id: 0x9F   get_feature: 0x0F   set_feature: 0x1F
    page_read: 0x13   read_cache: {x1: 0x0B, x2: 0x3B, x4: 0x6B}
    program_load: {x1: 0x02, x4: 0x32}   program_exec: 0x10
    block_erase: 0xD8   write_enable: 0x06   write_disable: 0x04
  feature_addrs:   {block_lock: 0xA0, config: 0xB0, status: 0xC0}
  status_bits:     {oip: 0, wel: 1, erase_fail: 2, prog_fail: 3}
  config_ecc_en_bit: 4         # B0[4]; a profile or chip may override
  address_model:   {row_bits: 24, read_dummy_bytes: 1, col_bits: 12}
  read_id_default: {method: dummy, id_bytes: 2}   # bootstrap 9F sequence used before a chip is known
```

`read_id_default` is the sequence the device uses to issue `9F` **before** it
knows which chip is in the socket (a chicken-and-egg: the read-ID method is
itself chip data). A chip whose ID read genuinely differs overrides it at the
chip layer (below) — it is *not* a vendor-wide property (see § 3.3).

### 3.2 `profile` (vendor / ECC-scheme quirks, grouped and named)

```yaml
profile dosilicon:
  datasheet: "DS35x1GAxxx (Dosilicon); matches the accepted mainline Linux spinand dosilicon driver"
  ecc:
    status_shift: 4    status_mask: 0x3      # [5:4], generic 2-bit
    scheme: generic2                          # named decoder; host expands to the 16-entry value_map
    strength: 4                               # bits/512B
    status2_reg: null                         # exact-count fetch: 0x30 Winbond / 0xF0 GigaDevice (deferred)
  qe:               {has: true, feature_addr: 0xB0, bit: 0x01}
  oob_layout:                                 # bounded region lists; BBM is separate. Cycle 1 uses only bbm.
    free_regions: [[2, 6], [16, 8], [32, 8], [48, 8]]   # 4×16 uniform, BBM in section 0; per mainline ooblayout
    ecc_regions:  [[8, 8], [24, 8], [40, 8], [56, 8]]
    bbm:          {off: 0, len: 2, good: 0xFF, pages: [first]}
  config_ecc_en_bit: null                     # optional override of family default
```

`read_id` is deliberately **not** a profile field. It varies per chip *within* a
vendor — GigaDevice is the proof: mainline Linux gives GD5F1GQ4xA / GD5F1GQ4UExxG
the address-based read-ID method but GD5F1GQ5UExxG the dummy-byte method, and the
ID *width* varies too (GD5F4GQ4RC carries a 2-byte device ID). It lives at the
chip layer with a family default (§ 3.1, § 3.3). Note also: a "dummy byte" is not
an address — the two Linux methods are distinct, so the old `needs_addr` framing
was both mis-layered and ambiguous.

Named `scheme` values expand (on the host) into the on-device 16-entry
`value_map`: `generic2` (`[5:4]`), `micron3` (`[6:4]`), `gd_uc` (`[6:4]`,
uncorrectable `0x7`), `xtx4` (`[7:4]`, uncorrectable `0xF`), `xtx_g0xa`
(mainline `[5:2]`, uncorrectable `8`, 8-bit-corrected `12`), `winbond_refresh`.
A profile that fits no named scheme provides an explicit `value_map`; the host
**must** fill all 16 entries (unnamed ⇒ `uncorrectable`).

### 3.3 `chip` (identity + geometry + two references)

```yaml
chips:
  DS35Q1GA:
    id:       {mfr: 0xE5, dev: 0x71, dev2: null, onfi: null}   # dev2 = same-ID tiebreaker
    family:   spi-nand
    profile:  dosilicon
    geometry: {page_size: 2112, spare_size: 64, pages_per_block: 64, total_blocks: 1024, planes: 1}
    read_mode: single
    vcc_mv:   3300
    read_id:  {method: dummy, id_bytes: 2}     # optional; omit to inherit family.read_id_default
    datasheet: "docs/datasheets/DS35x1GAxxx_SPI_NAND.pdf"      # required per CONTRIBUTING.md
    notes:    "off a Huawei ONT; boot region = blocks 0-7 written raw"
    overrides: {}                                              # rare per-chip escape hatch
```

Every chip **must** carry a `datasheet` citation (existing `CONTRIBUTING.md`
discipline; the community DB needs provenance most of all). `read_id` and
`config_ecc_en_bit` resolve profile-default → chip-override, like any other
inheritable field.

Prior-art grounding: `dev2`/`read_mode`/`dummy` from SNANDer
(`SPI_NAND_FLASH_INFO_T`); capability-flag / block-eraser / op-ref shape from
flashrom (`struct flashchip`); family-core + profile-eccinfo + oob_layout from
Linux MTD `spinand`.

## 4. Firmware/host split

**Invariant: the host flattens; the device trusts.** Flattening happens on the
host in exactly one place (`chipdb.py`), used both at build time (resident
header) and at runtime (push blob).

```
        ┌─────────────────── HOST (owns full YAML DB) ───────────────────┐
        │  families + profiles + chips  ──resolve+flatten──► flat blob     │
        └───────────────┬───────────────────────────────┬────────────────┘
              build time │ (resident subset only)        │ runtime (unknown chip)
                         ▼                                ▼  push over transport
        ┌──────────────────────── DEVICE (ESP32) ────────────────────────┐
        │  resident[] = pre-flattened structs (10–20 common chips)         │
        │  active     = one active_profile_t in RAM                        │
        │  detect: read 9F → resolve in resident[] → load into `active`    │
        │          miss + host present → host pushes flat blob → `active`  │
        │          miss + standalone   → "unknown chip, connect host"      │
        │  then: bulk read loop runs against `active`, no per-page chatter  │
        └─────────────────────────────────────────────────────────────────┘
```

Resolution order: resident-first (common chips run standalone), host-push
fallback for the long tail. An unknown chip off-host fails loud; it never
guesses.

### 4.1 Flattened device struct

The only representation the loops ever see:

```c
typedef struct {
  char     name[24];                       // NUL-terminated; strlen ≤ 23 (tier-2 enforced)
  uint32_t page_size, spare_size, pages_per_block, total_blocks;
  uint8_t  op_page_read, op_read_cache, op_get_feat, op_set_feat, op_status_addr, op_cfg_addr;
  uint8_t  ecc_en_bit;                     // bit in the config (B0h) register that toggles on-die ECC
  uint8_t  ecc_shift, ecc_mask;            // e.g. 4, 0xF  (mask may be up to a 4-bit field)
  uint8_t  ecc_map[16];                    // field-value → severity {OK,CORR,CORR_REFRESH,UNCOR};
                                           //   MUST cover 1<<popcount(mask); unmapped ⇒ UNCOR
  uint8_t  status2_reg;                    // 0 = none; 0x30 Winbond / 0xF0 GigaDevice exact-count (deferred)
  uint8_t  id_method, id_n_bytes;          // how the device issues 9F (0=addr, 1=dummy; ID byte count)
  uint8_t  qe_addr, qe_bit;                // 0 if none
  uint8_t  read_mode; uint16_t vcc_mv;
  // OOB layout — bounded region lists (data-driven equivalent of Linux ooblayout callbacks).
  // Cycle 1 consumes bbm_* only; the region lists are carried now for the write/BBT cycle.
  uint8_t  bbm_off, bbm_len, bbm_good;     // bad-block marker: offset in spare, length, good polarity
  uint8_t  oob_free_n, oob_ecc_n;          // valid region count, each ≤ 4
  uint16_t oob_free[8];                    // up to 4×(off,len) free/user regions in spare
  uint16_t oob_ecc[8];                     // up to 4×(off,len) ECC regions in spare
} active_profile_t;
```

Two schema changes here are deliberate and load-bearing:

- **`ecc_map[16]`, not `[8]`.** `ecc_mask` is allowed to be a 4-bit field
  (`0xF`) — the doc's own XTX `[7:4]` row uses value `0xF`, and mainline Linux's
  XT26G0xA encoding is `[5:2]` (uncorrectable at field value `8`, 8-bit-corrected
  at `12`). Indexing `ecc_map[(status>>shift)&mask]` with a 4-bit mask reaches
  index 15. An 8-entry array is an out-of-bounds read **on the safety-critical
  decode line this whole design exists to protect**. The host fills all 16
  entries explicitly; any value the scheme doesn't name defaults to `UNCOR`
  (conservative for a data-integrity tool — never silently call a suspect page
  clean).
- **OOB as region lists, not uniform sections.** `sections/section_bytes/
  ecc_bytes` only expresses uniform layouts (Dosilicon, Micron). Real parts
  aren't uniform: Winbond W25N02KV and XTX XT26GxxD use a split-half layout
  (free bytes in the first half of spare, ECC in the second). Since the struct
  *is* the wire format, the general form is nearly free now and a breaking
  change later.

`read_id` moved out of the profile (vendor) layer — see § 3. The struct keeps
only the resolved `id_method`/`id_n_bytes` the device uses to re-issue `9F`.

### 4.2 Pure-data ECC decoder

Replaces the `nand_ecc.h` `if uncorrectable / if refresh` ladder with one line:

```c
severity = active.ecc_map[(status >> active.ecc_shift) & active.ecc_mask];
```

Every vendor case reduces to different `ecc_shift`/`ecc_mask`/`ecc_map` bytes
the host fills in from the named `scheme`. `ecc_map` is fully populated (all 16
entries) by the host; entries the scheme leaves unnamed decode to `UNCOR`, so a
reserved/unexpected field value can never masquerade as a clean read. The device
carries no vendor knowledge. **Deferred exception:** the "read the second status
register for the exact bitflip count" path — register **`0x30` on Winbond**,
**`0xF0` on GigaDevice** (they differ; `status2_reg` is per-profile) — needs a
code path; this cycle stubs that severity as `CORRECTED` and flags it for the
write-adjacent cycle.

### 4.3 Build / footprint

`gen_chips.py` → `gen_profiles.py`: resolves the YAML and emits only
`resident: true` chips as pre-flattened `active_profile_t` initializers into
`nand_chips_generated.h`. The full DB never touches firmware. The two existing
chips (MT29F, DS35) migrate and are tagged resident, so nothing regresses.

**RAM cost is one struct, not the table.** `resident[]` is `static const`, so it
lives in flash (~120–160 bytes/chip after the struct grew for Walls 1 & 3);
only the single `active` copy occupies RAM. Adding resident chips costs flash,
never the dump-time working set.

## 5. ID-collision disambiguation

The ID lookup is **one-to-many**: DB keyed by `(mfr, dev)` returns a candidate
list. A ladder resolves it (first match wins):

1. **Extra ID bytes (`dev2`)** — `9F` third byte; if candidates differ there,
   auto-select. Zero friction.
2. **ONFI model string** — reserved hook (`id.onfi`); auto-selects when the
   ONFI cycle lands. Stubbed now.
3. **Manual selection** — host lists candidates (name + geometry); user picks;
   choice cached in `dump.config.json`. Standalone: same list on the serial
   menu, choice saved to NVS.

**Fail-closed:** an ambiguous ID never auto-arms a guessed geometry. Ladder
exhausted with no cached/user choice → device idles, reports `E_AMBIGUOUS_ID`
with the candidate list. Composes with the § 6 ID cross-check: that interlock
requires `pushed.id == silicon.id`; disambiguation only chooses *which*
same-ID profile is pushed.

## 6. Push protocol & verification pipeline (safety-critical)

A malformed profile is worse than none: bad `page_size` overruns the DMA
buffer, bad geometry runs the address generator off the chip, and (once write
lands) a wrong-chip profile erases real blocks. The path is **fail-closed**.

**Wire format** (extends proto-v2 CRC32 framing):

```
[ 'P','R','F' magic ][ schema_ver u8 ][ len u16 ][ flat active_profile_t bytes ][ crc32 ]
```

**Three validation tiers — all must pass before a value is even a candidate:**

1. **Transport integrity** — recomputed `crc32` matches; `len` equals the exact
   struct size for this `schema_ver`; unknown/newer `schema_ver` rejected
   outright (never guess at unknown fields).
2. **Structural** — `ecc_shift ≤ 7`; `ecc_mask ∈ {0x1,0x3,0x7,0xF}`; `ecc_map`
   is fully populated for `1<<popcount(ecc_mask)` entries with valid severities
   (guards the Wall-1 out-of-bounds/garbage-severity read directly); `name` is
   NUL-terminated with `strlen ≤ 23`; `oob_free_n ≤ 4` and `oob_ecc_n ≤ 4` with
   every region inside `spare_size`; required opcodes non-zero; `read_mode` /
   `id_method` known; `bbm_off + bbm_len ≤ spare_size`.
3. **Semantic / physical sanity (device-side catastrophe interlocks — hard,
   physical bounds only)**
   - `spare_size < page_size`; `main = page_size − spare_size > 0`;
     `pages_per_block` a power of two; `total_blocks > 0`.
   - **`page_size ≤ compiled MAX_PAGE_BUFFER`** — hard bound; the line between
     "wrong dump" and "heap corruption". Non-negotiable.
   - `page_size × pages_per_block × total_blocks` fits `uint32` (overflow guard).

   The *market-plausibility* window (e.g. the 512 Mb – 8 Gb capacity class) is
   **not** a device check — it moves host-side (§ 8). It is a fuzzy heuristic,
   not a physical limit; baking it into firmware would force a reflash the first
   time a legitimate 16 Gb part ships. The device enforces only what protects
   its own memory; the host is where fuzzy plausibility (and community-PR
   validation) evolves.

**ID cross-check (the key interlock).** The device already read the real JEDEC
ID. The pushed profile carries its expected `mfr/dev`. **Mismatch → reject
(`E_ID_MISMATCH`).** The profile is bound to the silicon in the socket, not
taken on faith.

**Two-phase arm (echo-before-commit).** After all checks pass, the device does
not go live. It echoes a summary — `name`, geometry, detected-vs-expected ID,
recomputed CRC — and waits for an explicit host `ARM`. Only then does `active`
become live. Until armed: no read loop; (write cycle) no erase/program path
reachable.

**Backward compatibility.** The push path upgrades the current one-way stream
into a small command session: `I` (query detected ID/candidates) → `P` (push
blob) → echo → `A` (arm) → `G` (go/dump). **Bare `G` must keep working** for a
resident chip that resolved from `9F` alone — so existing `dump.py` clients and
standalone use don't break the day this lands. A resident chip skips `P`/`A`
(it was flattened and sanity-checked at build time); only a pushed profile
requires the arm handshake.

**Fail-closed everywhere.** Any failure → profile discarded whole (never
partial-applied), device stays idle, and reports the specific failing check
(`E_BAD_CRC`, `E_SCHEMA_VER`, `E_PAGE_TOO_BIG`, `E_ID_MISMATCH`,
`E_AMBIGUOUS_ID`). There is no "load anyway."

**Resident subset guarded by the same rules.** `gen_profiles.py` runs tiers 2–3
at build time and fails the build on a bad profile; the device re-asserts the
cheap sanity checks on each resident entry at boot.

## 7. Read-path refactor (everything reads from `active`)

- **ECC decode** — delete Micron constants in `nand_ecc.h`;
  `nand_get_ecc_status()` still reads `C0h`; interpretation becomes the § 4.2
  data-driven line; `main.cpp` accounting switches to the severity enum.
  **DS35 fix:** `generic2` → correct `[5:4]` decode.
- **Quad enable** — `main.cpp` reads `active.qe_addr/qe_bit` (0 = none) instead
  of `g_chip->has_qe_bit`. **DS35 fix:** `qe_addr=0xB0, qe_bit=0x01` → quad
  self-test can pass.
- **Opcodes** — `nand_driver.cpp`'s hardcoded `0x13/0x0B/0x6B/0x0F/0x1F` and
  feature addrs come from `active` (same values today; the seam for future
  families).
- **Bad-block detection** — uses `active.bbm` (offset, length, polarity, which
  page) instead of `page[main] != 0xFF`.
- **ID read width** — `nand_read_id()` returns `uint16_t` (2 bytes) today. The
  `dev2` disambiguation ladder (§ 5) needs ≥ 3 ID bytes, so this widens to a
  ≥ 3-byte read honoring `id_method`/`id_n_bytes`. Small but load-bearing
  firmware change the schema implies rather than states.

**DS35 net:** all three defects resolve to correct data-driven behavior, and
every other Dosilicon chip inherits the fix.

## 8. Host tools

- **`chipdb.py` (new)** — the single home for load → validate → resolve →
  flatten. Validates every `chip → profile → family` reference and runs tier-2/3
  sanity at load time (a broken community PR fails on the host, never the
  device). Expands each named `scheme` into a **fully populated 16-entry
  `ecc_map`** (unnamed ⇒ `UNCOR`) and runs the **fuzzy market-plausibility
  capacity check here** — the heuristic the device deliberately doesn't carry.
  Given detected `(mfr, dev, dev2?)`: gather candidates → disambiguate → resolve
  to one flat `active_profile_t` → serialize to the wire blob.
- **Byte-identical packing contract** — the Python packer and the C struct
  agree on field order, sizes, endianness (LE), name padding. `gen_profiles.py`
  reuses the same flattener, so a resident chip and a pushed chip produce
  identical bytes. Enforced by the § 9 golden-blob test.
- **`dump.py` (orchestrator)** — learns the detected ID / candidate state;
  resolves via `chipdb.py` (prompting on collision); pushes the blob; waits for
  the echo-summary; sends `ARM` only when it matches. Records the chip choice in
  `dump.config.json`; writes profile identity (`name`, `family`, `schema_ver`,
  ECC scheme) into `*.meta.json` so dumps are self-describing.
- **`ecc_stripper.py` (profile-aware)** — reads `oob_layout` (`free_regions` /
  `ecc_regions`) + `bbm` from the meta (or re-resolves by recorded chip name);
  bad-block check uses `bbm {off, len, good, pages}` instead of the hardcoded
  `page[main] != 0xFF`. Still strips the full spare for the clean image this
  cycle; the region lists it now carries are what the future BBT/write path
  consumes. Optional cosmetic alias `oob_stripper.py` (back-compat symlink).
- **DB layout** — `db/families/`, `db/profiles/`, `db/chips/` as small YAML
  files, one-chip-per-PR, structured for later extraction to a git submodule.

## 9. Testing

- **Golden-blob (highest risk):** `chipdb.py` packs DS35 → a native
  (`pio test -e native`) C test unpacks into `active_profile_t` and asserts
  every field. To make it link, the struct **and** the pure-data decoder live in
  an Arduino-free module pulled into the native env's `build_src_filter`; the
  same module carries `static_assert(sizeof(active_profile_t) == N)`, mirrored by
  a size assertion in the test, so field-order/padding drift fails CI, not the
  device. Protects the host-flattens/device-trusts seam.
- **ECC decoder table test:** the full matrix — `generic2 [5:4]`,
  `micron3 [6:4]`, `gd_uc` value-`0x7`, `xtx4 [7:4]` value-`0xF`, `xtx_g0xa
  [5:2]` value-`8` (mainline) — each `(scheme, status_byte) → severity`. The
  `xtx4`/`xtx_g0xa` cases exercise index 15 / a 4-bit field, i.e. the Wall-1
  `ecc_map[16]` bound directly.
- **Verification pipeline:** malformed blobs assert fail-closed with the right
  code; `active` untouched, device idle. Include an over-wide `ecc_mask`/short
  `ecc_map` and an over-long `name` (the new tier-2 checks).
- **Disambiguation:** shared `(mfr,dev)` — differing `dev2` auto-selects;
  identical `dev2` returns candidates; cached choice honored.
- **DB validation + stripper:** dangling refs / insane geometry rejected at
  load; stripper finds a marker at a profile-defined non-zero offset/polarity.
- **Real-data characterization (local, not CI):** the `target/*.bin` fixtures
  are **gitignored** (`target/`, `lab/`), so this is a developer/bench test, not
  a CI gate. The two Aug-21 DS35 dumps (`2112×65536` = 1024 blocks — genuine
  DS35 geometry) let us run the *software* path now: strip the ECC-on dump
  through the new profile-aware stripper and assert the same clean image and
  magics as today (`UBI#` @ `0x100000`, uImage/squashfs offsets). The 285 MB
  Aug-16/18 dumps are MT29F-geometry and give the Micron no-regression fixture.
- **Hardware bench (pending T14):** the one thing the software path can't cover —
  on DS35 silicon confirm quad self-test now passes and the *live* ECC-on read
  reports the correct uncorrectable count on the boot region.

## 10. Migration & rollout

- **Migration:** move the two existing chips into `db/chips/`; create
  `db/families/spi-nand.yml`, `db/profiles/micron.yml` (scheme `micron3`,
  strength 8, no QE), `db/profiles/dosilicon.yml` (scheme `generic2`,
  strength 4, QE `B0[0]`, 4×16 OOB). `gen_chips.py` → `gen_profiles.py`;
  platformio pre-hook updated. One-shot converter for custom `chips.yml`. Old
  dumps stay strippable via `--page-size/--spare-size` or a new `--profile`.
- **Versioning:** `schema_ver` starts at 1, bumps on any flat-struct layout
  change; device rejects unknown versions (fail-closed). Breaking DB/config
  format change → project **v4.0.0**. NVS config carries a version and
  re-detects rather than loading a stale layout.
- **Implementation stages** (each independently testable): (1) host DB +
  validation + tests, no device change; (2) device resident-table + read-path
  refactor; (3) push protocol + two-phase arm. Docs (`CONTRIBUTING.md` "add a
  chip", USER/DEVELOPER guides) updated alongside.

## 11. Risks & open questions

- **STATUS2 exact-count path** (GD/Winbond) is stubbed as `CORRECTED` this
  cycle; a profile that needs it will under-report refresh pressure until the
  write-adjacent cycle implements the second-register read.
- **Standalone ambiguous ID** requires either a cached NVS choice or a host;
  deliberately no silent default (fail-closed).
- **`MAX_PAGE_BUFFER`** sets the largest page any pushed profile may declare;
  chips with larger pages need a firmware rebuild with a bigger buffer. Chosen
  bound and its RAM cost to be set during implementation.
- **Transport for push** reuses serial/WiFi proto-v2 framing; the ARM/echo
  round-trip adds a handshake the current one-way stream lacks.

## 12. Deferred cycles (anticipated by this schema)

1. Write / erase / program + write-safety (destructive; own safety spec).
2. ONFI parameter-page auto-detect (`id.onfi` hook; auto-geometry; save custom).
3. New families: SPI NOR (25-series), then I2C/SPI EEPROM, microwire.
4. Bad-block policies (Skip / Replace / Hard-Copy) and partition read/write.
5. OTP/UID area, serialization, blank-check — production niceties.
