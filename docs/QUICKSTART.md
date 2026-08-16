# Quick Start

Your first NAND dump in about ten minutes. This is the shortest path; for the
full picture see the [User Guide](USER_GUIDE.md).

## 0. You need

- An ESP32 dev board — **ESP32-classic** (WROOM-32) or **ESP32-S3**.
- An SPI NAND chip on 3.3 V (a 1.8 V part needs a level shifter).
- [PlatformIO](https://platformio.org/install) and Python 3.
- Short jumper wires — long ones cause bit flips.

## 1. Wire it

| NAND pin | ESP32-classic | ESP32-S3 |
|---|---|---|
| CLK | GPIO 18 | GPIO 12 |
| DI / SIO0 (MOSI) | GPIO 23 | GPIO 11 |
| DO / SIO1 (MISO) | GPIO 19 | GPIO 13 |
| WP# / SIO2 | GPIO 22 | GPIO 14 |
| HOLD# / SIO3 | GPIO 21 | GPIO 9 |
| CS# | GPIO 5 | GPIO 10 |
| VCC → 3V3, GND → GND | | |

For a first dump in single-line mode you only strictly need CLK, MOSI, MISO, CS,
VCC, GND — but wiring all four data lines lets you try quad later.

## 2. Build & flash

```bash
pio run -e esp32dev -t upload            # ESP32-classic
# pio run -e esp32-s3-devkitc-1 -t upload  # ESP32-S3
```

## 3. Open the serial monitor, start the dump

```bash
pio device monitor -b 115200
```

You'll see the chip auto-detected and a config menu. Note the **IP address**
printed after WiFi connects. Defaults are fine for a first run — just press **S**
to start. (Set WiFi with menu options `1`/`2` first if needed.)

## 4. Receive the dump on your PC

Edit `dump.py`, set `ESP32_IP` to the IP from step 3:

```python
ESP32_IP = '192.168.1.42'   # <-- from the serial monitor
```

Then, from a second terminal:

```bash
python3 dump.py
```

It reads the geometry the ESP32 sends, streams the dump, and writes two files in
`target/`:

- `nand_raw_dump_<timestamp>.bin` — the raw dump (main + spare per page)
- `nand_raw_dump_<timestamp>.bin.meta.json` — geometry + settings used

## 5. Make a mountable image (optional)

Strip the spare/OOB area, reading geometry from the sidecar:

```bash
python3 ecc_stripper.py target/nand_raw_dump_*.bin target/clean.bin \
        --meta target/nand_raw_dump_*.bin.meta.json
```

## That's it

- Dump didn't match its expected size, or you saw retries? → [User Guide → Troubleshooting](USER_GUIDE.md#troubleshooting)
- Want raw-vs-corrected ECC, quad mode, or bad-block handling explained? → [User Guide](USER_GUIDE.md)
- Adding a new chip or hacking on the firmware? → [Developer Guide](DEVELOPER_GUIDE.md)
