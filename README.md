# ESP32 SPI NAND Dumper

A modular ESP32-based tool for extracting firmware from SPI NAND flash chips over WiFi. Supports **Quad SPI (x4)** for high-speed reads, **interactive serial configuration** (no reflashing for parameter changes), and automatic read verification for signal integrity.

Built for the **DS35x1GA** series SPI NAND (FORESEE/DoSilicon), but adaptable to other SPI NAND chips with similar command sets (Winbond W25N, GigaDevice GD5F, etc.)

## Features

- **Interactive serial config menu** — change WiFi, SPI clock, read mode at boot without reflashing
- **Quad SPI (x4) reads** — 4x data throughput vs standard single-line SPI
- **WiFi TCP streaming** — no more fragile serial UART; reliable TCP flow control
- **Read verification** — each page read twice and compared; auto-retries on mismatch
- **Multi-dump comparison & repair** — majority-vote tool to fix transmission errors across multiple dumps
- **Modular architecture** — NAND driver, WiFi transport, and command logic are cleanly separated
- **ECC status checking** — read the NAND's internal ECC status after each page
- **Bad block detection** — post-processing script identifies and handles bad blocks
- **NAND ID readout** — verify chip manufacturer and device ID on startup

## Hardware Setup

### Components
- ESP32 Dev Board (ESP32-WROOM-32)
- SPI NAND Flash (DS35Q1GA or compatible)
- Jumper wires (keep short for signal integrity!)

### Wiring

| NAND Pin | Function     | ESP32 GPIO |
|----------|-------------|------------|
| CLK      | SPI Clock   | GPIO 18    |
| DI/SIO0  | MOSI / IO0  | GPIO 23    |
| DO/SIO1  | MISO / IO1  | GPIO 19    |
| WP#/SIO2 | IO2 (Quad)  | GPIO 22    |
| HOLD#/SIO3 | IO3 (Quad) | GPIO 21   |
| CS#      | Chip Select | GPIO 5     |
| VCC      | Power       | 3.3V       |
| GND      | Ground      | GND        |

> [!IMPORTANT]
> For Quad SPI mode, **all 4 data lines** (SIO0–SIO3) must be connected. For single SPI mode, only SIO0 (MOSI) and SIO1 (MISO) are needed.

> [!TIP]
> Keep wires as short as possible. Long jumper wires cause bit flips at higher SPI clock speeds. If you see read mismatches, lower `SPI_CLOCK_HZ` in `main.cpp`.

## Project Structure

```
├── src/
│   ├── main.cpp            # Interactive config menu & dump orchestration
│   ├── nand_driver.h       # NAND SPI driver API
│   ├── nand_driver.cpp     # NAND SPI implementation (single + quad)
│   ├── wifi_transport.h    # WiFi TCP transport API
│   └── wifi_transport.cpp  # WiFi TCP implementation
├── dump.py                 # PC-side: receives dump over TCP, saves to file
├── ecc_stripper.py         # Post-processing: strips OOB/spare, handles bad blocks
├── tools/
│   └── binary_compare_fix.py  # Compare multiple dumps & fix via majority voting
├── docs/
│   ├── DS35x1GAxxx_SPI_NAND.pdf   # NAND datasheet
│   └── esp32-wroom-32e_*.pdf      # ESP32 datasheet
├── platformio.ini          # PlatformIO build configuration
└── target/                 # Dump output directory (gitignored)
```

## Quick Start

### 1. Build & Flash

```bash
pio run --target upload
```

### 2. Configure via Serial Menu

Open the serial monitor:
```bash
pio device monitor -b 115200
```

You'll see the interactive config menu:
```
========================================
  ESP32 SPI NAND Dumper v2.1 — Config
========================================
  [1] WiFi SSID:      GAE
  [2] WiFi Password:  o*********!
  [3] TCP Port:       3333
  [4] SPI Clock (Hz): 1000000
  [5] Read Mode:      Single x1
  [6] Verify Reads:   ON
  [7] Max Retries:    5
  ----------------------------------------
  [S] START dump with above settings
========================================
  Select>
```

- Type `1`–`7` to change a parameter, then enter the new value
- SPI Clock (option `4`) offers presets: 1/5/10/20/40 MHz or custom Hz
- Options `5` and `6` toggle on each press
- Press **S** to start the dump with the displayed settings

> [!TIP]
> No need to reflash to change WiFi or SPI settings — just reset the ESP32 and reconfigure from the menu.

### 3. Run the PC-side Receiver

Edit `dump.py` with the IP shown in the serial monitor after WiFi connects:
```python
ESP32_IP = '192.168.1.42'  # <-- paste IP from serial monitor
```

Run the dump (from a separate terminal):
```bash
python3 dump.py
```

### 4. Post-Process the Dump

Strip the OOB/spare area from each page and handle bad blocks:
```bash
python3 ecc_stripper.py
```

This converts the raw dump into a clean 2048-byte/page firmware image.

### 5. (Optional) Compare & Repair Multiple Dumps

If you have multiple dumps of the same chip, use the comparison tool to fix transmission errors via majority voting:
```bash
python3 tools/binary_compare_fix.py target/dump1.bin target/dump2.bin target/dump3.bin
```

This generates a corrected binary and a detailed report showing every byte that disagreed.

## Configuration Options

All options are configurable at boot via the serial menu (no reflashing required):

| Option | Default | Description |
|--------|---------|-------------|
| WiFi SSID | `GAE` | WiFi network name |
| WiFi Password | `****` | WiFi password |
| TCP Port | `3333` | TCP server port for data streaming |
| SPI Clock | `1000000` (1 MHz) | SPI bus speed in Hz. Lower if you see read errors. |
| Read Mode | `Single x1` | `Single x1` or `Quad x4` (requires all 4 data lines) |
| Verify Reads | `ON` | Read each page twice and compare |
| Max Retries | `5` | Max retry attempts on verification mismatch |

Default values can be changed by editing the `cfg_*` variables at the top of `src/main.cpp`.

## NAND Driver API

The NAND driver (`nand_driver.h`) provides a clean abstraction for extending functionality:

```cpp
// Initialization
esp_err_t nand_init(const nand_config_t *config);

// Basic operations
void      nand_reset();
void      nand_wait_ready();
uint16_t  nand_read_id();

// Feature registers (Block Lock, OTP/QE, Status, Driver Strength)
uint8_t   nand_get_feature(uint8_t addr);
void      nand_set_feature(uint8_t addr, uint8_t value);

// Page reads
void      nand_page_read_to_cache(uint16_t row_addr);
void      nand_read_cache(uint8_t *buf, int len);          // Uses configured mode
void      nand_read_cache_single(uint8_t *buf, int len);   // Force x1
void      nand_read_cache_quad(uint8_t *buf, int len);     // Force x4

// Verified read (read + compare + retry)
bool      nand_read_page_verified(uint16_t row_addr, uint8_t *buf,
                                   int max_retries, uint32_t *retry_count);

// Diagnostics
uint8_t   nand_get_ecc_status();
nand_read_mode_t nand_get_read_mode();
```

### Adding New Commands

To add a new operation (e.g., bad block scan), create a new function in `main.cpp`:

```cpp
void cmd_scan_bad_blocks() {
  for (uint16_t block = 0; block < NAND_TOTAL_BLOCKS; block++) {
    uint16_t row = block << 6;  // First page of each block
    nand_page_read_to_cache(row);
    nand_wait_ready();

    uint8_t spare[64];
    // Read spare area starting at column 2048
    // ... check bad block marker at byte 0 of spare
  }
}
```

## NAND Chip Details (DS35Q2GA)

| Parameter | Value |
|-----------|-------|
| Capacity | 2 Gbit (256 MB) |
| Page Size | 2048 + 128 spare = 2176 bytes |
| Block Size | 64 pages = 136 KB |
| Total Blocks | 2048 |
| SPI Modes | x1, x2, x4 |
| Max Clock | 104 MHz |
| Internal ECC | 4-bit per 512-byte sector |
| Row Address | 16-bit: Block[15:6] + Page[5:0] |

### Key SPI Commands

| Command | Opcode | Description |
|---------|--------|-------------|
| RESET | `FFh` | Reset device to default state |
| PAGE READ | `13h` | Load page from array → cache |
| READ FROM CACHE | `0Bh` | Read cache (single, fast read) |
| READ FROM CACHE x4 | `6Bh` | Read cache (quad mode) |
| GET FEATURES | `0Fh` | Read feature register |
| SET FEATURES | `1Fh` | Write feature register |
| READ ID | `9Fh` | Read manufacturer + device ID |

### Feature Registers

| Address | Name | Key Bits |
|---------|------|----------|
| `A0h` | Block Lock | BP0, BP1, BP2 (write protection) |
| `B0h` | OTP | QE (bit 0) — Quad Enable |
| `C0h` | Status | OIP, WEL, ECC_S0, ECC_S1, P_Fail, E_Fail |

## Troubleshooting

### Read verification mismatches (retries > 0)
- **Lower SPI Clock** — select 1 MHz from the config menu for noisy setups
- **Shorten wires** — long breadboard wires degrade signals
- **Add decoupling capacitor** — 100nF between VCC and GND near the NAND chip
- **Take multiple dumps** — use `tools/binary_compare_fix.py` to fix errors via majority voting

### WiFi connection fails
- Check SSID/password in the serial config menu
- Ensure the ESP32 is within WiFi range
- Check serial monitor for connection status

### Dump size mismatch
- Expected: 286,261,248 bytes (273 MB raw with spare) for DS35Q2GA
- If short: check serial monitor for error messages
- Verify NAND chip is properly connected and powered

### QE bit won't set
- Some NAND chips have QE enabled by default
- Check if the chip has a write-protect mechanism (WP# pin should be HIGH)
- Try issuing WRITE ENABLE (06h) before SET FEATURES

## License

MIT
