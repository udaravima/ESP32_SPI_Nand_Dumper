# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Vendor/family profile architecture — Stage 1 (host foundation).** A data-driven
  `family → profile → chip` model that groups vendor-specific quirks (ECC-status
  encoding, OOB layout, Quad-Enable bit, read-ID method) instead of hardcoding one
  vendor's assumptions across the codebase. This stage lands **host-side and additive**:
  the firmware still auto-detects chips via `chips.yml` → `CHIPS[]`, and device
  behavior is unchanged (verified by an `esp32dev` firmware build) until Stage 2 wires
  the device onto it.
  - `db/{families,profiles,chips}/*.yml` — the three-layer database. The two existing
    chips are migrated in; `chips.yml` stays the live device path this stage.
  - `tools/chipdb.py` — load → fail-closed validate → flatten → **byte-identical
    110-byte pack** → JEDEC-ID collision disambiguation ladder. Named ECC `scheme`s
    (generic2, micron3, gd_uc, xtx4, xtx_g0xa) expand to a full 16-entry severity map,
    where any unnamed field value decodes to *uncorrectable* (fail-closed).
  - `src/nand_profile.h` / `.cpp` — the flat `active_profile_t` wire struct
    (`static_assert`-pinned at 110 bytes) and a pure, data-driven ECC-severity decoder
    that replaces per-vendor `if` ladders with one indexed lookup.
  - `tools/gen_profiles.py` — emits a resident `PROFILES[]` C header and a golden test
    blob from the *same* flattener (a second PlatformIO pre-hook), so a resident chip
    and a future host-pushed chip are byte-identical by construction.
  - A native golden-blob cross-check (`test/test_profile/`) proves the Python packer and
    the C struct agree byte-for-byte; `tests/test_chipdb.py` covers the host database.
  - Design: `docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md`
    and the Stage-1 plan under `docs/superpowers/plans/`.

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
- **Reserved Micron ECCS field values now fail closed.** On the profile ECC path, the
  `micron3` scheme's datasheet-reserved status-field values (4, 6, 7) decode to
  `NAND_SEV_UNCORRECTABLE` instead of being silently ignored by the legacy decoder — a
  conservative, safe-direction change now reachable on real hardware since Micron is a
  resident profile.

## [3.1.1] - 2026-08-17

### Fixed
- **Boot-loop crash when WiFi fails to connect.** `loop()` dereferenced a null TCP
  server after a failed `wifi_transport_init` (much more reachable since v3.1.0
  made WiFi credentials empty by default). Guarded the null server, and `setup()`
  no longer dead-ends: press `'M'` to enter credentials and it retries live.
- **ECC-on dumps now flag uncorrectable pages.** The firmware reads the on-die ECC
  status (ECCS) after each page when ECC is enabled and reports uncorrectable /
  refresh-recommended counts (and the first 20 uncorrectable page indices). A CRC
  over corrected-or-not data cannot reveal an uncorrectable page, so without this a
  damaged page passed silently. Decoding is per datasheet Table 9.

## [3.1.0] - 2026-08-17

Integrity, persistence, and portability. Verified on real MT29F2G01 silicon plus
the automated suites (host pytest + native Unity, dual-target compilation).

### Added
- **Per-page CRC32 integrity (wire proto v2).** Each page is now sealed with a
  CRC32 the PC re-checks; failures are pinned to a page index in a
  `<dump>.badpages.json` sidecar (data still written, so majority vote keeps every
  byte). Turns a silently-corrupt dump — the `dump2` failure mode — into a loud,
  localized one. `dump.py` still accepts v1 dumps.
- **Persistent settings.** The ESP32 saves its config (WiFi creds, port, SPI
  clock, read mode, verify, ECC, retries, batch size) to NVS on every menu exit and
  reloads it on boot — geometry stays auto-detected. `dump.py` gains `--ip`/`--port`/
  `--out-dir`, remembered in a local (gitignored) `dump.config.json` so flags are
  needed only once. A `config_validate()` clamp guards against corrupt NVS records.
- **Runtime capability report + self-sizing.** At boot the firmware prints the
  chip model, cores, clock, free heap/PSRAM, and flash — all queried live, so the
  same binary runs on any ESP32 variant without board edits. The send buffer is
  sized from the detected free heap.
- **Throughput knob (`[B]` batch pages/write).** Coalesces N page-frames into one
  TCP write to cut per-write overhead; runtime-selectable so you can benchmark and
  pick the best for your link, auto-clamped to available memory.
- **`verify_dump.py`.** CRC-verdict health report for a dump, plus CRC-aware
  cross-dump repair: each page is taken from a copy that passed its CRC, with
  majority voting only where every dump flagged the page bad.
- **Re-dumpable serve loop** — the dumper serves each new client without an ESP
  reset between dumps; `'M'` in the serial console re-opens the config menu live.
- **`dump.py` progress** now shows elapsed time and ETA, and the summary reports
  average throughput.

### Documentation
- Hardware **write-protection** guidance (CS# pull-up, WP# tie for single mode)
  added to the User Guide.

### Security
- **WiFi credentials no longer ship in source** — compiled defaults are empty;
  creds are entered once via the serial menu and persisted to NVS. (Prior
  hardcoded credentials remain in git history; rotate that WiFi password to fully
  retire them.)

### Changed
- Wire protocol version bumped `1` → `2`; header flag bit3 signals per-page CRC.

### Fixed
- **SPI clock is now actually applied.** The menu-selected clock was set but never
  pushed to the SPI device, pinning every dump to 1 MHz (~0.05 MB/s). Added
  `nand_set_clock()`; a 10 MHz dump is now bit-identical across runs and ~20–40×
  faster.
- **Graceful mid-dump disconnect.** `cmd_dump` now checks each send and aborts
  cleanly back to the wait state instead of streaming into a dead socket.

## [3.0.0] - 2026-08-16

Chip-agnostic release. Auto-detects the SPI NAND chip, adds ESP32-S3 support, and
fixes the bugs that corrupted 2 Gbit dumps.

> Verified via the automated test suites (host pytest + native Unity) and
> dual-target compilation. Hardware bench verification on real silicon is still
> pending (Task 14 of the implementation plan).

### Added
- JEDEC-ID **chip auto-detection** from a community-editable `chips.yml` registry,
  compiled to a C table by a build-time hook (`tools/gen_chips.py`) with schema
  validation.
- **ECC on/off toggle** in the config menu (default OFF/raw), recorded in the dump
  metadata.
- **32-byte geometry handshake header** — `dump.py` and `ecc_stripper.py`
  self-configure from it, so page-size mismatches can no longer occur.
- **Quad (x4) self-test** at boot with automatic fallback to single x1.
- **ESP32-S3** build target alongside ESP32-classic; per-target pins in
  `board_pins.h`.
- `.meta.json` sidecar written next to each dump.
- Documentation: Quick Start, User, and Developer guides, plus CONTRIBUTING.
- Native (host) unit tests and a Python test suite.

### Changed
- Serial menu (v3.0) shows the detected chip and pre-fills geometry; geometry,
  ECC, and read mode are runtime-configurable.
- `ecc_stripper.py` is parameterized by geometry (from the sidecar or flags)
  instead of hard-coded 2112-byte pages / 1024 blocks.
- `dump.py` derives the total size from the header; its network flow moved under
  `main()` so the module is importable/testable.
- Feature-register handling corrected to Micron semantics (B0h is the
  Configuration register, `ECC_EN` = bit 4; no QE bit).

### Fixed
- **Row-address overflow** — row addresses widened from `uint16_t` to `uint32_t`,
  so 2 Gbit chips read every block instead of aliasing the upper half into the
  lower.
- **Quad init abort** — removed the bogus QE-bit write that made selecting quad
  fail initialization on Micron parts.
- **Verify-buffer overflow** when the page size exceeded 2176 with verify on.
- **ECC status** now read as the full 3-bit field (was masking only 2 bits).
- **Verify with 0 retries** no longer falsely fails good pages.
- **CRC32 polynomial** in the wire header corrected (`0xEDB88420` → `0xEDB88320`)
  so it matches Python `zlib.crc32`.

### Removed
- `src/config.yml` (superseded by `chips.yml`) and the obsolete `src/quad.cpp.bk`.

### Hardware
- Datasheet-verified ESP32-S3 SPI pins (GPIO9–14, FSPI IOMUX) chosen to avoid the
  SoC's own flash/PSRAM pins and all strapping/USB/UART pins.
