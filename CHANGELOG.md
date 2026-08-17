# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

Post-3.0 work, verified on real MT29F2G01 silicon plus the automated suites.

### Added
- **Per-page CRC32 integrity (wire proto v2).** Each page is now sealed with a
  CRC32 the PC re-checks; failures are pinned to a page index in a
  `<dump>.badpages.json` sidecar (data still written, so majority vote keeps every
  byte). Turns a silently-corrupt dump — the `dump2` failure mode — into a loud,
  localized one. `dump.py` still accepts v1 dumps.
- **Persistent settings.** The ESP32 saves its config (WiFi creds, port, SPI
  clock, read mode, verify, ECC, retries) to NVS on every menu exit and reloads it
  on boot — geometry stays auto-detected. `dump.py` gains `--ip`/`--port`/
  `--out-dir`, remembered in a local (gitignored) `dump.config.json` so flags are
  needed only once. A `config_validate()` clamp guards against corrupt NVS records.
- **Re-dumpable serve loop** — the dumper serves each new client without an ESP
  reset between dumps; `'M'` in the serial console re-opens the config menu live.

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
