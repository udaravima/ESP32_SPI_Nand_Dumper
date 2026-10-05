# Hardware Test Plan

The firmware reads SPI NAND, SPI NOR and serial EEPROMs, and every part of it
passes the simulated-chip tests in CI. None of it has run against a real chip
yet. This plan is the order in which to put it on real hardware so that, when
something fails, the failure points at one layer of the code and is cheap to
fix.

Everything here is **read-only**. The firmware never sends WRITE ENABLE,
PROGRAM or ERASE to any chip (see
[User Guide → Write protection](USER_GUIDE.md#write-protection-belt-and-suspenders)),
so the worst a bug can do in these tests is give you a bad dump.

## Contents

- [The idea in one picture](#the-idea-in-one-picture)
- [What to have](#what-to-have)
- [Before you touch hardware](#before-you-touch-hardware)
- [Wiring](#wiring)
- [Stage 0: board alone, no chip](#stage-0-board-alone-no-chip)
- [Stage 1: SPI NOR ID](#stage-1-spi-nor-id)
- [Stage 2: SPI NOR dump](#stage-2-spi-nor-dump)
- [Stage 3: I2C EEPROM (the shared-pin test)](#stage-3-i2c-eeprom-the-shared-pin-test)
- [Stage 4: SPI EEPROM (optional)](#stage-4-spi-eeprom-optional)
- [Stage 5: SPI NAND](#stage-5-spi-nand)
- [Stage 6: in-circuit (chip still on its board)](#stage-6-in-circuit-chip-still-on-its-board)
- [How to tell a good dump from a bad one](#how-to-tell-a-good-dump-from-a-bad-one)
- [When a stage fails: which layer is it?](#when-a-stage-fails-which-layer-is-it)
- [What to send back](#what-to-send-back)
- [Results sheet](#results-sheet)

## The idea in one picture

The code is a stack. Each stage below tests one more layer, and only starts
once the layer under it works:

```
  dump.py (PC)              <- stage 2 onwards
  WiFi / TCP session        <- stage 2 onwards
  chip database (db/)       <- stage 1 (right name and size?)
  family driver             <- NOR: 1-2, EEPROM: 3-4, NAND: 5
  shared bus layer          <- stage 0, then 1 (SPI) and 3 (I2C on the same pins)
  wires, clip, power        <- every stage
```

The shared bus layer is the one real risk. Every chip family goes through it,
and the clip uses the same six pins for SPI and for I2C (an I2C 24xx has its
SDA where a SPI chip has MOSI, and SCL where it has CLK). If that layer is
wrong, everything above it needs re-testing (not rewriting). That is why
stage 0, stage 1 and stage 3 come first and are kept small.

## What to have

### Required

| Item | Why | Rough cost |
|---|---|---|
| ESP32-classic dev board (WROOM-32, the FreeNove board in `docs/` is fine) | Runs the firmware. Start with classic; the S3 build is a separate test later. | $5 |
| SOIC-8 test clip with cable (the common "SOP8 clip" sold with CH341A kits) | Connects to 8-pin chips without soldering | $3 |
| SOIC-8 to DIP-8 adapter boards (a few) | Lets you test a loose chip on a breadboard, which is far easier than in-circuit | $1 |
| Short jumper wires (10 cm or less), a breadboard | Long wires cause bit flips | $2 |
| 100 nF ceramic capacitors, 10 kΩ and 4.7 kΩ resistors | Decoupling, CS# pull-up, I2C pull-ups | $1 |
| A 2.4 GHz WiFi network (or a phone hotspot) | The dump is streamed over WiFi | – |

### Test chips (3.3 V parts only)

| Chip | Family | Why this one |
|---|---|---|
| **Winbond W25Q32 / W25Q64 / W25Q128 (JV or FV)** | SPI NOR | The most common SPI NOR there is, compiled into the firmware, SOIC-8, cheap. Start here. |
| GigaDevice GD25Q or Macronix MX25L (any size) | SPI NOR | A second vendor, so a bug that only shows on one vendor's quirks stands out. |
| **24C02 and 24C256** (AT24C, 24LC, any maker) | I2C EEPROM | 24C02 uses a 1-byte address, 24C256 a 2-byte address: two code paths. |
| 25LC256 or AT25 (optional) | SPI EEPROM | Only if you have one; lowest priority. |
| **Dosilicon DS35Q1GA or Micron MT29F2G01ABAGD** | SPI NAND | The two NAND chips in `db/`. Most come in **WSON-8 (8×6 mm)**, which a SOIC clip does not fit: buy a WSON-8 socket/adapter or a breakout board. |

Avoid any part with **W** in the voltage position (W25Q128**JW**,
MT29F2G01AB**B**GD) for now: those are 1.8 V and need a level shifter.

**E-waste is a good source of test chips, and better than new ones.** A dead
router, a PC motherboard (BIOS chip), a TV board or an old monitor (24C02 for
its EDID) gives you chips with real data on them. That matters, see the next
point.

### Strongly recommended: a CH341A programmer ($3–5)

A brand-new flash chip is erased: every byte reads `0xFF`. A broken MISO wire
with a pull-up **also** reads `0xFF`. So a dump of a new chip that comes back
all `0xFF` proves almost nothing.

A CH341A (with `flashrom` or IMSProg/AsProgrammer on the PC) fixes that:

1. It can **write a known test pattern** into a NOR chip or EEPROM, which our
   firmware will never do.
2. It is a **second opinion**: read the same chip with both tools and compare.

**Warning:** many cheap CH341A boards put 5 V on the data lines. Check yours
(or do the well-known 3.3 V mod) before using it on a 3.3 V chip.

If you have no CH341A, use salvaged chips with real data, and rely on the
"read twice / read at two speeds" checks below.

## Before you touch hardware

Make sure the code you flash is the code that passed CI, so a failure on the
bench is a hardware question, not a build question:

```bash
git pull                     # on master
python3 -m pytest -q         # host tools, chip DB, wire format
pio test -e native           # pure C logic against simulated chips
pio run -e esp32dev          # firmware builds
```

All three should pass. Note the commit (`git rev-parse --short HEAD`); you will
put it in the report.

### Making a test pattern (if you have a CH341A)

A random file only tells you *that* a dump is wrong. A file where **every
4-byte word holds its own address** also tells you *where* the read went: if
offset `0x1000` holds `0x00000000`, address bit 12 is not reaching the chip.

```bash
python3 - <<'EOF'
import struct, sys
size = 4 * 1024 * 1024            # set to the chip size in bytes
with open("pattern.bin", "wb") as f:
    for a in range(0, size, 4):
        f.write(struct.pack(">I", a))
EOF
flashrom -p ch341a_spi -w pattern.bin    # writes it to the NOR chip in the clip
```

For a 24C02 (256 bytes) a simple counting pattern is enough:
`python3 -c "open('ee.bin','wb').write(bytes(range(256)))"`, written with
IMSProg or `ch341eeprom`.

## Wiring

The six signal pins are in [`src/board_pins.h`](../src/board_pins.h). For the
ESP32-classic:

| SOIC-8 pin | SPI flash (25xx, W25Q) | I2C EEPROM (24xx) | ESP32-classic |
|---|---|---|---|
| 1 | CS# | A0 | GPIO 5 |
| 2 | DO (MISO) | A1 | GPIO 19 |
| 3 | WP# | A2 | GPIO 22 |
| 4 | GND | GND | GND |
| 5 | DI (MOSI) | **SDA** | GPIO 23 |
| 6 | CLK | **SCL** | GPIO 18 |
| 7 | HOLD# | WP | GPIO 21 |
| 8 | VCC | VCC | 3V3 |

(ESP32-S3: CS 10, MISO 13, WP 14, MOSI 11, CLK 12, HOLD 9.)

One wiring serves every chip; there is nothing to re-wire between stages.

Checks before power-up, every time:

- **Pin 1 of the clip (red wire) on pin 1 of the chip (the dot).** A clip on
  backwards puts 3.3 V on the chip's GND pin.
- **Power the chip from the ESP32's 3V3 pin, never 5V/VIN.**
- A 100 nF capacitor between pin 8 and pin 4, close to the chip.
- A 10 kΩ pull-up from CS# (pin 1) to 3V3 is recommended: it keeps the chip
  deselected while the ESP32 boots.
- Change chips with the USB cable **unplugged**.

## Stage 0: board alone, no chip

**Tests:** the build, flashing, the serial menu, and the shared bus with
nothing on it.

1. Flash: `pio run -e esp32dev -t upload`
2. Open the monitor: `pio device monitor -b 115200`, press the board's EN/RST
   button.

**Pass:**

- The banner and the runtime capability report (chip model, cores, heap)
  print.
- With nothing in the clip, the firmware reports an unknown chip with both ID
  views flat, like `NAND view 0xFF 0xFF 0xFF, NOR view 0xFF 0xFF 0xFF`. (MISO
  has a pull-up, so an empty bus must read `0xFF`. Random bytes here mean a
  floating or noisy MISO line.)
- No EEPROM is "found" (no I2C answer, no plausible SPI status).
- The menu appears. Set WiFi with `1` and `2`, press `S`; the board connects
  and prints an IP address. Reset: it should reconnect without asking again
  (settings are in NVS).

## Stage 1: SPI NOR ID

**Tests:** the shared bus layer in SPI mode, and chip detection.
**Chip:** a W25Qxx, out of circuit (on a DIP adapter or bare in the clip).

Reset the board with the chip connected.

**Pass:**

- `[*] Detected SPI NOR W25Q64.V (0xEF 0x40 0x17)` (or your size; W25Q32 is
  `0x16`, W25Q128 is `0x18`).
- The menu shows the right size for the part (8.00 MB for a W25Q64).
- Reset five times. The ID is the same every time.

**If it fails:** an ID of all `0xFF` means MISO, CS or power is not reaching
the chip; all `0x00` usually means MISO is shorted to ground or the chip is
unpowered; a *changing* ID means a loose wire or a too-long cable. Re-check
wiring before anything else. Then repeat with the second vendor's chip
(GD25Q / MX25L).

## Stage 2: SPI NOR dump

**Tests:** the NOR driver, the WiFi session and `dump.py`.

1. Leave the menu at its defaults: **1 MHz clock, single mode, verify ON**.
2. Press `S`, then on the PC: `python3 dump.py --ip <ip-from-serial>`
3. The file lands in `target/nor_raw_dump_<time>.bin`.

**Pass, all of these:**

- The file size is exactly the chip size (8,388,608 bytes for a W25Q64).
- The serial log ends with `Retries: 0 | Failed pages: 0`.
- **Dump it a second time.** Both files have the same SHA-256
  (`sha256sum target/nor_raw_dump_*.bin`).
- The dump is **not** all `0xFF` (unless you know the chip is blank; then
  this stage is not a real test, use a chip with data or a pattern).
- If you wrote `pattern.bin` with the CH341A: `cmp pattern.bin <dump>` prints
  nothing. If it prints an offset, look at what was read there; with the
  address pattern, the wrong value tells you which address bit is broken.
- If you have a CH341A: read the chip with it too
  (`flashrom -p ch341a_spi -r ref.bin`) and compare hashes.

Then raise the speed, one step at a time: menu `4` → 5 MHz, then 10, then 20.
Each dump must hash the same as the 1 MHz one. Note the highest clock that
gives identical dumps; it depends mostly on your wires.

Quad mode (menu `5`) is **optional**. The firmware never sets the chip's QE
bit, so on most W25Qs it will report that QE is clear and fall back to single.
That fallback message is itself a pass.

## Stage 3: I2C EEPROM (the shared-pin test)

**Tests:** the riskiest piece. The firmware releases the SPI bus, runs I2C on
GPIO 23 (SDA) and GPIO 18 (SCL), and biases the other pins (A0 up, A1 down,
A2 up, WP up).
**Chip:** a 24C02 first, then a 24C256.

Add **4.7 kΩ pull-ups from SDA (GPIO 23) and SCL (GPIO 18) to 3V3** for this
stage. The firmware turns on the internal pull-ups too, but those are weak;
start with the external ones so a failure is about the code, not the
pull-ups. (Once it works, try without them and note the result.)

Reset the board with the 24C02 in the clip.

**Pass:**

- The NOR/NAND ID reads come back flat, and the firmware then says it found
  an I2C EEPROM, printing the I2C scan. With A0=1, A1=0, A2=1 as the firmware
  biases them, a 24C02 that uses its address pins answers at **0x55**. Many
  small parts (24C02 to 24C16) ignore some or all address pins and answer at
  several addresses, up to all of 0x50..0x57. Any answer is a pass; write down
  the exact scan line.
- Pick the part with `P` (24C02). The menu shows 256 bytes, 1-byte address.
- Dump it with `dump.py`. The file is exactly 256 bytes. Dump again: same
  hash. If you wrote the counting pattern, it reads back `00 01 02 ... FF`.
- Then the 24C256: pick `24C256`, expect 32,768 bytes, same checks.

**Extra checks for the shared pins:**

1. **Switch back to SPI without a reset.** With the EEPROM still in the clip,
   press `F` until the family comes back round to I2C EEPROM (it passes
   through SPI EEPROM, SPI NAND and SPI NOR, so the pins go I2C → SPI → I2C),
   then `R` to rescan. The scan should still find the part. Then swap the EEPROM for the
   W25Q (USB unplugged), power up and confirm stage 1 still passes.
2. **Wrong size on purpose.** Pick 24C04 for a 24C02. The dump should be 512
   bytes where the second half repeats the first (the part wraps). This is
   expected, not a bug, and the chip must be unaffected afterwards (dump it
   again as 24C02: same hash as before).
3. **400 kHz.** Menu `4` toggles 100/400 kHz. Both should give the same hash.

**If it fails here** and stages 1–2 passed, the bug is almost certainly in
the bus hand-over (`nand_bus_release()` / `eeprom_use_i2c()` in
`src/eeprom_driver.cpp`) and not in the EEPROM logic. Send the full serial log
including the scan line.

## Stage 4: SPI EEPROM (optional)

Only if you have a 25LC / AT25 / M95 part. Pick it with `P`, keep the clock at
1 MHz (many 25xx are rated 5 MHz or less at 3.3 V), dump twice, compare. The
firmware checks the part's status register first; the status value it prints
goes in your report.

## Stage 5: SPI NAND

**Tests:** the NAND driver, including the busy-wait, ECC and plane selection.
NAND is last because it has the most moving parts and needs a WSON adapter.

1. Connect the DS35Q1GA or MT29F2G01ABAGD, reset. Expect
   `[*] Detected SPI NAND DS35Q1GA (0xE5 0x71 ...)` or `MT29F2G01ABAGD (0x2C 0x24 ...)`.
2. Dump at 1 MHz, single, verify ON, **ECC ON** first.
   Expected size: **138,412,032 bytes** for the DS35Q1GA
   (1024 × 64 × 2112), **286,261,248 bytes** for the MT29F2G01
   (2048 × 64 × 2176).
3. Dump again with ECC ON. The two hashes must match, unless the serial log
   listed uncorrectable pages.
4. Now dump with **ECC OFF** (raw), twice. Raw dumps **may differ by a few
   bits**; NAND cells are physically noisy and raw mode shows that. Run
   `python3 tools/binary_compare_fix.py raw1.bin raw2.bin` and note how many
   bytes differ: a handful scattered around is normal, whole pages or a
   regular pattern is a bug.
5. MT29F2G01 only: `python3 tools/check_planes.py <dump>` must exit 0. This is
   the first real-chip test of the plane-select fix.
6. `python3 verify_dump.py <dump>` gives a health report; keep its output.
7. Strip the spare area and look at the result:
   `python3 ecc_stripper.py <dump> clean.bin --meta <dump>.meta.json`, then
   `binwalk clean.bin` if you have it. On a chip from a router you should see
   a bootloader, a kernel or a filesystem signature; on a blank chip, all
   `0xFF` and a list of factory bad blocks (a few is normal).

The serial log lists bad blocks (`[BBM]`) and, with ECC ON, uncorrectable
pages (`[ECC]`). Keep both.

## Stage 6: in-circuit (chip still on its board)

Only after stages 1–3 pass on loose chips. Clipping onto a chip that is still
soldered to a router or motherboard adds two problems that have nothing to do
with our code:

- **Back-powering:** the ESP32's 3V3 pin ends up powering the whole target
  board through the chip's VCC. The board may brown out, or the target's
  processor may wake up and drive the same SPI lines.
- **Bus fights:** the target's own processor is connected to the chip and may
  talk to it at the same time.

Try it with the target board **unpowered**. If the ID is unstable, hold the
target processor in reset if it has a reset pin, or lift the chip's VCC pin.
Note in the report that the test was in-circuit, and what the target board
was.

## How to tell a good dump from a bad one

| Check | How | What it catches |
|---|---|---|
| Size | File size equals the chip size | Wrong profile, a dropped connection |
| Repeatability | Two dumps, same `sha256sum` (NOR, EEPROM, NAND with ECC ON) | Noise, loose wires, timing |
| Speed independence | 1 MHz and a faster clock give the same hash | Timing, signal integrity |
| Not blank | `xxd <dump> \| head` shows something other than `ffff` | A dead MISO line reading the pull-up |
| Known content | Matches the pattern you wrote, or a CH341A read | Address and bit-order bugs that repeat the same way every time |
| No wrap | The second half of the file is not a copy of the first | A size that is too large, a stuck address bit |
| Looks like firmware | `binwalk`, `strings <dump> \| head` | Gross corruption |

The "known content" row is the one the others cannot replace: a bug that
reads the wrong address the same way every time passes the repeatability
check perfectly. (That is exactly how the old plane-select bug hid.)

## When a stage fails: which layer is it?

| Symptom | Most likely layer | Where in the code |
|---|---|---|
| Random or changing ID | Wires / power | Check wiring first |
| Stable ID, but the wrong chip name or size | Chip database | `db/chips/` entry |
| ID right, dump wrong only at higher clocks | Wires or timing | Lower the clock; `nand_set_clock()` |
| ID right, dump wrong even at 1 MHz | Family driver | `src/nor_seq.cpp`, `src/nand_seq.cpp`, `src/eeprom_seq.cpp` |
| Same wrong bytes every time (pattern offset) | Address handling in the driver | the `*_seq.cpp` file for that family |
| NAND pages partly stale or repeated | Missed busy-wait | `nand_wait_ready()`, `src/nand_seq.cpp` |
| SPI works, I2C never answers | Shared bus hand-over | `src/eeprom_driver.cpp`, `src/nand_driver.cpp` (`nand_bus_release/restore`) |
| I2C works, SPI broken after an I2C session | Shared bus hand-over | same as above |
| Dump starts then stops | WiFi / session | `src/wifi_transport.cpp`, `dump.py` |

## What to send back

For each failure (and ideally each pass, so the results sheet fills up),
post in the project thread:

1. **Stage number** and what you expected versus what happened.
2. **Commit** you flashed (`git rev-parse --short HEAD`) and the board
   (ESP32-classic or S3, which dev board).
3. **Chip:** the full marking on the package (e.g. `W25Q64JVSIQ`), and
   whether it was new, salvaged or in-circuit.
4. **The complete serial log** from reset to the end of the dump, as a text
   file. Partial logs hide the boot-time ID and scan lines, which matter most.
5. **The `dump.py` output** and the `.meta.json` sidecar.
6. **Settings:** clock, read mode, verify, ECC, and whether the I2C pull-ups
   were fitted.
7. **Hashes** of each dump (`sha256sum`), and the reference hash if you have
   a CH341A read.
8. For a wrong dump, the first difference: `cmp ref.bin dump.bin | head` and
   `xxd -s <offset> -l 64` on both files around it.
9. A photo of the wiring if anything looks odd.

Small dumps (EEPROMs, small NOR chips) can be attached as-is. Do not attach a
dump of a chip from someone else's device if it may contain their private
data (WiFi passwords, keys).

## Results sheet

Copy this into a GitHub issue or the project thread and fill it in as you go.

| Stage | Chip (marking) | Clock | Result | Notes |
|---|---|---|---|---|
| 0 Board alone | – | – | | |
| 1 NOR ID | | 1 MHz | | |
| 1 NOR ID, 2nd vendor | | 1 MHz | | |
| 2 NOR dump, repeat hash | | 1 MHz | | |
| 2 NOR dump vs pattern/CH341A | | 1 MHz | | |
| 2 Highest clock with same hash | | | | |
| 3 24C02 scan + dump | | 100 kHz | | |
| 3 24C256 dump | | 100 kHz | | |
| 3 I2C ↔ SPI switch | | | | |
| 3 400 kHz | | 400 kHz | | |
| 3 Without external pull-ups | | | | |
| 4 SPI EEPROM (optional) | | 1 MHz | | |
| 5 NAND ECC ON ×2 | | 1 MHz | | |
| 5 NAND raw ×2, bytes differing | | 1 MHz | | |
| 5 check_planes (MT29F2G01) | | | | |
| 6 In-circuit | | | | |
| ESP32-S3 build (repeat 0–2) | | | | |

Once stages 0–3 pass, the shared bus layer is proven and work on the
remaining read features can continue safely. Writing and erasing should wait
until the whole sheet is green.
