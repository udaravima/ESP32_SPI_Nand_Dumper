# SPI NOR support (chip family 2)

Date: 2026-10-05. Builds on the vendor-profile architecture
(`2026-08-23-vendor-profile-architecture-design.md`, stages 1 to 3).

## 1. Goal and scope

Read (dump) SPI NOR flash with the same board, wiring, session and host tools
as SPI NAND. Read only: the firmware never programs, erases or writes a status
register on a NOR part. Out of scope: dual/octal I/O, DTR, bank/extended
address registers (EAR), 2-byte-address parts, writing the QE bit.

## 2. Chip data

- Seeded from flashrom's chip tables (`flashchips/*.c`, GPL-2.0-or-later) by
  `tools/import_flashrom.py`. Only facts are imported (name, JEDEC ID, size,
  voltage range, 4-byte addressing features, whether flashrom reports a
  successful read), each entry with a link to the flashrom file and commit it
  came from. No flashrom code is copied.
- Kept: `PROBE_SPI_RDID` chips read with `SPI_CHIP_READ` that have a plain
  3-byte ID, a power-of-two size of at least 4 KiB, and (above 16 MiB) a
  4-byte read opcode set or B7h entry. Entries that share (mfr, model, size)
  are merged into one record and the other names become `aliases`.
- A chip is `resident` (compiled into the firmware) when flashrom reports a
  successful read and its supply range includes 3.3 V. At import: 366 chips,
  186 resident, one shared ID (W77Q16JW/W77Q32JW: same ID, different size).
- Output: `db/chips/spi-nor/flashrom-<vendor>.yml`, regenerated wholesale. A
  chip written by hand anywhere else under `db/chips/` with the same ID wins
  over the imported one.
- `db/families/spi-nor.yml` holds the JEDEC opcodes; `db/profiles/nor-*.yml`
  holds the per-vendor quad-enable requirement (QER, JESD216 codes):
  Winbond 5, Macronix 2, GigaDevice 5, everyone else 0 (unknown).

## 3. Profile (schema v2, 128 bytes)

One flat `active_profile_t` serves both families. v2 adds:

| offset | field        | meaning                                              |
|-------:|--------------|------------------------------------------------------|
| 77     | `family`     | 0 SPI NAND, 1 SPI NOR (was padding in v1)            |
| 118    | `addr_bytes` | NOR: 3 or 4                                          |
| 119    | `addr4_mode` | 0 none, 1 native (13h/6Ch), 2 B7h, 3 WREN + B7h      |
| 120    | `dummy_x1`   | dummy cycles of the single read (0 for 03h)          |
| 121    | `dummy_x4`   | dummy cycles of the 1-1-4 read (8 for 6Bh)           |
| 122    | `qer`        | JESD216 quad-enable requirement (0..6)               |
| 123    | `_pad1[5]`   | must be zero                                         |

For a NOR profile the geometry fields are reused: `page_size` is a 4 KiB read
unit, `spare_size` is 0, `pages_per_block` is at most 16, and `total_blocks`
makes up the capacity. All NAND-only fields (ECC, BBM, feature registers,
plane bits) must be zero, and the NOR tail must be zero on a NAND profile;
`nand_profile_check` and `chipdb.check_flat` enforce both. The ID is matched
with `id_method none` (plain 9Fh) and 3 bytes, the third being the capacity
byte (`id_dev2`, `HAS_DEV2`).

`nand_profile_find` changed meaning slightly: an entry that declares `dev2`
is only a candidate when `dev2` matches. Many W25Q parts share `EF 40`, and
without this a W25Q256 would resolve to the W25Q128 entry.

## 4. Telling NAND from NOR

The same 9Fh opcode means different things: SPI NAND wants one dummy/address
byte after it, SPI NOR answers straight away. The firmware reads both views at
boot (NOR view after ABh, release from deep power-down) and
`chip_detect_resident` resolves each view only against its own family. If both
views match a resident chip the result is ambiguous and nothing is guessed
(fail closed). The host's `resolve_profile` applies the same rule to the DB.

A NOR ID that matches nothing but looks real (manufacturer byte not 00/FF, not
a flat bus) falls through to SFDP.

## 5. SFDP fallback

`sfdp_probe` reads the JESD216 header and the newest Basic Flash Parameter
Table (5Ah, 3-byte address, 8 dummy cycles) and takes density, address mode,
the 1-1-4 opcode and dummy count, QER (DWORD15) and the 4-byte entry methods
(DWORD16). `sfdp_build_profile` turns that into a profile named
`SFDP-XXYYZZ` (chip state `sfdp`, 4), preferring B7h, then WREN + B7h, then
the native 4-byte opcodes. It refuses 4-byte-only parts and parts above
16 MiB that only offer EAR, and the result must pass `nand_profile_check`.

## 6. Read path

`src/nor_seq.*` is the pure sequencer (bus callback, testable against
`test/test_nor/sim_nor.h`); `src/nor_driver.*` binds it to the shared SPI bus
through `nand_spi_xfer`. A dump is bracketed by `nor_begin`/`nor_end`, which
enter and leave 4-byte mode on B7h parts so the chip ends in its power-on
state. Reads use the same verify-and-fall-back policy as NAND: two reads of
each 4 KiB unit must agree, and a quad read that never settles is redone
single; after `NAND_QUAD_FALLBACK_LIMIT` such units the session stays single.

Quad is opt-in. Because QE is never written, the firmware reads the QE bit
back (per QER) and runs a self-test; with QE clear it explains why and stays
single. WiFi is the bottleneck anyway.

## 7. Session v2 and the dump stream

- `INFO` grows from 34 to 38 bytes: `+ nor_id[3]` (the NOR view) and the
  active profile's family. `ECHO` grows from 52 to 53: `+ family`. The ID
  cross-check and the echoed detected ID use the pushed profile's family view.
  `dump.py` still accepts v1 replies (SPI NAND only).
- The dump header sets `DUMP_FLAG_NOR` (0x10); mfr/dev come from the NOR view.
- `dump.config.json` remembers a NOR choice under `NOR:MF:DV:CP` (the capacity
  byte is part of the key); SPI NAND keeps `MF:DV`.
- `ecc_stripper.py` copies a NOR dump through unchanged (there is no spare).

## 8. Testing without hardware

- `test/test_nor`: the sequencer and SFDP parser against a simulated NOR chip
  that checks the protocol, counts any write command, gates quad on QE and
  serves synthetic SFDP tables.
- `test/test_profile`, `test/test_session`: v2 layout, golden NOR blobs,
  family-aware find and cross-check.
- `tests/test_dump_session.py`: v2 INFO/ECHO, NOR resolution, cross-family
  ambiguity, and a push against the real session C code with a NOR ID.
- `tools/tests/test_import_flashrom.py`: the importer against a small
  synthetic flashrom tree.
- The ESP32 firmware itself is built in CI.
