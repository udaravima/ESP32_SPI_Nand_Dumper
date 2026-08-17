# User Guide

Everything you need to operate the dumper: wiring, the config menu, ECC and read
modes, the dump workflow, post-processing, and troubleshooting. If you just want
a first dump fast, start with the [Quick Start](QUICKSTART.md). To modify the
firmware or add a chip, see the [Developer Guide](DEVELOPER_GUIDE.md).

## Contents

- [What this tool does](#what-this-tool-does)
- [Hardware](#hardware)
- [Building and flashing](#building-and-flashing)
- [The config menu](#the-config-menu)
- [ECC: raw vs corrected](#ecc-raw-vs-corrected)
- [Read modes: single and quad](#read-modes-single-and-quad)
- [Running a dump](#running-a-dump)
- [The metadata sidecar](#the-metadata-sidecar)
- [Post-processing](#post-processing)
- [Repairing dumps by majority vote](#repairing-dumps-by-majority-vote)
- [Understanding the raw layout](#understanding-the-raw-layout)
- [Troubleshooting](#troubleshooting)

## What this tool does

It reads a SPI NAND flash chip page by page and streams the raw contents over
WiFi/TCP to your PC. It **auto-detects** the chip from its JEDEC ID, so geometry
(page size, block count, spare size) is filled in for you. The raw stream
includes each page's spare/OOB area; a post-processing step strips that to a
clean firmware image.

The tool is read-only — it never programs or erases the chip.

## Hardware

ESP32 I/O is **3.3 V**. A 3.3 V NAND wires directly. A **1.8 V** part (e.g. the
Micron `MT29F2G01ABBGD` variant) must go through a level shifter on every signal
line — do not connect it directly.

### Wiring

| NAND pin | Function | ESP32-classic | ESP32-S3 |
|---|---|---|---|
| CLK | SPI clock | GPIO 18 | GPIO 12 |
| DI / SIO0 | MOSI / data 0 | GPIO 23 | GPIO 11 |
| DO / SIO1 | MISO / data 1 | GPIO 19 | GPIO 13 |
| WP# / SIO2 | data 2 (quad only) | GPIO 22 | GPIO 14 |
| HOLD# / SIO3 | data 3 (quad only) | GPIO 21 | GPIO 9 |
| CS# | chip select | GPIO 5 | GPIO 10 |
| VCC | power | 3V3 | 3V3 |
| GND | ground | GND | GND |

The pins are defined in [../src/board_pins.h](../src/board_pins.h). Classic uses
the VSPI (SPI3) IOMUX pins; the S3 uses the FSPI (SPI2) IOMUX pins on GPIO9–14,
chosen to avoid the S3's internal flash/PSRAM pins.

### Signal integrity

Long breadboard wires are the #1 cause of read errors at higher clocks. Keep them
short, add a 100 nF decoupling capacitor across VCC/GND near the chip, and if you
see mismatches drop the SPI clock (the menu defaults to a safe 1 MHz).

### Write protection (belt and suspenders)

The firmware is **provably read-only** — it issues only read/reset/feature opcodes
and never sends WRITE ENABLE (06h), PROGRAM, or ERASE — and the chip powers up with
all blocks locked (BP bits set, WEL = 0). So the software alone cannot modify the
chip. If you want hardware to back that up:

- **10 kΩ pull-up on CS# to 3V3 (recommended).** During ESP32 boot/reset the GPIOs
  float briefly; a pull-up keeps the chip *deselected* through that window, so no
  bus noise can be latched as a command. GPIO5 is a strapping pin (often already
  pulled up) but an explicit resistor makes it certain.
- **Tie WP# low to freeze the block-lock bits (single mode only).** Per the
  datasheet, WP# low provides "hardware write protection to freeze BP bits" — it
  stops any command from *unlocking* the already-locked blocks. **Caveat / trap:**
  WP# and HOLD# double as the SIO2/SIO3 data lines in **quad** mode, so this only
  applies when reading single-x1. In quad you cannot use WP# for protection — rely
  on the read-only firmware and the power-on lock instead.

Because the firmware sends no write/erase, these are extra insurance, not a
prerequisite for safe dumping.

## Building and flashing

```bash
pio run -e esp32dev -t upload             # ESP32-classic
pio run -e esp32-s3-devkitc-1 -t upload   # ESP32-S3
```

Each build regenerates the compiled chip table from `chips.yml` automatically, so
if you add a chip you just rebuild.

## The config menu

Open the serial monitor after flashing:

```bash
pio device monitor -b 115200
```

The menu shows the detected chip and lets you change settings without reflashing:

```
  ESP32 SPI NAND Dumper v3.0 — Config
  Detected: MT29F2G01ABAGD (0x2C 0x24)
  -- Network --
  [1] WiFi SSID / [2] Password / [3] TCP Port
  -- SPI --
  [4] SPI Clock / [5] Read Mode / [6] Verify / [7] Max Retries
  [B] Batch pages/write: 1 (per-page)
  [E] ECC on read:  OFF (raw)
  -- NAND Geometry --
  [8] Page Size / [9] Pages/Block / [0] Total Blocks
  [S] START
```

Settings are saved to the ESP32's NVS on **[S]**, so later boots skip re-entry.
At boot the firmware also prints a **runtime capability report** (chip model,
cores, clock, free heap/PSRAM, flash) — queried live, so the same binary adapts to
any ESP32 variant. **[B] Batch pages/write** is a throughput knob: it coalesces
that many page-frames into one TCP write (1 = per-page). Higher values cut
per-write overhead when WiFi is the bottleneck; the value is auto-clamped to what
the board's free memory can hold. Try a few and watch the MB/s to find your best.

| Option | What it controls |
|---|---|
| `1` `2` `3` | WiFi SSID, password, TCP port (default 3333) |
| `4` | SPI clock — presets 1/5/10/20/40 MHz or custom. **Lower it if you see retries.** |
| `5` | Read mode — Single x1 (reliable) or Quad x4 (faster; self-tested, see below) |
| `6` | Verify — read each page twice and compare |
| `7` | Max retries on a verify mismatch |
| `E` | ECC on read — OFF (raw) or ON (corrected); see next section |
| `8` `9` `0` | Geometry — page size, pages per block, total blocks (pre-filled from the detected chip; override for an unknown chip) |
| `S` | Start the dump with the shown settings |

If the chip isn't in the registry, the menu says `Detected: UNKNOWN` and you set
geometry manually with `8`/`9`/`0`. Better: add it to `chips.yml` (see the
[Developer Guide](DEVELOPER_GUIDE.md#adding-a-chip)).

## ECC: raw vs corrected

SPI NAND chips have an on-die ECC engine that corrects bit errors on read. It
powers up **enabled**, but this tool defaults it **OFF** for dumping. The choice
matters:

- **OFF (raw)** — the array is read with no correction; you capture the literal
  stored bits, including the ECC parity bytes in the spare area. This is
  **lossless and reversible**: you can run ECC correction offline afterward, or
  majority-vote across several dumps to repair transmission errors. Choose this
  for forensics, wear analysis, or when you don't trust the controller.
- **ON (corrected)** — the engine corrects bit errors and reports status; the
  data is cleaner and immediately mountable after stripping. But the raw parity
  is gone, and a page with *uncorrectable* errors is silently mis-corrected.
  Choose this when you just want the working filesystem.

The asymmetry is the reason for the OFF default: from a raw dump you can always
compute the corrected image, but from a corrected dump you can never recover the
raw parity. The mode you used is recorded in the metadata sidecar.

## Read modes: single and quad

**Single x1** uses one data line (`0Bh` fast read) and is the reliable default.
**Quad x4** uses all four data lines (`6Bh`) for ~4× throughput but needs SIO2/SIO3
wired and good signal integrity.

When you select Quad, the firmware runs a **self-test at boot**: it reads one page
via quad and via single and compares them. If they differ (bad wiring, or a chip
whose quad path isn't supported), it prints a warning and **falls back to single
x1** automatically. So selecting quad can never silently corrupt a dump — worst
case it quietly runs single.

## Running a dump

1. Pass the address the serial monitor printed with `--ip` (remembered in a
   local `dump.config.json` afterwards, so later runs need no flag):
2. Run it:

   ```bash
   python3 dump.py --ip 192.168.1.42   # first time
   python3 dump.py                     # thereafter
   ```

`dump.py` connects, sends the trigger, reads the geometry header the ESP32 sends,
and streams the dump to `target/nand_raw_dump_<timestamp>.bin`, printing progress.
Because it learns the geometry from the header, you never have to configure page
size or total size on the PC side — they can't get out of sync with the firmware.

For the Micron MT29F2G01 the expected size is **286,261,248 bytes**
(2048 blocks × 64 pages × 2176 bytes). If `dump.py` warns that it received fewer
bytes than expected, something interrupted the stream — see Troubleshooting.

## The metadata sidecar

Alongside each dump, `dump.py` writes `<dump>.bin.meta.json`:

```json
{
  "geometry": { "page_size": 2176, "spare_size": 128, "pages_per_block": 64,
                "total_blocks": 2048, "total_pages": 131072, "mfr_id": 44,
                "dev_id": 36, "page_addr_bits": 6, "flags": 0,
                "total_bytes": 286261248 },
  "ecc_on": false, "quad": false, "verify": true,
  "bytes_received": 286261248,
  "timestamp": "..."
}
```

This records exactly how the dump was taken. `ecc_stripper.py` reads it so
post-processing uses the right geometry automatically.

## Post-processing

Strip the spare/OOB area to get a clean, main-area-only image:

```bash
python3 ecc_stripper.py target/nand_raw_dump_*.bin target/clean.bin \
        --meta target/nand_raw_dump_*.bin.meta.json
```

Without a sidecar you can pass geometry explicitly:

```bash
python3 ecc_stripper.py raw.bin clean.bin \
        --page-size 2176 --spare-size 128 --pages-per-block 64
```

**Bad blocks:** the stripper checks the first spare byte of each block's first
page. If it isn't `0xFF`, the block is flagged bad and written as `0xFF` padding
so filesystem offsets stay aligned. It prints the list of bad blocks found.

## Repairing dumps by majority vote

If you take several dumps of the same chip (useful with ECC off on a noisy
setup), compare and repair them:

```bash
python3 tools/binary_compare_fix.py dump1.bin dump2.bin dump3.bin \
        -o target/corrected.bin -r target/report.txt
```

With 3+ files it fixes each disagreeing byte by majority vote and writes a report
listing every byte that differed and which files were wrong. With 2 files it can
only flag differences, not resolve them.

**CRC-aware repair (preferred for proto-v2 dumps).** Because each page now carries
a CRC verdict recorded in `<dump>.badpages.json`, `verify_dump.py` can do something
smarter than blind majority: for each page it takes a copy that *passed* its CRC,
and only majority-votes where every dump flagged the page bad.

```bash
python3 verify_dump.py dump.bin                       # health report for one dump
python3 verify_dump.py d1.bin d2.bin d3.bin -o fixed.bin   # CRC-aware repair
```

It reports how many pages came from a known-good copy vs needed majority voting.

## Understanding the raw layout

Each raw page for the MT29F2G01 is **2176 bytes = 2048 main + 128 spare**:

- bytes `0..2047` — main data (4 × 512-byte ECC sectors)
- byte `2048` — bad-block marker (`0x00` = bad, `0xFF` = good)
- bytes `2049..2175` — spare: user metadata and the on-die ECC parity

> A router's Linux log may report "OOB size: 64" for this chip. That's the
> MTD-visible free OOB after the ECC layout reserves the rest; the physical spare
> is 128 bytes and the dump captures all of it.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Verify retries > 0, mismatches | Lower SPI clock (menu `4` → 1 MHz), shorten wires, add a 100 nF cap, take multiple dumps and run `binary_compare_fix.py` |
| Quad self-test failed | Nothing to fix — it already fell back to single x1; the dump is valid. Check all four data lines if you want quad speed |
| Dump shorter than expected | Check the serial monitor for errors; verify power and wiring; retry at a lower clock |
| `Detected: UNKNOWN` | Enter geometry via menu `8`/`9`/`0`, or add the chip to `chips.yml` |
| WiFi won't connect | Re-check SSID/password (menu `1`/`2`) and that the board is in range |
| `dump.py` can't connect | Confirm `--ip` (or saved `dump.config.json`) matches the serial monitor and both are on the same network |
