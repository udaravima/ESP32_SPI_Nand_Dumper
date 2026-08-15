# ESP32 SPI NAND Dumper — Generalization & Correctness Design

**Date:** 2026-08-15
**Status:** Draft for review
**Author:** Udara Vimarsha (with Claude)

## 1. Summary & Motivation

The tool works on the 1 Gbit FORESEE DS35 it was written around, but several
hard-coded constants and one feature-register assumption are silently wrong for
the 2 Gbit Micron **MT29F2G01ABAGD** (JEDEC `0x2C 0x24`) confirmed in the target
boot log. The single worst bug means a 2 Gbit chip **cannot be dumped correctly
today**: the row address overflows a `uint16_t`, so the upper 128 MB is never
read and the lower 128 MB is dumped twice.

This design does three things at once, because the generalization cannot be
correct without the fixes:

1. **Fix the show-stopper bugs** (row address, page-size consistency, verify
   buffer overflow, ECC-status mask, quad-abort).
2. **Generalize** geometry/config into one JEDEC-ID-keyed chip registry
   (`chips.yml`) that outside contributors can extend, plus a firmware↔PC
   geometry handshake so the host tools stop guessing.
3. **Broaden hardware support**: ESP32-S3 alongside ESP32-classic, a runtime
   quad self-test with automatic fallback to single, and per-chip voltage
   metadata for future level-shifted parts.

## 2. Goals / Non-Goals

**Goals**
- A correct, full-size, non-aliased dump of the MT29F2G01 (and the DS35).
- Auto-detect the chip by JEDEC ID; auto-fill geometry; manual override for
  unknown chips without reflashing.
- Community-friendly chip registry: add a chip in ~6 lines of YAML + a PR.
- ECC selectable per dump, **default OFF/raw** (raw is reversible; corrected is
  lossy), recorded in dump metadata.
- Build for ESP32-classic and ESP32-S3 from one source tree.
- Quad reads that degrade gracefully to single when the hardware/wiring can't
  sustain them.

**Non-Goals (YAGNI)**
- No write / erase / program (read-only dumper).
- No runtime filesystem config (rejected: too many moving parts for a table that
  changes rarely).
- No block-unlock (block lock gates PROGRAM/ERASE only; reads are always
  allowed).
- No ONFI parameter-page auto-geometry (the JEDEC table covers us; possible
  future).
- No firmware voltage switching (level shifting is external hardware, "later").

## 3. Chip Registry (`chips.yml`) + Codegen

### 3.1 Registry format

`chips.yml` at repo root is the single human-editable source of truth. Flat map
keyed by part name:

```yaml
chips:
  MT29F2G01ABAGD:
    mfr_id:  0x2C
    dev_id:  0x24
    page_size:       2176    # total bytes read per page (main + spare)
    spare_size:      128     # of which this many are spare/OOB
    pages_per_block: 64
    total_blocks:    2048
    bad_block_mark:  0x00    # value written to spare[0] of a bad block's page 0
    has_qe_bit:      false   # Micron: quad needs no enable bit
    ecc_default:     off     # global default is off/raw regardless
    vcc_mv:          3300    # documentation; 1800 parts need a level shifter
    notes: "2Gb SLC, 8-bit/512 on-die ECC, 2 planes x 1024 blocks"

  DS35Q1GA:
    mfr_id:  0xE5            # placeholder — confirm from silicon / DS35 datasheet
    dev_id:  0x71            # placeholder — confirm from silicon
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

For chips that *do* have a QE bit (Winbond W25N, GigaDevice GD5F), two optional
fields describe where it lives:

```yaml
    has_qe_bit:      true
    qe_feature_addr: 0xB0
    qe_bit:          0x01
```

**`vcc_mv`** and **`notes`** are documentation-only (not required by firmware
logic, but `vcc_mv` is surfaced in a build warning if it differs from the board
I/O voltage — future). ESP32 GPIO wiring is deliberately **not** in this file:
wiring is a property of the board, identical across chips (see §5).

### 3.2 Generator (`tools/gen_chips.py`)

Runs as a PlatformIO `pre:` build hook (`extra_scripts = pre:tools/gen_chips.py`).

- Parses `chips.yml` (PyYAML; the hook auto-installs it into PlatformIO's Python
  env if missing — standard PIO pattern).
- **Validates**: required fields present and typed; every `{mfr_id, dev_id}` pair
  unique; `spare_size < page_size`; `pages_per_block` a power of two;
  `has_qe_bit: true` requires `qe_feature_addr` + `qe_bit`. On any error it
  **fails the build** with a message naming the offending chip and field — never
  emits a half-broken table.
- **Derives** `page_addr_bits = log2(pages_per_block)` (validation guarantees an
  integer).
- Emits `src/nand_chips_generated.h`: a `static const nand_chip_t CHIPS[]` array
  and `CHIPS_COUNT`.

The generated header is **committed to git** so a fresh clone or CI build works
even without running the hook; the hook keeps it in sync during local builds.

The generator itself is unit-tested (host Python): valid YAML → expected header
bytes; each validation failure → non-zero exit + clear message.

## 4. Firmware Data Model

`src/nand_chips.h` (hand-written, stable API):

```c
typedef enum { NAND_READ_SINGLE = 0, NAND_READ_QUAD = 1 } nand_read_mode_t;

typedef struct {
  const char *name;
  uint8_t  mfr_id, dev_id;
  uint16_t page_size;        // total bytes/page (main + spare)
  uint16_t spare_size;       // spare bytes within page_size
  uint16_t pages_per_block;
  uint16_t total_blocks;
  uint8_t  page_addr_bits;   // log2(pages_per_block); derived by generator
  uint8_t  bad_block_mark;
  bool     has_qe_bit;
  uint8_t  qe_feature_addr;  // valid iff has_qe_bit
  uint8_t  qe_bit;           // valid iff has_qe_bit
  bool     ecc_default_on;
  uint16_t vcc_mv;
} nand_chip_t;

const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id);
```

A single runtime `nand_info` (the selected/edited chip) drives the driver and
the dump loop. `page_size`, `pages_per_block`, `total_blocks` become **runtime
values**, not compile-time `#define`s.

## 5. Board Abstraction (ESP32-classic + ESP32-S3)

Board wiring lives in `src/board_pins.h`, selected at compile time:

```c
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  /* ESP32-S3 FSPI (SPI2) IOMUX pins — GPIO9-14 are plain I/O on the S3:
     not flash/PSRAM (those are GPIO26-37), not strapping (0/3/45/46),
     not USB (19/20) or UART0 (43/44). Confirmed from S3 datasheet Table 2-1. */
  #define NAND_PIN_CLK  12   // FSPICLK
  #define NAND_PIN_D0   11   // FSPID  (MOSI / SIO0)
  #define NAND_PIN_D1   13   // FSPIQ  (MISO / SIO1)
  #define NAND_PIN_D2   14   // FSPIWP (SIO2, quad only)
  #define NAND_PIN_D3    9   // FSPIHD (SIO3, quad only)
  #define NAND_PIN_CS   10   // FSPICS0
#else /* ESP32 classic — VSPI (SPI3) IOMUX pins */
  #define NAND_PIN_CLK  18
  #define NAND_PIN_D0   23
  #define NAND_PIN_D1   19
  #define NAND_PIN_D2   22
  #define NAND_PIN_D3   21
  #define NAND_PIN_CS   5
#endif
```

- **SPI host (per target):** never SPI0/SPI1 — those are the chip's own built-in
  flash/PSRAM controller. Use a **general-purpose GPSPI** host, selected per
  target so each board keeps its native IOMUX pins:
  - **ESP32-classic → `SPI3_HOST`** (the peripheral the deprecated `VSPI_HOST`
    alias points at). The existing pins (18/23/19/22/21/5) are its IOMUX mapping
    (`VSPICLK`/`VSPID`/`VSPIQ`/`VSPIWP`/`VSPIHD`/`VSPICS0`, confirmed in the WROOM
    datasheet Table 3), so classic keeps full-speed IOMUX routing.
  - **ESP32-S3 → `SPI2_HOST`** (a.k.a. FSPI; SPI0/1 are the S3's flash/PSRAM).
    Uses the FSPI IOMUX pins GPIO9-14 (§5 board_pins).

  A dumper runs at ≤40 MHz for signal integrity anyway (default 1 MHz), so the
  IOMUX-vs-GPIO-matrix clock ceiling is not a practical constraint either way.

- **Forbidden pins (chip's own SPI flash/PSRAM):** confirmed from the WROOM
  datasheet — **GPIO6–11 are the internal QSPI flash (not even led out)**, and on
  PSRAM module variants **GPIO16/17 go to PSRAM**. Board pin defaults must avoid
  these; the S3 has its own reserved flash/PSRAM GPIO set to avoid (S3 datasheet,
  §13). Wiring any of them to the external NAND would conflict with the ESP32's
  boot flash.
- **`platformio.ini`:** shared `[env]` base + two envs:

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
```

- **Voltage:** ESP32 I/O is 3.3 V. The `ABAGD` (3.3 V) target wires direct; the
  1.8 V `ABBGD` variant needs a level shifter. `vcc_mv` in the registry records
  this per chip. No firmware logic now.

## 6. Boot / Auto-Detect Flow

Restructured `setup()` (today the menu runs before SPI is up, so it can't know
the chip):

1. Bring up SPI at a **conservative 1 MHz**.
2. `nand_reset()`, read ID (`9Fh`), `nand_chip_lookup(mfr, dev)`.
3. **Found** → load geometry + capabilities into `nand_info`; default ECC from
   the global OFF/raw policy. **Not found** → warn, seed `nand_info` with safe
   defaults for manual entry.
4. Show the config menu, now displaying the **detected** chip and pre-filled
   geometry. User can override any field, toggle ECC, pick read mode, or
   hand-enter an unknown chip.
5. Apply final clock/mode; **allocate page + verify buffers to the actual
   `page_size`** (see §8); set `max_transfer_sz` from it.
6. If quad selected: run the quad self-test (§7). Then dump.

## 7. Read Path, ECC, and Quad Self-Test

- **ECC toggle:** `nand_set_ecc(bool)` → `SET FEATURE B0h` bit 4 (`ECC_EN`).
  Global default OFF/raw; per-dump override in menu. State recorded in the
  geometry header (§9).
- **ECC status:** fix `nand_get_ecc_status()` to mask **3 bits** (`& 0x07`) —
  the field is ECCS0-2 (status bits 4-6), not two bits. Meaningful only when ECC
  is ON (datasheet: ECC status is invalid when ECC disabled); when ON, per-page
  status is aggregated into the dump summary + metadata.
- **Quad enable:** the QE-bit write runs **only** for `has_qe_bit` chips, against
  that chip's `qe_feature_addr`/`qe_bit`. Micron/DS35 are `false`, so quad no
  longer aborts init on the target (the current `ESP_FAIL` bug).
- **Quad data path:** keep `6Bh` (1-1-4) with `SPI_TRANS_MODE_QIO` (data-only
  quad; address stays single — verified correct ESP-IDF semantics). Document
  `EBh` (1-4-4) with `QIO | DIOQIO_ADDR` as the fallback framing to try if `6Bh`
  misbehaves on hardware.
- **Quad self-test + automatic fallback:** on boot, when quad is requested, read
  one page via quad and the same page via single, compare. Match → use quad.
  Mismatch (or `has_qe_bit` unsupported) → **warn and fall back to single x1**.
  This converts "quad may not work on this board" from a silent corrupt dump into
  a logged, handled downgrade. Single x1 (`0Bh`) is always the guaranteed-good
  default.

## 8. Correctness Fixes (bundled)

| Fix | File(s) | Why it matters |
|---|---|---|
| Row address → `uint32_t` end to end | `nand_driver.{h,cpp}`, `main.cpp` | The headline bug: 17-bit rows so 2 Gbit chips read fully instead of aliasing the upper half into the lower |
| Row formula → `(block << page_addr_bits) \| (page & (pages_per_block-1))` | `main.cpp` | Generalizes past 64-pages/block chips |
| Verify buffer + `max_transfer_sz` sized to **actual** `page_size` | `nand_driver.cpp` | Kills the heap overflow when page size > 2176 with Verify on |
| `nand_get_ecc_status` mask `& 0x07` | `nand_driver.cpp` | 3-bit ECC field, not 2 |
| Verify logic: a matching first pair returns `true` regardless of `max_retries` | `nand_driver.cpp` | `max_retries=0` no longer falsely fails good pages |

## 9. Geometry Handshake + Sidecar Metadata

The firmware sends a fixed **32-byte little-endian header** immediately after the
`'G'` trigger, before the page stream:

| Offset | Size | Field |
|---|---|---|
| 0  | 6 | magic `"NANDMP"` |
| 6  | 1 | proto_version (=1) |
| 7  | 1 | flags: bit0 ecc_on, bit1 quad, bit2 verify |
| 8  | 2 | page_size |
| 10 | 2 | spare_size |
| 12 | 2 | pages_per_block |
| 14 | 2 | total_blocks |
| 16 | 4 | total_pages |
| 20 | 1 | mfr_id |
| 21 | 1 | dev_id |
| 22 | 1 | page_addr_bits |
| 23 | 1 | reserved |
| 24 | 4 | total_bytes |
| 28 | 4 | CRC32 of bytes 0..27 |

- **`dump.py`** reads and CRC-checks the header first, self-computes the expected
  byte count (no more hard-coded `PAGE_SIZE`/`TOTAL_PAGES`), streams the dump, and
  writes a **`<dump>.meta.json`** sidecar: geometry, ECC state, read mode, chip
  ID, byte count, timestamp.
- **`ecc_stripper.py`** reads geometry from the sidecar (or CLI overrides):
  parametrized `page_size`, `spare_size`, `total_blocks` — **this is what
  structurally eliminates the 2112-vs-2176 bug** and the hard-coded "1024 blocks".

Breaking protocol bump, acceptable because both ends live in this repo. The `'G'`
trigger is unchanged.

## 10. Host Tools

- **`dump.py`:** header read + CRC, self-config, sidecar metadata, and a
  rewritten (readable) progress line replacing the current tangled expression.
- **`ecc_stripper.py`:** geometry from sidecar/CLI; bad-block marker check stays
  (`spare[0] != good`, marker value from metadata); drop fixed filenames and the
  "1024 blocks" print.
- **`tools/binary_compare_fix.py`:** unchanged behavior; add a unit test.

## 11. Testing

- **Host Python (TDD):** generator (valid → expected header; invalid → clear
  failure), `ecc_stripper` geometry handling, `binary_compare_fix`, header +
  metadata round-trip.
- **Firmware pure logic (native `pio test -e native`):** row-address computation,
  chip lookup, header pack/unpack — everything with no hardware dependency.
- **Manual bench (hardware, yours):** full MT29F2G01 dump end-to-end; quad
  self-test behavior on real wiring; S3 build + dump once GPIOs are confirmed.
  These are called out explicitly as un-unit-testable.

## 12. Documentation

- **`README.md`:** reconcile the chip identity (it currently describes a
  "DS35Q2GA" and mixes 2112/2176) around the real, proven MT29F2G01; correct the
  Winbond-flavored feature-register notes (no QE bit; SET FEATURE needs no WRITE
  ENABLE on this part); document ECC modes, the S3 env, and level-shifter wiring.
- **`CONTRIBUTING.md` (new):** "add your chip in 6 lines" — the `chips.yml`
  schema, how to read the JEDEC ID off the serial log, and the PR flow.

## 13. Open Questions

- **DS35Q1GA JEDEC `mfr_id`/`dev_id`** — placeholders in §3.1; confirm from
  silicon (`9Fh` on the serial log) or the DS35 datasheet before shipping that
  registry entry.
- **S3 pins assume a WROOM-1-class module** (GPIO9-14 free). If the actual S3
  board routes any of GPIO9-14 elsewhere, adjust `board_pins.h`; the constraint
  (avoid GPIO26-37 flash/PSRAM, strapping, USB, UART0) is what matters.

*Resolved:* ESP32-S3 SPI pins finalized to the FSPI IOMUX set GPIO9-14 from the
S3 datasheet (§5).

## 14. Working Directory Convention

`lab/` at repo root is the scratch playground for temporary scripts, throwaway
tests, and sample dumps. It lives in-repo (survives reboot, unlike `/tmp`) and is
**gitignored** — nothing there is committed. Work that proves useful graduates
into `tools/`, `test/`, or `src/`.

## 15. Build Order (for the implementation plan)

1. `nand_chips.h` + `chips.yml` + `tools/gen_chips.py` + generated header (+ tests).
2. `board_pins.h` + `platformio.ini` dual env; per-target host (`SPI3_HOST`
   classic / `SPI2_HOST` S3), avoiding SPI0/1 and reserved flash/PSRAM pins.
3. Driver refactor: runtime geometry, `uint32_t` row, ECC toggle, ecc-status
   mask, verify fix (+ native tests for pure logic).
4. Boot/detect/menu restructure in `main.cpp`.
5. Quad self-test + fallback.
6. Geometry header in firmware + `wifi_transport`.
7. `dump.py` header/metadata; `ecc_stripper.py` parametrization (+ host tests).
8. README + CONTRIBUTING.
9. Bench verification pass (hardware).
