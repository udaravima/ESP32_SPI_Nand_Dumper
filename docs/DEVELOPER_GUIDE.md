# Developer Guide

How the code is organized, how to build and test it, and how to extend it — add a
chip, add a board, or change the wire protocol. For operating the tool, see the
[User Guide](USER_GUIDE.md).

## Contents

- [Architecture](#architecture)
- [The chip registry and code generation](#the-chip-registry-and-code-generation)
- [Adding a chip](#adding-a-chip)
- [Adding a board](#adding-a-board)
- [The wire protocol](#the-wire-protocol)
- [Build system](#build-system)
- [Testing](#testing)
- [SPI / NAND reference](#spi--nand-reference)
- [Design docs](#design-docs)

## Architecture

The design deliberately isolates **pure logic** (no ESP-IDF/Arduino dependencies)
so it can be unit-tested on the host, from **hardware-coupled** code that can only
be verified on real silicon.

### Firmware (`src/`)

| File | Responsibility | Pure? |
|---|---|---|
| [`nand_profile.h`](../src/nand_profile.h) / [`.cpp`](../src/nand_profile.cpp) | `active_profile_t` (the flat profile); ECC severity decode; tier 2/3 checks; `PRF` blob unpack; ID resolution; bad-block marker | ✅ host-testable |
| `nand_profiles_generated.h` | Generated `NAND_RESIDENT[]` table from `db/` (do not edit) | ✅ data |
| [`nand_addr.h`](../src/nand_addr.h) | `nand_row_addr()` — 32-bit row from block/page | ✅ host-testable |
| [`dump_header.h`](../src/dump_header.h) / [`.cpp`](../src/dump_header.cpp) | 32-byte geometry header pack + CRC32 | ✅ host-testable |
| [`config_store.h`](../src/config_store.h) / [`.cpp`](../src/config_store.cpp) | `nand_app_config_t`; `config_defaults()` / `config_validate()` clamp | ✅ host-testable |
| [`config_nvs.cpp`](../src/config_nvs.cpp) | `config_load()` / `config_save()` — NVS blob via `Preferences` | ❌ hardware |
| [`sys_info.h`](../src/sys_info.h) / [`.cpp`](../src/sys_info.cpp) | `sys_recommend_batch_pages()` — memory-aware batch sizing | ✅ host-testable |
| [`sys_info_esp.cpp`](../src/sys_info_esp.cpp) | `sys_info_report()` / `sys_free_dma_bytes()` — runtime chip/heap query | ❌ hardware |
| [`board_pins.h`](../src/board_pins.h) | Per-target pins + `NAND_SPI_HOST` | — macros |
| [`nand_seq.h`](../src/nand_seq.h) / [`.cpp`](../src/nand_seq.cpp) | Read-path sequencing (PAGE READ, READ FROM CACHE, READ ID, plane-select column) over a bus callback; verified read with quad → single fallback | ✅ host-testable |
| [`nand_driver.h`](../src/nand_driver.h) / [`.cpp`](../src/nand_driver.cpp) | SPI transactions: reset, feature regs, page read, cache read, ECC toggle, verify, quad self-test | ❌ hardware |
| [`wifi_transport.h`](../src/wifi_transport.h) / [`.cpp`](../src/wifi_transport.cpp) | WiFi connect + TCP stream | ❌ hardware |
| [`main.cpp`](../src/main.cpp) | Boot flow, config menu, dump loop, header emission | ❌ hardware |

**Boot flow** (`main.cpp` → `setup()`): bring SPI up at 1 MHz → read `9Fh` →
`nand_profile_find` in the resident table → copy it into the active profile and
pre-fill geometry (unknown chip: a manual profile) → run the menu → apply the
profile, ECC and read mode →
(if quad) self-test with fallback → allocate the page buffer to the real page
size → WiFi → send the geometry header → stream pages.

### Host tools

| File | Responsibility |
|---|---|
| [`dump.py`](../dump.py) | Receive the stream; `parse_header`, `recv_exact`, `write_metadata`; network flow under `main()` |
| [`ecc_stripper.py`](../ecc_stripper.py) | `strip()` spare/OOB → main-area image; `load_geometry()` from the sidecar |
| [`verify_dump.py`](../verify_dump.py) | CRC-verdict health report + CRC-aware cross-dump repair (`choose_page_sources`, `majority_bytes`) |
| [`tools/binary_compare_fix.py`](../tools/binary_compare_fix.py) | Majority-vote repair across multiple dumps (byte-level, no CRC verdicts) |
| [`tools/chipdb.py`](../tools/chipdb.py) | Load, validate, resolve and flatten `db/`; pack the `PRF` blob |
| [`tools/gen_profiles.py`](../tools/gen_profiles.py) | Emit the resident C table (and, with `--golden`, the test blobs) from `db/` |
| [`tools/chips_yml_to_db.py`](../tools/chips_yml_to_db.py) | One-shot converter from a v3 `chips.yml` |

The Python entry points all guard their side effects behind `if __name__ ==
"__main__"` / `main()`, so they import cleanly for testing.

## The chip database and code generation

[`db/`](../db/README.md) is the human-editable source of truth: families,
vendor profiles and chips (design:
`docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md`).
[`tools/chipdb.py`](../tools/chipdb.py) is the only place the layers are
resolved; it flattens each chip into the 120-byte `active_profile_t` the device
runs on. [`tools/gen_profiles.py`](../tools/gen_profiles.py) writes every
`resident: true` chip into `src/nand_profiles_generated.h`. It runs:

- **As a PlatformIO pre-hook** — `extra_scripts = pre:tools/gen_profiles.py` in
  `platformio.ini` regenerates the header on every build (auto-installing PyYAML
  into PlatformIO's Python if missing). A chip that fails validation fails the
  build.
- **Standalone** — `python tools/gen_profiles.py`, and `--golden` for
  `test/test_profile/golden_blobs.h`.

The generated headers are committed, so a fresh clone builds even before the
hook runs; CI fails if they drift from `db/`.

**The seam that matters** is the byte layout: the Python `LAYOUT` table in
`chipdb.py` and the C struct in `nand_profile.h` must agree. The C header
`static_assert`s its size and key offsets, and the native golden-blob test
unpacks the blobs Python packed and checks every field. The device re-runs the
tier-2/3 checks (`nand_profile_check`) on the resident entry at boot.

## Adding a chip

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the full walkthrough. In short: read
the JEDEC id off the serial log, add `db/chips/<part>.yml` pointing at the right
vendor profile, run `python3 tools/chipdb.py` and `pio run`, verify a dump, open
a PR. The quad-enable bit, ECC decode and bad-block marker come from the profile.

## Adding a board

Board wiring lives in [`board_pins.h`](../src/board_pins.h), selected at compile
time by `CONFIG_IDF_TARGET_*`. To add a target:

1. Add a `#elif defined(CONFIG_IDF_TARGET_ESP32XX)` branch with the six pins and
   `NAND_SPI_HOST`.
2. Add an `[env:...]` block in `platformio.ini`.

**Pin rules — do not violate these:** never use the SoC's own flash/PSRAM SPI
(`SPI0`/`SPI1`). On ESP32-classic avoid GPIO6–11 (internal flash) and GPIO16/17
on PSRAM modules. On ESP32-S3 avoid GPIO26–37 (flash/PSRAM) plus strapping
(0/3/45/46), USB (19/20), and UART0 (43/44). Use a general-purpose GPSPI host
(`SPI2_HOST`/`SPI3_HOST`), never `SPI0/1`. A dumper runs ≤40 MHz, so routing
through the GPIO matrix instead of IOMUX is fine if you can't hit the IOMUX pins.

## The wire protocol

### Command session (v4)

Each TCP connection is a short command session
([`nand_session.h`](../src/nand_session.h)). A client may send:

| Command | Payload | Reply |
|---|---|---|
| `'I'` | none | info: session ver u8, schema ver u8, max page u32, detected ID[3], state u8 (0 resident, 1 unknown, 2 ambiguous, 3 pushed), active profile name[24] |
| `'P'` | a 130-byte `PRF` blob (`tools/chipdb.py --blob`) | echo: name[24], page/spare/ppb/blocks u32, planes u8, expected ID[3] + id_flags, detected ID[3], blob CRC32 |
| `'A'` | the staged blob's CRC32 (u32) | empty |
| `'G'` | none | the 32-byte dump header and the page stream (below) |

Every reply to `I`/`P`/`A`, and a refused `G`, is one frame:
`"NRSP" | cmd u8 | status u8 | len u16 | payload | CRC32`, where `status` is a
`nand_prf_err_t` (`0` = OK) and the CRC covers everything before it.

A push is **fail-closed and two-phase**. `P` runs the framing, structure and
geometry checks, then the ID cross-check against the chip the device read. Only
then is the profile *staged*, and the device echoes it back. `dump.py` compares
the echo field by field with what it sent, and only then sends `A` with the
blob's CRC, which makes the staged profile live. A failed push discards the
profile and anything staged before it; an `A` with the wrong CRC disarms; a `G`
while a pushed profile is still unarmed is refused rather than dumping on the
old profile. A framing header the firmware doesn't understand (other magic,
schema version or length) closes the connection, since the stream can't be
resynced without guessing a length. A staged profile never outlives its
connection; an armed one stays active until reboot.

A bare `'G'` behaves exactly as before v4, so older clients still work. Pre-v4
firmware ignores `'I'`; `dump.py` treats three seconds of silence as that and
falls back to a bare `G`.

### Dump stream

After the client sends the `'G'` trigger, the firmware sends a **32-byte
little-endian header**, then the page stream.

| Offset | Size | Field |
|---|---|---|
| 0 | 6 | magic `"NANDMP"` |
| 6 | 1 | proto version (`2`) |
| 7 | 1 | flags: bit0 ECC on, bit1 quad, bit2 verify, bit3 per-page CRC |
| 8 | 2 | page_size |
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

The `flags` field reflects the **actual** run — if quad fell back to single, the
quad bit is 0. The CRC32 is the standard reflected polynomial (`0xEDB88320`), so
it matches Python `zlib.crc32`.

**Page framing (proto v2).** With the per-page-CRC flag set (bit3, always on in
current firmware), each page is sent as `[page_size data bytes][4-byte CRC32 of
that data, little-endian]` in a single write. `dump.py` re-computes the CRC per
page: on a mismatch it records the page index in a `<dump>.badpages.json` sidecar
but **still writes the data**, so a later majority vote across dumps keeps every
byte. This seals the ESP→PC path — a garbled or truncated page is caught and
pinned to its index instead of passing silently as it did in the `dump2` incident.
It does *not* catch corruption on the chip→ESP read (a CRC over garbage is still a
valid CRC); on-die ECC and its status bits guard that half. A v1 dump — a raw,
unframed byte stream — is still accepted by `dump.py` for backward compatibility.

**The header is packed in C** ([`dump_header.cpp`](../src/dump_header.cpp)) **and
parsed in Python** ([`dump.py`](../dump.py)). Because two implementations must
agree byte-for-byte, [`tests/test_dump_header.py`](../tests/test_dump_header.py)
pins the exact 32-byte image as a golden vector — this is what caught a wrong CRC
polynomial that both sides' self-consistent tests had happily accepted. If you
change the layout, update the C packer, the Python parser, and the golden bytes
together.

## Build system

`platformio.ini` defines three environments:

| Env | Purpose |
|---|---|
| `esp32dev` | ESP32-classic firmware |
| `esp32-s3-devkitc-1` | ESP32-S3 firmware |
| `native` | Host Unity tests for the pure modules |

The `native` env needs two non-obvious settings: `test_build_src = yes` (so
`pio test` compiles `src/`), and `build_src_filter = -<*> +<nand_profile.cpp>
+<dump_header.cpp> ...` (so only the ESP-IDF-free modules compile on the host — the
Arduino sources would not build natively). `framework = arduino` is set per-board,
not in `[env]`, so the native env doesn't inherit it.

## Testing

```bash
pip install -r requirements-dev.txt   # pyyaml, pytest
python3 -m pytest                     # host tools + wire-format cross-check
pio test -e native                    # pure C logic, golden profile blobs, simulated NAND
```

What each layer covers:

- **`python -m pytest`** — the generator's validation, `ecc_stripper` geometry
  handling, `binary_compare_fix` majority vote, the wire header round-trip and
  golden bytes, and `dump.py` header parsing / metadata.
- **`pio test -e native`** — `nand_chip_lookup`, `nand_row_addr` (including the
  regression test that block 1024 does not alias to row 0), and header pack/CRC.
  `test/test_sim` runs the real read-path sequencer (`nand_seq`) against a
  simulated multi-plane NAND (`sim_nand.h`, one cache register per plane), so
  plane-select and addressing mistakes fail a test instead of corrupting dumps.
- **Bench (manual, real hardware)** — SPI transactions, ECC on/off on the array,
  the quad self-test on real wiring, WiFi, and a full end-to-end dump. These
  cannot be unit-tested; the bench checklist is Task 14 of the implementation
  plan (see below).

The approach is TDD for everything host-testable: write the failing test, watch
it fail, implement, watch it pass. A local Python venv (e.g. under the gitignored
`lab/`) keeps pytest off the system interpreter.

## SPI / NAND reference

**Row address:** `row = (block << page_addr_bits) | (page & (pages_per_block-1))`,
computed as `uint32_t` — a 2 Gbit chip needs 17 bits, so the old `uint16_t`
overflowed and aliased the upper half of the array. See
[`nand_addr.h`](../src/nand_addr.h).

**Command set (MT29F2G01):** `FFh` reset, `13h` page-read to cache, `0Bh` read
cache (single), `6Bh` read cache (quad 1-1-4), `0Fh`/`1Fh` get/set feature,
`9Fh` read id. Feature registers: `A0h` block lock (PROGRAM/ERASE only; reads are
always allowed), `B0h` **configuration** (`ECC_EN` = bit 4; **no QE bit** on
Micron), `C0h` status (`OIP`, `WEL`, `E_Fail`, `P_Fail`, and a **3-bit** ECC
status in bits 4–6). `SET FEATURE` needs no `WRITE ENABLE`.

The full datasheets are in [`datasheets/`](datasheets/) (kept local, out of git
via a `*` ignore because of their size).

## Design docs

The reasoning behind these decisions is captured in:

- Spec: [`superpowers/specs/2026-08-15-nand-dumper-generalization-design.md`](superpowers/specs/2026-08-15-nand-dumper-generalization-design.md)
- Plan: [`superpowers/plans/2026-08-15-nand-dumper-generalization.md`](superpowers/plans/2026-08-15-nand-dumper-generalization.md) (Task 14 is the hardware bench checklist)
