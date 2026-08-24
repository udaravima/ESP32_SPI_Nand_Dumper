# ESP32 SPI NAND Dumper

A chip-agnostic ESP32 tool for extracting firmware from SPI NAND flash over WiFi. It **auto-detects the chip** by its JEDEC ID, loads geometry from a community-editable registry (`chips.yml`), and streams a raw dump over TCP. Builds for **ESP32-classic and ESP32-S3** from one source tree.

Verified on the **Micron MT29F2G01** (2 Gbit, JEDEC `0x2C 0x24`). Adding another chip is a few lines of YAML — see [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- **[Quick Start](docs/QUICKSTART.md)** — first dump in ~10 minutes.
- **[User Guide](docs/USER_GUIDE.md)** — full operation: wiring, config menu, ECC modes, workflow, troubleshooting.
- **[Developer Guide](docs/DEVELOPER_GUIDE.md)** — architecture, tests, wire protocol, adding a chip or board.
- **[Contributing](CONTRIBUTING.md)** — add your chip to the registry.

## Features

- **Auto-detect by JEDEC ID** — reads `9Fh`, looks up geometry + capabilities in the compiled chip table. Unknown chips fall back to a manual-entry menu.
- **Community chip registry** — `chips.yml` is the single human-editable source; a build-time hook generates the C table (`src/nand_chips_generated.h`).
- **ECC selectable per dump, default OFF/raw** — raw preserves the literal stored bits + parity (reversible: you can compute the corrected image later, but never recover raw parity from a corrected dump). Toggle to ON for an immediately-mountable image.
- **Quad (x4) with a self-test + automatic fallback** — a quad read is compared against a single read at boot; on mismatch it logs and falls back to single x1, so an unsupported quad setup never silently corrupts a dump.
- **Geometry handshake** — the firmware sends a 32-byte header before the stream; `dump.py` and `ecc_stripper.py` self-configure from it, so page-size mismatches can't happen.
- **Read verification** — each page read twice and compared, with retries.
- **Dual-board** — `esp32dev` and `esp32-s3-devkitc-1` build targets.

## Hardware Setup

ESP32 GPIO is 3.3 V. The MT29F2G01**AB A** GD (3.3 V) wires directly; a **1.8 V** part (e.g. the `ABBGD` variant) needs a level shifter. Keep wires short — long jumpers cause bit flips at higher clocks.

### Wiring

| NAND pin | Function | ESP32-classic | ESP32-S3 |
|---|---|---|---|
| CLK | SPI Clock | GPIO 18 | GPIO 12 |
| DI / SIO0 | MOSI / IO0 | GPIO 23 | GPIO 11 |
| DO / SIO1 | MISO / IO1 | GPIO 19 | GPIO 13 |
| WP# / SIO2 | IO2 (quad) | GPIO 22 | GPIO 14 |
| HOLD# / SIO3 | IO3 (quad) | GPIO 21 | GPIO 9 |
| CS# | Chip Select | GPIO 5 | GPIO 10 |
| VCC | 3.3 V (or via level shifter) | 3V3 | 3V3 |
| GND | Ground | GND | GND |

The classic pins are the VSPI (SPI3) IOMUX set; the S3 pins are the FSPI (SPI2) IOMUX set (GPIO9–14, chosen to avoid the S3's internal flash/PSRAM pins GPIO26–37 and all strapping/USB/UART pins). Pins live in [src/board_pins.h](src/board_pins.h). For single-SPI mode only SIO0/SIO1 are needed.

## Quick Start

### 1. Build & flash

```bash
pio run -e esp32dev -t upload                 # ESP32-classic
pio run -e esp32-s3-devkitc-1 -t upload       # ESP32-S3
```

The build regenerates `src/nand_chips_generated.h` from `chips.yml` automatically.

### 2. Configure via the serial menu

```bash
pio device monitor -b 115200
```

The menu shows the **detected chip** with geometry pre-filled:

```
  ESP32 SPI NAND Dumper v3.1.1 — Config
  Detected: MT29F2G01ABAGD (0x2C 0x24)
  [1] WiFi SSID / [2] Password / [3] TCP Port
  [4] SPI Clock  / [5] Read Mode / [6] Verify / [7] Max Retries
  [E] ECC on read:  OFF (raw)
  [8] Page Size / [9] Pages/Block / [0] Total Blocks
  [S] START
```

Change any field, toggle **[E]** for ECC (default OFF/raw), press **S** to start. Settings are saved to the ESP32's NVS, so later boots skip re-entry. No reflash needed to change settings.

### 3. Receive the dump

Pass the IP printed in the serial monitor (remembered in `dump.config.json` after the first run):

```bash
python3 dump.py --ip 192.168.1.42   # first time
python3 dump.py                     # thereafter
```

It reads the geometry header, streams to `target/nand_raw_dump_<timestamp>.bin`, and writes a `<dump>.meta.json` sidecar describing the geometry and ECC state.

### 4. Post-process

Strip spare/OOB to a mountable main-area image, using geometry from the sidecar:

```bash
python3 ecc_stripper.py target/nand_raw_dump_*.bin target/clean.bin \
        --meta target/nand_raw_dump_*.bin.meta.json
```

### 5. (Optional) Compare & repair multiple dumps

```bash
python3 tools/binary_compare_fix.py dump1.bin dump2.bin dump3.bin
```

Majority-votes across dumps to correct transmission errors, with a detailed report.

## Adding a Chip

Edit [chips.yml](chips.yml) (JEDEC ID from the serial log's `mfr_id`/`dev_id`), run `pio run`, and open a PR. See [CONTRIBUTING.md](CONTRIBUTING.md). Entries are validated at build time — a malformed one fails the build with a message naming the field.

> A data-driven vendor **profile database** (`db/`) is being built to group vendor-specific ECC/OOB quirks (SPI NAND is only partly standardized). It is host-side foundation today and **not yet on the device path** — `chips.yml` is what the firmware uses. See the [Developer Guide](docs/DEVELOPER_GUIDE.md#vendorprofile-architecture-stage-1).

## NAND Details (MT29F2G01)

| Parameter | Value |
|---|---|
| JEDEC ID | Mfr `0x2C` (Micron), Dev `0x24` |
| Capacity | 2 Gbit (256 MB) |
| Page size | **2176 bytes = 2048 main + 128 spare** |
| Block size | 64 pages (128 KB) |
| Total blocks | 2048 (2 planes × 1024) |
| Row address | 17-bit: Block[10:0] + Page[5:0] |
| Internal ECC | user-selectable, 8-bit / 512 bytes |

> **Why boot logs say "OOB size: 64" but the spare is 128:** the physical spare is 128 bytes; a Linux MTD driver exposes only 64 as free OOB because the on-die ECC layout reserves the rest. The dump captures all 2176 bytes/page.

### Key SPI commands

| Command | Opcode | Notes |
|---|---|---|
| RESET | `FFh` | |
| PAGE READ | `13h` | array → cache, 24-bit row |
| READ FROM CACHE | `0Bh` | single-line fast read |
| READ FROM CACHE x4 | `6Bh` | 1-1-4 quad read |
| GET / SET FEATURE | `0Fh` / `1Fh` | no WRITE ENABLE needed |
| READ ID | `9Fh` | manufacturer + device |

### Feature registers

| Addr | Name | Key bits |
|---|---|---|
| `A0h` | Block Lock | BP0–3, TB (protects PROGRAM/ERASE only — reads always allowed) |
| `B0h` | **Configuration** | **ECC_EN (bit 4)**, CFG0–2, LOT_EN. **No QE bit** — Micron quad reads need no enable. |
| `C0h` | Status | OIP, WEL, E_Fail, P_Fail, **ECCS0–2 (3-bit ECC status, bits 4–6)** |

> These are Micron/Configuration-register semantics. Winbond/GigaDevice parts *do* have a QE bit — declare it per-chip in `chips.yml` (`has_qe_bit`, `qe_feature_addr`, `qe_bit`) and the quad path enables it automatically.

### ECC: raw vs corrected

ECC powers up **enabled**. This tool defaults it **OFF** for dumping:

- **OFF (raw):** the array is read without correction; you capture the literal bits including the ECC parity region. Lossless and reversible — you can run correction offline later, or majority-vote across dumps.
- **ON (corrected):** the on-die engine corrects bit errors on read (status in ECCS0–2), giving cleaner data, but the raw parity is gone and uncorrectable pages are silently mis-corrected.

## Troubleshooting

- **Read mismatches / retries > 0** — lower the SPI clock (1 MHz for noisy setups), shorten wires, add a 100 nF cap across VCC/GND, or take multiple dumps and run `binary_compare_fix.py`.
- **Quad self-test failed** — the tool already fell back to single x1; the dump is fine. Check all four data lines if you want quad speed.
- **Unknown chip** — the menu lets you enter geometry manually; better, add it to `chips.yml`.
- **WiFi fails** — check SSID/password in the menu and range.

## Development / Tests

```bash
pip install -r requirements-dev.txt
python3 -m pytest              # host-side tools + wire-format cross-check
pio test -e native            # pure C logic (lookup, row-address, header)
```

The wire header is tested on both sides (C `dump_header_pack` and Python `parse_header`) against a golden byte image, so the two ends can't silently diverge.

## License

MIT
