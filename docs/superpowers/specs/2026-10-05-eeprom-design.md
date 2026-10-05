# Serial EEPROM support (chip family 3)

Date: 2026-10-05. Builds on the SPI NOR design
(`2026-10-05-spi-nor-design.md`) and the vendor-profile architecture.

## 1. Goal and scope

Read (dump) I2C serial EEPROMs (24xx: 24C01 .. 24CM02) and SPI serial
EEPROMs and FRAMs (25xx, M95, AT25, MB85RS/FM25) with the same board, clip,
session and host tools. Read only. Out of scope: Microwire 93Cxx (three-wire,
odd bit framing, 8/16-bit organisation pin), SPD5 hubs, writing.

## 2. Chip data

- `db/chips/eeprom/i2c-24xx.yml` (13 parts) and `spi-25xx.yml` (16 parts),
  written by hand. Sizes and 8/16-bit addressing come from the Linux at24
  driver's chip table (`drivers/nvmem/at24.c`, GPL-2.0-or-later, as facts,
  with a link to the commit); the SPI address-width rules (8, 9 = A8 in the
  opcode, 16, 24 bits) from the Linux at25 binding; page-write sizes and the
  device-address bit layout from vendor datasheets. Each entry lists the
  common vendor names as `aliases`, so `--chip AT24C256` finds `24C256`.
- Families `i2c-eeprom` and `spi-eeprom`, one profile `eeprom-generic`.
- All 29 parts are resident (about 3.7 KB of flash).

## 3. Profile

Same 128-byte struct and schema v2. EEPROM profiles use `family` 2 (I2C) or
3 (SPI), carry no ID (`id_method none`, `id_n_bytes 0`) and reuse the NOR
framing: `page_size` is one 256-byte read unit (or the whole part when it is
smaller), `spare_size` 0, at most 16 units per "block". `addr_bytes` is the
word-address width (I2C 1-2, SPI 1-3). Two former pad bytes (123, 124) say
where address bits above it travel:

| part | addr_bytes | dev_addr_bits | dev_addr_shift | carried in |
|------|-----------:|--------------:|---------------:|------------|
| 24C04 / 08 / 16 | 1 | 1 / 2 / 3 | 0 | I2C device address P0..P2 |
| 24CM01 / 24CM02 | 2 | 1 / 2 | 0 | I2C device address A16..A17 |
| 24xx1025 | 2 | 1 | 2 | I2C device address bit 2 (B0) |
| 25xx040 | 1 | 1 | 3 | SPI opcode bit 3 (READ 0Bh) |

NAND and NOR profiles must keep these two bytes zero. `nand_profile_check`
and `chipdb._check_eeprom` refuse anything that writes (an SPI status-write
opcode, a quad opcode), an ID, a size outside 128 B .. 512 KiB or not a power
of two, and an address that cannot reach the whole part. The schema version
stays 2: NAND and NOR blobs are byte-identical, and older firmware refuses an
EEPROM blob as `E_STRUCTURE` (unknown family).

ID lookups (`nand_profile_find*`, `nand_profile_pick`, `chipdb.candidates`,
`shared_ids`) skip EEPROMs, so an empty bus reading 00/FF can never resolve
to one. They are found by name (`nand_profile_by_name`, `chipdb.resolve_name`).

## 4. One clip for every SOIC-8 part

A 24xx has the 25xx footprint, so in a clip wired for SPI its pins land on the
SPI lines. I2C mode frees the SPI bus and reuses them:

| pin | 24xx | SPI line | I2C mode |
|----:|------|----------|----------|
| 1 | A0 | CS | weak pull-up (also keeps any SPI part deselected) |
| 2 | A1 | D1 / MISO | weak pull-down |
| 3 | A2 | D2 / WP | weak pull-up (a 24xx1025 needs A2 high) |
| 5 | SDA | D0 / MOSI | I2C data |
| 6 | SCL | CLK | I2C clock |
| 7 | WP | D3 / HOLD | weak pull-up: the part is write-protected |

Only weak pulls touch the strap and WP pins, so an in-circuit part's own straps
win; the scan finds wherever it answers. SPI traffic cannot disturb a 24xx in
the clip: SPI mode 0 only changes MOSI while SCK is low, so it never forms an
I2C START or STOP.

## 5. Detection without an ID

At boot, if neither READ ID view matched and both look like an empty bus, the
firmware:

- reads the SPI status register (RDSR, 05h). A 25xx/FRAM reads its unused
  bits 6..4 as 0; 0xFF (MISO pulled up) means nothing answered;
- lends the pins to I2C and probes 0x50..0x57 with an address-only write
  (START, address + W, STOP: no data byte, so nothing can be written). The
  result is an 8-bit ACK mask.

If either finds something, the menu switches to that family and asks for the
part (**[P]**); a part picked before (NVS, keyed by family with mfr 0) is
taken again if it still answers. An I2C part "answers" when every address it
occupies acknowledged (`eeprom_i2c_base`: a 24C16 needs all eight, a 24C04 an
aligned pair); its base address is the lowest such set.

## 6. Read path

`src/eeprom_seq.*` is the pure sequencer (bus callbacks, tested against
`test/test_eeprom/sim_eeprom.h`); `src/eeprom_driver.*` binds it to the Arduino
`Wire` controller and to `nand_spi_xfer`.

- I2C: random read = START, device + W, word address, repeated START,
  device + R, data, NACK, STOP. The word address is never followed by a STOP
  or a data byte. Transactions are at most 128 bytes (the `Wire` buffer) and
  never cross the end of the word-address range, where the carried bits
  change. 100 or 400 kHz.
- SPI: 03h (0Bh for the upper half of a 25xx040) + address, at the menu's SPI
  clock (the firmware warns above 5 MHz).
- Each read unit is read twice and must agree (up to the retry limit), as for
  NAND/NOR. An I2C NACK fills the rest of the unit with 0xFF and counts as a
  failed unit.

## 7. Session v3 and the dump stream

- `INFO` grows from 38 to 40 bytes: `+ i2c_ack_mask, spi_ee_status`. Chip
  state 5 is `picked`.
- A pushed EEPROM profile is bound to the socket by presence instead of an ID:
  an I2C part must answer at all its addresses, a SPI part must return a
  plausible status; otherwise `E_ID_MISMATCH`. The echo's "detected ID" is the
  presence byte it compared (`[mask, 0, 0]` or `[status, 0, 0]`).
- `G` with an EEPROM family selected but no part picked or pushed is refused
  with `E_UNKNOWN_ID`.
- The dump header sets `DUMP_FLAG_EEPROM` (0x20), plus `DUMP_FLAG_I2C` (0x40)
  on I2C, where `mfr_id` is the device address the part answered at.
- `dump.py --chip` takes a name or an alias. An EEPROM pick is never cached
  in `dump.config.json` (it is not tied to an ID). With no `--chip`, `dump.py`
  prints which parts fit the I2C scan. `ecc_stripper.py` copies an EEPROM dump
  through unchanged.

## 8. Known risk: picking the wrong size

An EEPROM cannot report its size. Picking a 2-byte-address part for a
1-byte-address one (24C256 for a 24C02) makes the second address byte a data
byte in the part's page buffer. The firmware never ends that transaction with
a STOP (a repeated START follows), which is what commits a 24xx write, and in
the clip the part's WP pin is pulled high. The dump of a wrongly sized pick is
wrong (wrapped or short), not written. A read-only size probe is left for
later (for example, comparing current-address reads, which send no address at
all).

## 9. Testing without hardware

- `test/test_eeprom`: the sequencer against simulated 24xx parts (strap pins,
  carried bits, the 24xx1025 A2 rule, rollover, page-buffer bytes) and 25xx
  parts (A8 in the opcode, status, write opcodes counted), for every
  addressing scheme in the DB, plus noise, a vanished part and the profile
  checks.
- `test/test_profile`, `test/test_session`: golden EEPROM blobs and the
  presence binding.
- `tools/tests/test_chipdb_eeprom.py`, `tests/test_dump_session.py`: DB
  rules, aliases, the I2C base rule mirrored in Python, and dump.py against
  the real session C code with an I2C and a SPI EEPROM.
