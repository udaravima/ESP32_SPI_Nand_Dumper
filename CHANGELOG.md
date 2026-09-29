# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- **Odd blocks on 2-plane chips were read from the wrong plane.** The READ FROM
  CACHE column address never carried the plane-select bit (bit 12 for a 2048-byte
  page; block bit RA6 selects the plane per the Micron M79A datasheet). On the
  2-plane MT29F2G01, every page of each odd block is expected to read back as the last page of
  the preceding even block. The dump was consistent across runs, so CRC and
  verify could not catch it. `chips.yml` gains an optional `planes` field
  (MT29F2G01 = 2), and the driver now sets the plane bit on every cache read.

### Changed
- **Breaking: the firmware now builds from the v4 chip database (vendor-profile
  stage 2).** `chips.yml`, `tools/gen_chips.py` and `nand_chips.*` are gone. The
  build flattens every `resident: true` chip in `db/` into the C
  `active_profile_t` table (`tools/gen_profiles.py`), and the read path runs
  against one active profile: opcodes, feature addresses, the ECC-enable bit,
  the ECC status decode, the quad-enable bit and the bad-block marker all come
  from it. `tools/chips_yml_to_db.py` converts a custom v3 `chips.yml`.
- ID detection reads three bytes and resolves them in the resident table,
  using `dev2` when chips share an ID and refusing to guess otherwise.

### Fixed
- **DS35: ECC status decoded with Micron's field.** The Dosilicon profile
  decodes the 2-bit `SR[5:4]` field, so a status such as `0x50` is "corrected",
  not Micron's "7-8 bits corrected, refresh".
- **DS35: quad-enable bit never set.** Quad mode now sets `B0h` bit 0 from the
  profile before the self-test.
- Factory bad-block markers are counted during raw dumps, using the profile's
  marker offset, width and page (the DS35's marker is 2 bytes).

### Added
- **On-device quad → single fallback.** With verify on, a page whose quad reads
  never agree is re-read single from the same cache load. After three such
  pages the rest of the dump runs single; the next dump tries quad again.
- Native golden-blob test: the C side unpacks the `PRF` blobs `chipdb.py`
  packs and checks every field, and the resident table equals the pushed bytes.
- Simulated multi-plane NAND for native tests (`test/test_sim`). The read-path
  command sequencing moved into the hardware-free `nand_seq` module so the same
  code runs on the device and against the simulator.
- **v4 chip database, stage 1 (host only).** `db/` holds the family → profile →
  chip model, and `tools/chipdb.py` validates it, resolves each chip to the flat
  device profile, expands ECC schemes to a 16-entry map, disambiguates shared
  JEDEC IDs without guessing, and packs the fail-closed `PRF` push blob. The
  firmware still builds from `chips.yml`, and a test keeps the two in agreement.
- GitHub Actions CI: pytest, native Unity tests, firmware builds for both
  targets, and a generated-chip-table drift check on every PR.
- `tools/check_planes.py` finds that signature in existing dumps so affected
  ones can be re-taken.

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
