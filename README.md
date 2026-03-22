# ESP32 SPI NAND Dumper

A modular ESP32-based tool for extracting firmware from SPI NAND flash chips over WiFi. Supports **Quad SPI (x4)** for high-speed reads and includes automatic read verification for signal integrity.

Built for the **DS35x1GA** series SPI NAND (FORESEE/DoSilicon), but adaptable to other SPI NAND chips with similar command sets (Winbond W25N, GigaDevice GD5F, etc.)

## Features

- **Quad SPI (x4) reads** — 4x data throughput vs standard single-line SPI
- **WiFi TCP streaming** — no more fragile serial UART; reliable TCP flow control
- **Read verification** — each page read twice and compared; auto-retries on mismatch
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
│   ├── main.cpp            # Configuration & dump command orchestration
│   ├── nand_driver.h       # NAND SPI driver API
│   ├── nand_driver.cpp     # NAND SPI implementation (single + quad)
│   ├── wifi_transport.h    # WiFi TCP transport API
│   └── wifi_transport.cpp  # WiFi TCP implementation
├── dump.py                 # PC-side: receives dump over TCP, saves to file
├── ecc_stripper.py         # Post-processing: strips OOB/spare, handles bad blocks
├── docs/
│   ├── DS35x1GAxxx_SPI_NAND.pdf   # NAND datasheet
│   └── esp32-wroom-32e_*.pdf      # ESP32 datasheet
├── platformio.ini          # PlatformIO build configuration
└── target/                 # Dump output directory (gitignored)
```

## Quick Start

### 1. Configure WiFi Credentials

Edit `src/main.cpp`:
```cpp
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";
```

### 2. Build & Flash

```bash
pio run --target upload
```

### 3. Get the ESP32's IP Address

```bash
pio device monitor -b 115200
```

You'll see output like:
```
[*] ESP32 SPI NAND Dumper v2.0
[*] SPI clock: 5000000 Hz (Quad x4 mode)
[*] NAND ID: 0xE571 (Mfr: 0xE5, Dev: 0x71)
[+] Connected! IP: 192.168.1.42
[*] TCP server on port 3333
[*] Waiting for client...
```

### 4. Update the PC Script & Run

Edit `dump.py` with the IP shown in the serial monitor:
```python
ESP32_IP = '192.168.1.42'  # <-- paste IP from serial monitor
```

Run the dump (from a separate terminal):
```bash
python3 dump.py
```

### 5. Post-Process the Dump

Strip the 64-byte OOB/spare area from each page and handle bad blocks:
```bash
python3 ecc_stripper.py
```

This converts the raw 2112-byte/page dump into a clean 2048-byte/page firmware image.

## Configuration Options

All configuration is in `src/main.cpp`:

| Option | Default | Description |
|--------|---------|-------------|
| `SPI_CLOCK_HZ` | `5000000` | SPI bus speed in Hz. Lower if you see read errors. |
| `READ_MODE` | `NAND_READ_QUAD` | `NAND_READ_QUAD` for x4, `NAND_READ_SINGLE` for x1 |
| `VERIFY_READS` | `true` | Read each page twice and compare |
| `MAX_RETRIES` | `5` | Max retry attempts on verification mismatch |
| `TCP_PORT` | `3333` | TCP server port |

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

## NAND Chip Details (DS35Q1GA)

| Parameter | Value |
|-----------|-------|
| Capacity | 1 Gbit (128 MB) |
| Page Size | 2048 + 64 spare = 2112 bytes |
| Block Size | 64 pages = 132 KB |
| Total Blocks | 1024 |
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
- **Lower `SPI_CLOCK_HZ`** — try 1 MHz for noisy setups
- **Shorten wires** — long breadboard wires degrade signals
- **Add decoupling capacitor** — 100nF between VCC and GND near the NAND chip

### WiFi connection fails
- Verify SSID/password in `main.cpp`
- Ensure the ESP32 is within WiFi range
- Check serial monitor for connection status

### Dump size mismatch
- Expected: 138,412,032 bytes (132 MB raw with spare)
- If short: check serial monitor for error messages
- Verify NAND chip is properly connected and powered

### QE bit won't set
- Some NAND chips have QE enabled by default
- Check if the chip has a write-protect mechanism (WP# pin should be HIGH)
- Try issuing WRITE ENABLE (06h) before SET FEATURES

## License

MIT
