#include <Arduino.h>
#include "nand_driver.h"
#include "nor_driver.h"
#include "eeprom_driver.h"
#include "eeprom_seq.h"
#include "nand_profile.h"
#include "chip_detect.h"
#include "sfdp.h"
#include "nand_session.h"
#include "nand_addr.h"
#include "dump_header.h"
#include "wifi_transport.h"
#include "config_store.h"
#include "sys_info.h"

#define MAX_PAGE_SIZE 8192  // upper bound for buffer/transfer sizing
// Session timeouts: how long a connected client may sit between commands, and
// how long one command's payload (a pushed profile, an ARM CRC) may take.
#define SESSION_IDLE_MS    30000
#define SESSION_PAYLOAD_MS 5000

// ============ RUNTIME CONFIG (defaults) ============
// WiFi creds start empty: entered once via the menu, then persisted to NVS, so
// no password ships in source. NVS overrides these on boot if a config exists.
static char     cfg_ssid[64]     = "";
static char     cfg_pass[64]     = "";
static uint16_t cfg_tcp_port     = 3333;
static int      cfg_spi_clock_hz = 1000000;    // 1 MHz
static nand_read_mode_t cfg_read_mode = NAND_READ_SINGLE;
static bool     cfg_verify       = true;
static int      cfg_max_retries  = 5;
static int      cfg_batch_pages  = 1;          // pages coalesced per TCP write (throughput knob)
// NAND geometry (filled from the detected chip; overridable in the menu)
static int      cfg_page_size       = 2176;
static int      cfg_spare_size       = 128;
static int      cfg_pages_per_block = 64;
static int      cfg_total_blocks    = 2048;
static int      cfg_page_addr_bits  = 6;
static int      cfg_planes          = 1;   // 2 on multi-plane parts (MT29F2G01)
static bool     cfg_ecc_on          = false;   // global policy: OFF/raw (SPI NAND only)
// An unknown chip's family and, for SPI NOR, its size (menu [F] / [8]).
static uint8_t  cfg_family          = CHIP_FAMILY_SPI_NAND;
static uint32_t cfg_nor_size        = 1u << 20;
static uint32_t cfg_i2c_hz          = 100000;   // I2C EEPROM clock: 100 or 400 kHz
// Detected chip. g_active is the one flat profile the read path runs against:
// a copy of the resident entry for a known chip, a host-pushed profile once
// one is armed, or a manual profile built from the menu geometry otherwise.
static uint8_t  g_id[3] = {0, 0, 0};           // mfr, dev, dev2: 9Fh + dummy (SPI NAND view)
static uint8_t  g_nor_id[3] = {0, 0, 0};       // mfr, type, capacity: plain 9Fh (SPI NOR view)
static uint8_t  g_detect_family = CHIP_FAMILY_SPI_NAND;   // family of a shared-ID match
static bool     g_known = false;               // g_active came from the table or a push
static nand_chip_state_t g_chip_state = NAND_CHIP_UNKNOWN;
static active_profile_t g_active;
static nand_session_t g_session;
// Serial EEPROMs have no ID; what the device knows is whether one answers.
static uint8_t  g_i2c_mask = 0;                // ACK scan of 0x50..0x57 (bit i = 0x50 + i)
static uint8_t  g_i2c_base = EEPROM_I2C_BASE;  // where the picked 24xx answers
static uint8_t  g_spi_ee_sr = 0xFF;            // RDSR as read (0xFF: nothing answered)
// ===================================================

void cmd_dump(bool verify);
void apply_runtime_settings();
static void adopt_profile(const active_profile_t *p, nand_chip_state_t state);
String read_serial_line();

static bool is_nor() { return g_active.family == CHIP_FAMILY_SPI_NOR; }
// The family the menu and the read path work with: the active profile's once
// a chip is known, else the user's choice for the unknown chip.
static uint8_t cur_family() { return g_known ? g_active.family : cfg_family; }

static const char *family_name(uint8_t f) {
  switch (f) {
    case CHIP_FAMILY_SPI_NOR:    return "SPI NOR";
    case CHIP_FAMILY_I2C_EEPROM: return "I2C EEPROM";
    case CHIP_FAMILY_SPI_EEPROM: return "SPI EEPROM";
    default:                     return "SPI NAND";
  }
}

static bool flat_id(const uint8_t id[3]) {
  return (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) ||
         (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF);
}

// Is this EEPROM profile's part plausibly in the clip? The same rule the
// session applies to a pushed EEPROM profile (nand_session.cpp).
static bool eeprom_bound(const active_profile_t *p) {
  if (p->family == CHIP_FAMILY_I2C_EEPROM) return eeprom_i2c_base(p, g_i2c_mask) >= 0;
  return eeprom_spi_status_plausible(g_spi_ee_sr);
}

// Read both EEPROM presence signals: RDSR on SPI, then the I2C ACK scan (the
// clip's pins are lent to I2C for it and handed back unless I2C is in use).
static void eeprom_probe() {
  if (eeprom_use_i2c(cfg_i2c_hz)) g_i2c_mask = eeprom_scan();
  else Serial.println("[!] Could not start I2C on the clip pins");
  if (cur_family() != CHIP_FAMILY_I2C_EEPROM) {
    eeprom_use_spi();
    g_spi_ee_sr = eeprom_spi_status();
  }
}

static void print_i2c_scan() {
  if (!g_i2c_mask) { Serial.println("    I2C scan: nothing answers at 0x50..0x57"); return; }
  Serial.print("    I2C scan: answers at");
  for (int i = 0; i < 8; i++)
    if (g_i2c_mask >> i & 1) Serial.printf(" 0x%02X", 0x50 + i);
  Serial.println();
}

// The detected ID as a chip of `family` reports it.
static const uint8_t *id_view(uint8_t family) {
  return family == CHIP_FAMILY_SPI_NOR ? g_nor_id : g_id;
}

// Could resident entry `t` be the chip in the socket? Same family view of
// (mfr, dev), and the same dev2 if the entry declares one.
static bool is_candidate(const active_profile_t *t) {
  if (t->family != g_detect_family) return false;
  const uint8_t *id = id_view(t->family);
  if (t->id_mfr != id[0] || t->id_dev != id[1]) return false;
  return !(t->id_flags & NAND_PROFILE_ID_HAS_DEV2) || t->id_dev2 == id[2];
}

// Resident chips that share the detected ID. More than one means the ID alone
// can't say which part this is (design § 5).
static unsigned id_candidates() {
  unsigned n, c = 0;
  const active_profile_t *t = nand_profile_resident(&n);
  for (unsigned i = 0; i < n; i++)
    if (is_candidate(&t[i])) c++;
  return c;
}

// SPI NOR manual size from the JEDEC capacity byte, where it follows the
// common 2^n convention (Winbond, Macronix, GigaDevice: 0x18 = 16 MiB).
static uint32_t nor_size_from_id(uint8_t cap) {
  return cap >= 0x10 && cap <= 0x1C ? 1u << cap : 1u << 20;
}

// Standalone tiebreak: list the resident chips sharing this ID, take the
// user's pick, and remember it in NVS for this (mfr, dev).
static void choose_chip() {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  const active_profile_t *cand[16];
  unsigned k = 0;
  for (unsigned i = 0; i < n && k < 16; i++)
    if (is_candidate(&t[i])) {
      cand[k] = &t[i];
      Serial.printf("    [%u] %s: %u blocks x %u pages x %u B, %u plane(s)\n", k + 1,
                    cand[k]->name, (unsigned)cand[k]->total_blocks,
                    (unsigned)cand[k]->pages_per_block, (unsigned)cand[k]->page_size,
                    cand[k]->planes);
      k++;
    }
  Serial.print("  Pick> ");
  int q = read_serial_line().toInt();
  if (q < 1 || q > (int)k) { Serial.println("  [!] Invalid choice"); return; }
  if (nand_profile_check(cand[q - 1], MAX_PAGE_SIZE) != NAND_PRF_OK) {
    Serial.println("  [!] That profile failed its sanity checks"); return;
  }
  adopt_profile(cand[q - 1], NAND_CHIP_RESIDENT);
  cfg_read_mode = (nand_read_mode_t)g_active.read_mode;
  nand_chip_choice_t ch = {};
  ch.mfr = id_view(g_active.family)[0]; ch.dev = id_view(g_active.family)[1];
  strncpy(ch.name, g_active.name, sizeof(ch.name) - 1);
  config_save_chip_choice(&ch);
  Serial.printf("  Chip: %s (saved for this ID)\n", g_active.name);
}

// Serial EEPROMs have no ID: list the resident parts of the chosen family and
// take the user's pick (remembered in NVS under the family, mfr 0).
static void pick_eeprom() {
  uint8_t fam = cur_family();
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  const active_profile_t *cand[48];
  unsigned k = 0;
  if (fam == CHIP_FAMILY_I2C_EEPROM) print_i2c_scan();
  else Serial.printf("    SPI status (RDSR): 0x%02X%s\n", g_spi_ee_sr,
                     eeprom_spi_status_plausible(g_spi_ee_sr) ? "" : " (no 25xx answers)");
  for (unsigned i = 0; i < n && k < 48; i++) {
    if (t[i].family != fam) continue;
    cand[k] = &t[i];
    bool fits = fam != CHIP_FAMILY_I2C_EEPROM || eeprom_i2c_base(&t[i], g_i2c_mask) >= 0;
    Serial.printf("    [%2u] %-10s %7u B%s\n", k + 1, t[i].name,
                  (unsigned)nand_profile_bytes(&t[i]), fits ? "" : "   (does not fit the scan)");
    k++;
  }
  Serial.print("  Pick> ");
  int q = read_serial_line().toInt();
  if (q < 1 || q > (int)k) { Serial.println("  [!] Invalid choice"); return; }
  const active_profile_t *p = cand[q - 1];
  if (nand_profile_check(p, MAX_PAGE_SIZE) != NAND_PRF_OK) {
    Serial.println("  [!] That profile failed its sanity checks"); return;
  }
  if (!eeprom_bound(p)) {
    Serial.println(fam == CHIP_FAMILY_I2C_EEPROM
                   ? "  [!] The part does not answer at every address it occupies; check the"
                     " clip, VCC and SDA/SCL pull-ups, then [R] to rescan"
                   : "  [!] No SPI EEPROM answers (status reads 0xFF); check the clip and VCC,"
                     " then [R] to rescan");
    return;
  }
  adopt_profile(p, NAND_CHIP_PICKED);
  nand_chip_choice_t ch = {};
  ch.mfr = 0; ch.dev = fam;                 // no ID: keyed by family
  strncpy(ch.name, p->name, sizeof(ch.name) - 1);
  config_save_chip_choice(&ch);
  Serial.printf("  Part: %s (saved)\n", p->name);
}

// ---- Serial helpers ----

// Read a line from Serial (blocking, with echo). Returns the trimmed string.
String read_serial_line() {
  String line = "";
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        Serial.println();
        delay(5);
        while (Serial.available()) {
          char next = Serial.peek();
          if (next == '\r' || next == '\n') Serial.read();
          else break;
        }
        break;
      } else if (c == 127 || c == 8) {  // backspace
        if (line.length() > 0) {
          line.remove(line.length() - 1);
          Serial.print("\b \b");
        }
      } else {
        line += c;
        Serial.print(c);
      }
    }
    delay(1);
  }
  line.trim();
  return line;
}

String mask_password(const char *pass) {
  int len = strlen(pass);
  if (len == 0) return "(empty)";
  if (len <= 2) return "**";
  String masked = "";
  masked += pass[0];
  for (int i = 1; i < len - 1; i++) masked += '*';
  masked += pass[len - 1];
  return masked;
}

static int log2_int(int v) {
  int b = 0;
  while ((1 << (b + 1)) <= v) b++;
  return b;
}

void show_menu() {
  int total_pages = cfg_total_blocks * cfg_pages_per_block;
  float total_mb = (float)total_pages * cfg_page_size / (1024.0 * 1024.0);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  ESP32 SPI Flash Dumper v3.1.1 — Config");
  Serial.println("========================================");
  const uint8_t *id = id_view(cur_family());
  const char *fam = family_name(cur_family());
  bool ee = chip_family_is_eeprom(cur_family());
  if (ee && g_known)
    Serial.printf("  Part:     %s %s (picked; serial EEPROMs have no ID)\n", fam, g_active.name);
  else if (ee)
    Serial.printf("  Part:     %s, not picked yet: [P] picks it (or dump.py --chip)\n", fam);
  else if (g_known) Serial.printf("  Detected: %s %s (0x%02X 0x%02X 0x%02X)%s\n", fam,
                             g_active.name, id[0], id[1], id[2],
                             g_chip_state == NAND_CHIP_PUSHED ? " — pushed by host"
                             : g_chip_state == NAND_CHIP_SFDP ? " — from its SFDP table" : "");
  else         Serial.printf("  Detected: UNKNOWN (NAND view 0x%02X 0x%02X, NOR view 0x%02X 0x%02X 0x%02X)"
                             " — manual %s geometry\n",
                             g_id[0], g_id[1], g_nor_id[0], g_nor_id[1], g_nor_id[2], fam);
  if (!g_known || g_chip_state == NAND_CHIP_PICKED)
    Serial.printf("  [F] Family:          %s (no ID; cycles NAND/NOR/I2C EEPROM/SPI EEPROM)\n", fam);
  if (id_candidates() > 1)
    Serial.printf("  [C] Choose chip:     %u resident chips share this ID\n", id_candidates());
  Serial.println("  -- Network --");
  Serial.printf( "  [1] WiFi SSID:       %s\n", cfg_ssid);
  Serial.printf( "  [2] WiFi Password:   %s\n", mask_password(cfg_pass).c_str());
  Serial.printf( "  [3] TCP Port:        %u\n", cfg_tcp_port);
  if (ee) {
    Serial.println("  -- Serial EEPROM --");
    Serial.printf( "  [P] Pick part:       %s\n", g_known ? g_active.name : "(none)");
    if (cur_family() == CHIP_FAMILY_I2C_EEPROM) {
      Serial.printf("  [R] Rescan I2C:      mask 0x%02X", g_i2c_mask);
      if (g_known) Serial.printf(", part at 0x%02X", g_i2c_base);
      Serial.println();
      Serial.printf("  [4] I2C Clock (Hz):  %u\n", (unsigned)cfg_i2c_hz);
    } else {
      Serial.printf("  [R] Re-read status:  RDSR 0x%02X\n", g_spi_ee_sr);
      Serial.printf("  [4] SPI Clock (Hz):  %d\n", cfg_spi_clock_hz);
    }
    if (g_known)
      Serial.printf("       Size:           %u bytes, %u-byte address%s\n",
                    (unsigned)nand_profile_bytes(&g_active), g_active.addr_bytes,
                    g_active.dev_addr_bits ? " + carried bits" : "");
    Serial.printf( "  [6] Verify Reads:    %s\n", cfg_verify ? "ON" : "OFF");
    Serial.printf( "  [7] Max Retries:     %d\n", cfg_max_retries);
    Serial.println("  ----------------------------------------");
    Serial.println("  [S] START dump with above settings");
    Serial.println("========================================");
    Serial.print(  "  Select> ");
    return;
  }
  Serial.println("  -- SPI --");
  Serial.printf( "  [4] SPI Clock (Hz):  %d\n", cfg_spi_clock_hz);
  Serial.printf( "  [5] Read Mode:       %s\n",
                 cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1");
  Serial.printf( "  [6] Verify Reads:    %s\n", cfg_verify ? "ON" : "OFF");
  Serial.printf( "  [7] Max Retries:     %d\n", cfg_max_retries);
  Serial.printf( "  [B] Batch pages/write: %d %s\n", cfg_batch_pages,
                 cfg_batch_pages == 1 ? "(per-page)" : "(coalesced)");
  bool nor = g_known ? is_nor() : cfg_family == CHIP_FAMILY_SPI_NOR;
  if (nor) {
    Serial.println("  [E] ECC on read:     n/a (SPI NOR has no on-die ECC)");
    Serial.println("  -- NOR Geometry --");
    if (g_known)
      Serial.printf("       Size:           %.2f MB, %u-byte address\n",
                    nand_profile_bytes(&g_active) / (1024.0 * 1024.0), g_active.addr_bytes);
    else
      Serial.printf("  [8] Size:            %u KiB\n", (unsigned)(cfg_nor_size >> 10));
  } else {
    Serial.printf( "  [E] ECC on read:     %s\n", cfg_ecc_on ? "ON (corrected)" : "OFF (raw)");
    Serial.println("  -- NAND Geometry --");
    Serial.printf( "  [8] Page Size:       %d bytes (spare %d)\n", cfg_page_size, cfg_spare_size);
    Serial.printf( "  [9] Pages/Block:     %d\n", cfg_pages_per_block);
    Serial.printf( "  [0] Total Blocks:    %d\n", cfg_total_blocks);
    Serial.printf( "       Total:          %d pages, %.1f MB\n", total_pages, total_mb);
  }
  Serial.println("  ----------------------------------------");
  Serial.println("  [S] START dump with above settings");
  Serial.println("========================================");
  Serial.print(  "  Select> ");
}

void config_menu() {
  show_menu();

  while (true) {
    String input = read_serial_line();
    if (input.length() == 0) { Serial.print("  Select> "); continue; }

    char choice = toupper(input.charAt(0));

    if (choice == 'S') {
      if (chip_family_is_eeprom(cur_family()) && !g_known)
        Serial.println("[!] No EEPROM part picked: dump.py --chip NAME must push one before a dump.");
      Serial.println("[*] Starting with current settings...");
      return;
    }

    // Serial EEPROM: pick the part, rescan, I2C clock; no ECC/quad/geometry.
    if (chip_family_is_eeprom(cur_family())) {
      bool i2c = cur_family() == CHIP_FAMILY_I2C_EEPROM;
      if (choice == 'P') { pick_eeprom(); show_menu(); continue; }
      if (choice == 'R') {
        eeprom_probe();
        if (i2c) print_i2c_scan();
        else Serial.printf("  RDSR: 0x%02X\n", g_spi_ee_sr);
        show_menu();
        continue;
      }
      if (choice == '4' && i2c) {
        cfg_i2c_hz = cfg_i2c_hz == 100000 ? 400000 : 100000;
        Serial.printf("  I2C clock: %u Hz\n", (unsigned)cfg_i2c_hz);
        show_menu();
        continue;
      }
      if (choice == 'E' || choice == '5' || choice == '8' || choice == '9' || choice == '0' ||
          choice == 'B' || choice == 'C') {
        Serial.println("  [!] Not applicable to a serial EEPROM");
        show_menu();
        continue;
      }
    }

    // SPI NOR: no ECC, and geometry is one size (from the profile when known).
    bool nor = g_known ? is_nor() : cfg_family == CHIP_FAMILY_SPI_NOR;
    if (nor && (choice == 'E' || choice == '9' || choice == '0' || (choice == '8' && g_known))) {
      Serial.println("  [!] Not applicable to this SPI NOR chip");
      show_menu();
      continue;
    }
    if (nor && choice == '8') {
      Serial.print("  Enter size in KiB (power of 2, 64-262144): ");
      int q = read_serial_line().toInt();
      if (q >= 64 && q <= 262144 && (q & (q - 1)) == 0) cfg_nor_size = (uint32_t)q << 10;
      else Serial.println("  [!] Invalid");
      show_menu();
      continue;
    }

    switch (choice) {
      case 'F':
        if (g_known && g_chip_state != NAND_CHIP_PICKED) {
          Serial.println("  [!] The chip was identified; family is fixed"); break;
        }
        // A picked EEPROM is only a choice, not an identification: drop it.
        cfg_family = (uint8_t)((cur_family() + 1) % CHIP_FAMILY_COUNT);
        g_known = false;
        g_chip_state = NAND_CHIP_UNKNOWN;
        if (chip_family_is_eeprom(cfg_family)) eeprom_probe();
        break;
      case 'C':
        if (id_candidates() > 1) choose_chip();
        else Serial.println("  [!] Only one chip matches this ID");
        break;
      case '1':
        Serial.print("  Enter new SSID: ");
        { String v = read_serial_line();
          if (v.length() > 0) strncpy(cfg_ssid, v.c_str(), sizeof(cfg_ssid) - 1); }
        break;
      case '2':
        Serial.print("  Enter new Password: ");
        { String v = read_serial_line();
          if (v.length() > 0) strncpy(cfg_pass, v.c_str(), sizeof(cfg_pass) - 1); }
        break;
      case '3':
        Serial.print("  Enter new TCP Port: ");
        { String v = read_serial_line(); int p = v.toInt();
          if (p > 0 && p <= 65535) cfg_tcp_port = (uint16_t)p;
          else Serial.println("  [!] Invalid port (1-65535)"); }
        break;
      case '4':
        Serial.println("  SPI Clock presets:");
        Serial.println("    [a] 1 MHz   [b] 5 MHz   [c] 10 MHz");
        Serial.println("    [d] 20 MHz  [e] 40 MHz  [x] Custom");
        Serial.print("  Pick> ");
        { String v = read_serial_line(); char p = toupper(v.charAt(0));
          switch (p) {
            case 'A': cfg_spi_clock_hz = 1000000; break;
            case 'B': cfg_spi_clock_hz = 5000000; break;
            case 'C': cfg_spi_clock_hz = 10000000; break;
            case 'D': cfg_spi_clock_hz = 20000000; break;
            case 'E': cfg_spi_clock_hz = 40000000; break;
            case 'X': Serial.print("  Enter Hz: ");
              { String hz = read_serial_line(); int q = hz.toInt();
                if (q > 0) cfg_spi_clock_hz = q; else Serial.println("  [!] Invalid"); }
              break;
            default: Serial.println("  [!] Invalid choice"); break;
          } }
        break;
      case '5':
        cfg_read_mode = (cfg_read_mode == NAND_READ_QUAD) ? NAND_READ_SINGLE : NAND_READ_QUAD;
        Serial.printf("  Read mode: %s\n", cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1");
        break;
      case '6':
        cfg_verify = !cfg_verify;
        Serial.printf("  Verify reads: %s\n", cfg_verify ? "ON" : "OFF");
        break;
      case '7':
        Serial.print("  Enter max retries: ");
        { String v = read_serial_line(); int q = v.toInt();
          if (q >= 0 && q <= 100) cfg_max_retries = q; else Serial.println("  [!] Invalid (0-100)"); }
        break;
      case 'B':
        Serial.printf("  Enter pages per TCP write (1-%d; 1=per-page): ", CONFIG_BATCH_MAX);
        { String v = read_serial_line(); int q = v.toInt();
          if (q >= 1 && q <= CONFIG_BATCH_MAX) cfg_batch_pages = q;
          else Serial.printf("  [!] Invalid (1-%d)\n", CONFIG_BATCH_MAX); }
        break;
      case 'E':
        cfg_ecc_on = !cfg_ecc_on;
        Serial.printf("  ECC on read: %s\n", cfg_ecc_on ? "ON (corrected)" : "OFF (raw)");
        break;
      case '8':
        Serial.println("  Page size presets:");
        Serial.println("    [a] 2112 (2048+64)   [b] 2176 (2048+128)");
        Serial.println("    [c] 4320 (4096+224)  [x] Custom");
        Serial.print("  Pick> ");
        { String v = read_serial_line(); char p = toupper(v.charAt(0));
          switch (p) {
            case 'A': cfg_page_size = 2112; cfg_spare_size = 64;  break;
            case 'B': cfg_page_size = 2176; cfg_spare_size = 128; break;
            case 'C': cfg_page_size = 4320; cfg_spare_size = 224; break;
            case 'X': Serial.print("  Enter page size (bytes): ");
              { String sz = read_serial_line(); int q = sz.toInt();
                if (q > 0 && q <= MAX_PAGE_SIZE) cfg_page_size = q;
                else Serial.println("  [!] Invalid"); }
              Serial.print("  Enter spare size (bytes): ");
              { String sz = read_serial_line(); int q = sz.toInt();
                if (q >= 0 && q < cfg_page_size) cfg_spare_size = q;
                else Serial.println("  [!] Invalid"); }
              break;
            default: Serial.println("  [!] Invalid choice"); break;
          } }
        break;
      case '9':
        Serial.print("  Enter pages per block (power of 2): ");
        { String v = read_serial_line(); int q = v.toInt();
          if (q > 0 && q <= 256 && (q & (q - 1)) == 0) {
            cfg_pages_per_block = q; cfg_page_addr_bits = log2_int(q);
          } else Serial.println("  [!] Invalid (power of 2, 1-256)"); }
        break;
      case '0':
        Serial.print("  Enter total blocks: ");
        { String v = read_serial_line(); int q = v.toInt();
          if (q > 0 && q <= 65535) cfg_total_blocks = q; else Serial.println("  [!] Invalid"); }
        break;
      default:
        Serial.println("  [!] Unknown option");
        break;
    }
    show_menu();
  }
}

// SPI NOR half of apply_runtime_settings: opcodes/address width from the
// profile, then the quad self-test. The firmware never sets the QE bit (on
// most parts it is a non-volatile status write); it only reads it to explain
// a failed self-test.
static void apply_nor_settings() {
  cfg_ecc_on = false;
  nor_apply_profile(&g_active);
  nor_set_read_mode(cfg_read_mode);
  if (cfg_read_mode == NAND_READ_QUAD) {
    int qe = nor_qe_state();
    uint32_t probe = nand_profile_bytes(&g_active) > 2 * (uint32_t)cfg_page_size
                         ? (uint32_t)cfg_page_size : 0;
    if (!nor_quad_selftest(probe, cfg_page_size)) {
      Serial.printf("[!] Quad self-test FAILED%s — falling back to single x1\n",
                    qe == 0 ? " (QE bit is clear; this read-only build never sets it)" : "");
      cfg_read_mode = NAND_READ_SINGLE;
      nor_set_read_mode(NAND_READ_SINGLE);
    } else {
      Serial.println("[+] Quad self-test passed");
    }
  }
  static const char *addr4[] = {"", ", 4-byte opcodes", ", B7h 4-byte mode",
                                ", WREN+B7h 4-byte mode"};
  Serial.printf("[*] SPI NOR %s: %.2f MB, %u-byte address%s | Read: %s | Clock: %d Hz\n",
                g_active.name, nand_profile_bytes(&g_active) / (1024.0 * 1024.0),
                g_active.addr_bytes, addr4[g_active.addr4_mode & 3],
                cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1", cfg_spi_clock_hz);
}

// Serial EEPROM half of apply_runtime_settings. I2C: the clip's pins go to the
// I2C controller and the scan says where the part answers. SPI: the status
// register says whether a 25xx is there. Single line, no ECC, read-only.
static void apply_eeprom_settings() {
  cfg_ecc_on = false;
  cfg_read_mode = NAND_READ_SINGLE;
  if (cur_family() == CHIP_FAMILY_I2C_EEPROM) {
    if (!eeprom_use_i2c(cfg_i2c_hz)) { Serial.println("[!] I2C start failed"); return; }
    g_i2c_mask = eeprom_scan();
    if (!g_known) { print_i2c_scan(); return; }
    int base = eeprom_i2c_base(&g_active, g_i2c_mask);
    g_i2c_base = base < 0 ? EEPROM_I2C_BASE : (uint8_t)base;
    eeprom_apply_profile(&g_active, g_i2c_base);
    if (base < 0) {
      print_i2c_scan();
      Serial.printf("[!] %s does not answer at every address it occupies: reads will fail\n",
                    g_active.name);
    }
    Serial.printf("[*] I2C EEPROM %s at 0x%02X: %u bytes, %u-byte address | Clock: %u Hz\n",
                  g_active.name, g_i2c_base, (unsigned)nand_profile_bytes(&g_active),
                  g_active.addr_bytes, (unsigned)cfg_i2c_hz);
    return;
  }
  g_spi_ee_sr = eeprom_spi_status();
  if (!g_known) return;
  eeprom_apply_profile(&g_active, 0);
  if (!eeprom_spi_status_plausible(g_spi_ee_sr))
    Serial.printf("[!] Status reads 0x%02X: no 25xx seems to answer\n", g_spi_ee_sr);
  Serial.printf("[*] SPI EEPROM %s: %u bytes, %u-byte address%s | Clock: %d Hz\n",
                g_active.name, (unsigned)nand_profile_bytes(&g_active), g_active.addr_bytes,
                g_active.dev_addr_bits ? " (A8 in the opcode)" : "", cfg_spi_clock_hz);
  if (cfg_spi_clock_hz > 5000000)
    Serial.println("[~] Many 25xx parts are rated 5 MHz or less at 3.3 V; lower [4] if reads retry");
}

// Apply the user's SPI clock, read mode, and ECC to the chip, and (if quad is
// selected) run the quad self-test with fallback. Called after the initial menu
// and after every [M] reconfigure. The chip retains these settings across
// dumps, so no reset is needed between dumps.
void apply_runtime_settings() {
  // An I2C EEPROM borrows the clip's pins from the SPI bus; everything else
  // needs the SPI bus back.
  if (cur_family() == CHIP_FAMILY_I2C_EEPROM) { apply_eeprom_settings(); return; }
  eeprom_use_spi();
  // The device was created at 1 MHz for safe detection; re-clock it to the
  // user-selected speed. Without this the bus stays at 1 MHz (the old bug).
  esp_err_t clk_ret = nand_set_clock(cfg_spi_clock_hz);
  if (clk_ret == ESP_OK)
    Serial.printf("[*] SPI clock applied: %d Hz\n", cfg_spi_clock_hz);
  else
    Serial.printf("[!] SPI clock change failed (%d) — staying at init speed\n", clk_ret);

  // An unknown chip runs on a manual profile that follows the menu geometry.
  if (!g_known) {
    if (cfg_family == CHIP_FAMILY_SPI_NOR) {
      nand_profile_manual_nor(&g_active, cfg_nor_size);
      cfg_page_size = g_active.page_size; cfg_spare_size = 0;
      cfg_pages_per_block = g_active.pages_per_block; cfg_total_blocks = g_active.total_blocks;
      cfg_page_addr_bits = log2_int(cfg_pages_per_block); cfg_planes = 1;
    } else if (cfg_family == CHIP_FAMILY_SPI_EEPROM) {
      apply_eeprom_settings();        // no part picked: nothing to build
      return;
    } else {
      nand_profile_manual(&g_active, cfg_page_size, cfg_spare_size, cfg_pages_per_block,
                          cfg_total_blocks, cfg_planes);
    }
  }
  if (is_nor()) { apply_nor_settings(); return; }
  if (g_active.family == CHIP_FAMILY_SPI_EEPROM) { apply_eeprom_settings(); return; }
  nand_apply_profile(&g_active);

  nand_set_read_mode(cfg_read_mode);
  nand_set_ecc(cfg_ecc_on);
  // Before any page read: multi-plane dies need the plane bit on every cache
  // read, or odd blocks come back as the other plane's cache contents.
  nand_set_plane_config(cfg_planes, cfg_page_addr_bits, cfg_page_size - cfg_spare_size);

  // Quad self-test: verify a quad read matches a single read, else fall back.
  if (cfg_read_mode == NAND_READ_QUAD) {
    if (g_active.qe_addr)   // quad-enable bit from the profile (DS35: B0h bit 0)
      nand_set_feature(g_active.qe_addr, nand_get_feature(g_active.qe_addr) | g_active.qe_bit);
    uint32_t probe = nand_row_addr(1, 0, cfg_page_addr_bits);
    if (!nand_quad_selftest(probe, cfg_page_size)) {
      Serial.println("[!] Quad self-test FAILED — falling back to single x1");
      cfg_read_mode = NAND_READ_SINGLE;
      nand_set_read_mode(NAND_READ_SINGLE);
    } else {
      Serial.println("[+] Quad self-test passed");
    }
  }

  Serial.printf("[*] Geometry: %d blocks x %d pages x %d bytes = %.1f MB (%d plane%s)\n",
                cfg_total_blocks, cfg_pages_per_block, cfg_page_size,
                (float)cfg_total_blocks * cfg_pages_per_block * cfg_page_size / (1024.0 * 1024.0),
                cfg_planes, cfg_planes == 1 ? "" : "s");
  Serial.printf("[*] ECC: %s | Read: %s | Clock: %d Hz\n",
                cfg_ecc_on ? "ON" : "OFF",
                cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1",
                cfg_spi_clock_hz);
}

// Load persisted settings from NVS over the current globals. Called AFTER chip
// detection so saved behaviour (clock/read/verify/ecc/WiFi) overrides the chip
// defaults, while geometry stays whatever detection set. On first boot (nothing
// saved) the globals keep their detected/compiled values.
void load_persisted_config() {
  nand_app_config_t c;
  config_defaults(&c);
  if (config_load(&c)) {
    strncpy(cfg_ssid, c.ssid, sizeof(cfg_ssid) - 1); cfg_ssid[sizeof(cfg_ssid) - 1] = '\0';
    strncpy(cfg_pass, c.pass, sizeof(cfg_pass) - 1); cfg_pass[sizeof(cfg_pass) - 1] = '\0';
    cfg_tcp_port     = c.tcp_port;
    cfg_spi_clock_hz = c.spi_clock_hz;
    cfg_read_mode    = (nand_read_mode_t)c.read_mode;
    cfg_verify       = c.verify;
    cfg_ecc_on       = c.ecc_on;
    cfg_max_retries  = c.max_retries;
    cfg_batch_pages  = c.batch_pages;
    Serial.println("[*] Loaded saved settings from NVS.");
  } else {
    Serial.println("[*] No saved settings yet — enter WiFi in the menu; it is saved on 'S'.");
  }
}

// Snapshot the current globals into NVS. Called whenever the menu is left.
void persist_config() {
  nand_app_config_t c;
  memset(&c, 0, sizeof(c));
  strncpy(c.ssid, cfg_ssid, sizeof(c.ssid) - 1);
  strncpy(c.pass, cfg_pass, sizeof(c.pass) - 1);
  c.tcp_port     = cfg_tcp_port;
  c.spi_clock_hz = cfg_spi_clock_hz;
  c.read_mode    = (uint8_t)cfg_read_mode;
  c.verify       = cfg_verify;
  c.ecc_on       = cfg_ecc_on;
  c.max_retries  = cfg_max_retries;
  c.batch_pages  = cfg_batch_pages;
  config_validate(&c);
  config_save(&c);
  Serial.println("[*] Settings saved to NVS.");
}

// Attempt to bring WiFi + the TCP server up from the current credentials.
// Safe to call again after a failure (only re-inits while not already ready).
bool bring_up_wifi() {
  if (wifi_transport_ready()) return true;
  wifi_transport_config_t wifi_cfg = { .ssid = cfg_ssid, .password = cfg_pass, .port = cfg_tcp_port };
  return wifi_transport_init(&wifi_cfg);
}

// Make `p` the active profile and take its geometry. A pushed profile keeps
// the user's read mode and ECC choices, exactly like a resident one does after
// the NVS settings load.
static void adopt_profile(const active_profile_t *p, nand_chip_state_t state) {
  g_active = *p;
  g_known = true;
  g_chip_state = state;
  cfg_page_size       = g_active.page_size;
  cfg_spare_size      = g_active.spare_size;
  cfg_pages_per_block = g_active.pages_per_block;
  cfg_total_blocks    = g_active.total_blocks;
  cfg_page_addr_bits  = log2_int(g_active.pages_per_block);
  cfg_planes          = g_active.planes;
  if (g_active.family != CHIP_FAMILY_SPI_NAND) cfg_ecc_on = false;
  if (chip_family_is_eeprom(g_active.family)) cfg_read_mode = NAND_READ_SINGLE;
  if (g_active.vcc_mv && g_active.vcc_mv < 3000)
    Serial.printf("[!] %s is a %u mV part: the ESP32 drives 3.3 V. Use a level shifter.\n",
                  g_active.name, g_active.vcc_mv);
}

static size_t link_read(void *, uint8_t *buf, size_t n, uint32_t timeout_ms) {
  return wifi_transport_read(buf, n, timeout_ms);
}
static size_t link_write(void *, const uint8_t *buf, size_t n) {
  return wifi_transport_send(buf, n);
}

// One client connection: answer I/P/A until G (dump) or the link drops.
// A bare 'G' goes straight to the dump, as before stage 3.
static void serve_session() {
  static const nand_link_t link = { link_read, link_write, NULL };
  g_session.max_page_size = MAX_PAGE_SIZE;
  g_session.timeout_ms = SESSION_PAYLOAD_MS;
  memcpy(g_session.id, g_id, sizeof(g_id));
  memcpy(g_session.nor_id, g_nor_id, sizeof(g_nor_id));
  g_session.i2c_ack_mask = g_i2c_mask;
  g_session.spi_ee_status = g_spi_ee_sr;
  nand_session_begin(&g_session);
  Serial.println("[*] Client connected; waiting for a command...");

  while (true) {
    g_session.chip_state = g_chip_state;
    g_session.active = &g_active;
    uint8_t cmd;
    if (wifi_transport_read(&cmd, 1, SESSION_IDLE_MS) != 1) {
      Serial.println("[!] Client idle or gone; closing.");
      wifi_transport_close();
      return;
    }
    switch (nand_session_handle(&g_session, &link, cmd)) {
      case NAND_SESS_MORE:
        if (cmd == NAND_CMD_PUSH)
          Serial.printf("[*] Profile push: %s\n",
                        g_session.has_staged ? "verified, waiting for ARM" : "rejected");
        break;
      case NAND_SESS_ARMED:
        adopt_profile(&g_session.armed, NAND_CHIP_PUSHED);
        Serial.printf("[+] Armed pushed profile %s (0x%02X 0x%02X)\n", g_active.name,
                      id_view(g_active.family)[0], id_view(g_active.family)[1]);
        apply_runtime_settings();
        break;
      case NAND_SESS_GO:
        if (chip_family_is_eeprom(cur_family()) && !g_known) {
          // No EEPROM part was picked or pushed: there is nothing to read with.
          uint8_t f[NAND_RESP_HDR_SIZE + 4];
          wifi_transport_send(f, nand_session_frame(f, NAND_CMD_GO, NAND_PRF_E_UNKNOWN_ID,
                                                    NULL, 0));
          Serial.println("[!] GO refused: no EEPROM part picked ([P] in the menu or dump.py --chip)");
          wifi_transport_close();
          return;
        }
        Serial.println("[*] GO received!");
        cmd_dump(cfg_verify);
        Serial.println("[+] Dump complete. Run dump.py again to re-dump, or 'M' to reconfigure.");
        return;
      case NAND_SESS_CLOSE:
        Serial.println("[!] Session closed after a refused command.");
        wifi_transport_close();
        return;
    }
  }
}

// Nothing answered READ ID: look for a serial EEPROM (they have no ID). The
// I2C scan and the SPI status read are both read-only. A part the user picked
// before (NVS, keyed by family) is taken again if it still answers. Returns
// true if an EEPROM was picked or seen, with cfg_family set to match.
static bool boot_eeprom() {
  cfg_family = CHIP_FAMILY_SPI_NAND;
  eeprom_probe();
  bool i2c = g_i2c_mask != 0, spi = eeprom_spi_status_plausible(g_spi_ee_sr);
  if (!i2c && !spi) return false;
  nand_chip_choice_t ch;
  if (config_load_chip_choice(&ch) && ch.mfr == 0 && chip_family_is_eeprom(ch.dev)) {
    unsigned n;
    const active_profile_t *p = nand_profile_by_name(nand_profile_resident(&n), n, ch.dev,
                                                     ch.name);
    if (p && nand_profile_check(p, MAX_PAGE_SIZE) == NAND_PRF_OK && eeprom_bound(p)) {
      adopt_profile(p, NAND_CHIP_PICKED);
      Serial.printf("[*] %s %s (saved pick; serial EEPROMs have no ID, [P] changes it)\n",
                    family_name(p->family), p->name);
      return true;
    }
  }
  g_chip_state = NAND_CHIP_UNKNOWN;
  cfg_family = i2c ? CHIP_FAMILY_I2C_EEPROM : CHIP_FAMILY_SPI_EEPROM;
  if (i2c) print_i2c_scan();
  else Serial.printf("    SPI status (RDSR) 0x%02X looks like a 25xx EEPROM\n", g_spi_ee_sr);
  Serial.printf("[!] No READ ID, but a %s seems to be there: pick it with [P] in the menu,"
                " or let dump.py --chip NAME push it.\n", family_name(cfg_family));
  return true;
}

// ============ SETUP ============

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println("  ESP32 SPI NAND Dumper v3.1.1");
  Serial.println("  Auto-detect + WiFi TCP");
  Serial.println("========================================");

  sys_info_report();   // runtime board capabilities (portable across ESP32 variants)

  // ---- Bring SPI up slow, detect the chip ----
  nand_config_t nand_cfg = NAND_DEFAULT_CONFIG();
  nand_cfg.clock_hz  = 1000000;
  nand_cfg.read_mode = NAND_READ_SINGLE;
  if (nand_init(&nand_cfg, MAX_PAGE_SIZE) != ESP_OK) {
    Serial.println("[!] NAND init failed"); return;
  }

  // Read the ID both ways (SPI NAND: 9Fh + dummy byte; SPI NOR: plain 9Fh,
  // after ABh in case the part is in deep power-down) and resolve each view
  // in its own family's resident entries (design § 5, chip_detect.h).
  nand_read_id(g_id);
  nor_init(MAX_PAGE_SIZE);
  nor_release_power_down();
  nor_read_id(g_nor_id);
  eeprom_driver_init(MAX_PAGE_SIZE);
  unsigned n_resident;
  const active_profile_t *table = nand_profile_resident(&n_resident);
  chip_detect_t det = chip_detect_resident(table, n_resident, g_id, g_nor_id);
  g_detect_family = det.family;
  const active_profile_t *hit = det.hit;
  nand_prf_err_t perr = det.err;
  const uint8_t *id = id_view(det.family);
  // Shared ID: a pick the user saved for this exact (mfr, dev) settles it.
  bool from_choice = false;
  if (!hit && det.state == NAND_CHIP_AMBIGUOUS) {
    nand_chip_choice_t ch;
    if (config_load_chip_choice(&ch) && ch.mfr == id[0] && ch.dev == id[1]) {
      hit = nand_profile_pick(table, n_resident, id[0], id[1], ch.name);
      from_choice = hit != NULL && is_candidate(hit);
      if (!from_choice) hit = NULL;
    }
  }
  // Re-assert the cheap sanity checks on the resident entry before trusting it.
  if (hit && (perr = nand_profile_check(hit, MAX_PAGE_SIZE)) != NAND_PRF_OK) hit = NULL;
  // A SPI NOR part in no table can still describe itself (JESD216 SFDP).
  active_profile_t sfdp_prof;
  sfdp_info_t sfdp;
  bool from_sfdp = !hit && det.state == NAND_CHIP_UNKNOWN &&
                   chip_detect_nor_id_plausible(g_nor_id) && nor_probe_sfdp(&sfdp) &&
                   sfdp_build_profile(&sfdp, g_nor_id, &sfdp_prof) &&
                   nand_profile_check(&sfdp_prof, MAX_PAGE_SIZE) == NAND_PRF_OK;
  if (hit || from_sfdp) {
    adopt_profile(hit ? hit : &sfdp_prof, hit ? NAND_CHIP_RESIDENT : NAND_CHIP_SFDP);
    cfg_read_mode = (nand_read_mode_t)g_active.read_mode;
    id = id_view(g_active.family);
    Serial.printf("[*] Detected %s %s (0x%02X 0x%02X 0x%02X)%s\n",
                  is_nor() ? "SPI NOR" : "SPI NAND", g_active.name, id[0], id[1], id[2],
                  from_choice ? " — saved choice for a shared ID; [C] in the menu changes it"
                  : from_sfdp ? " — profile built from its SFDP table" : "");
  } else if (det.state == NAND_CHIP_UNKNOWN && flat_id(g_id) &&
             !chip_detect_nor_id_plausible(g_nor_id) && boot_eeprom()) {
    // A serial EEPROM: picked from NVS, or at least seen answering.
  } else {
    g_chip_state = det.state == NAND_CHIP_AMBIGUOUS ? NAND_CHIP_AMBIGUOUS : NAND_CHIP_UNKNOWN;
    // A plausible NOR ID with no SFDP: most likely a pre-SFDP NOR part.
    if (det.state == NAND_CHIP_UNKNOWN && chip_detect_nor_id_plausible(g_nor_id)) {
      cfg_family = CHIP_FAMILY_SPI_NOR;
      cfg_nor_size = nor_size_from_id(g_nor_id[2]);
    } else {
      cfg_family = det.family;
    }
    Serial.printf("[!] %s for chip (NAND view 0x%02X 0x%02X 0x%02X, NOR view 0x%02X 0x%02X 0x%02X)"
                  " — using manual %s geometry\n", nand_prf_err_name(perr),
                  g_id[0], g_id[1], g_id[2], g_nor_id[0], g_nor_id[1], g_nor_id[2],
                  cfg_family == CHIP_FAMILY_SPI_NOR ? "SPI NOR" : "SPI NAND");
    if (det.state == NAND_CHIP_AMBIGUOUS)
      for (unsigned i = 0; i < n_resident; i++)
        if (is_candidate(&table[i]))
          Serial.printf("    candidate: %s\n", table[i].name);
    if (det.state == NAND_CHIP_AMBIGUOUS)
      Serial.println("    Pick one with [C] in the menu (saved for next boot), or let dump.py push it.");
    else
      Serial.println("    dump.py can push this chip's profile from the host database.");
  }

  // ---- Load saved settings (over detected defaults), then configure ----
  load_persisted_config();
  config_menu();
  persist_config();
  apply_runtime_settings();

  // ---- WiFi transport (brought up once; stays up across dumps) ----
  if (bring_up_wifi()) {
    Serial.println("[+] Ready. Run dump.py to start a dump.");
  } else {
    // Do NOT dead-end: loop() stays alive (client calls are inert until ready),
    // so the user can set credentials with 'M' and it retries — no reset needed.
    Serial.println("[!] WiFi not connected. Press 'M' to set SSID/password; it will retry.");
  }
  Serial.println("    Between dumps: press 'M' then Enter here to reconfigure (no reset needed).");
}

// Re-dumpable serve loop — dump on each new client, no ESP reset between dumps.
void loop() {
  static bool announced = false;
  if (!announced) {
    Serial.println("[*] Waiting for client (run dump.py)...");
    announced = true;
  }

  // Live reconfigure between dumps, over serial, without a reset.
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'M' || c == 'm') {
      while (Serial.available()) Serial.read();   // drain the rest of the line
      config_menu();
      persist_config();
      apply_runtime_settings();
      if (!wifi_transport_ready()) {
        // WiFi never came up (e.g. blank creds on a fresh board) — retry now.
        if (bring_up_wifi()) Serial.println("[+] WiFi connected. Run dump.py to start a dump.");
        else Serial.println("[!] Still not connected — check credentials and press 'M' again.");
      } else {
        Serial.println("    (SPI/geometry/ECC applied now; WiFi SSID/port changes still need a reset.)");
      }
      announced = false;
    }
  }

  // Serve each client connection: optional profile push, then 'G' dumps.
  // cmd_dump() closes the client at the end, so the next connection begins a
  // fresh session.
  if (wifi_transport_client_available()) {
    serve_session();
    announced = false;
  }

  delay(5);
}

// ---- Dump Command ----
void cmd_dump(bool verify) {
  bool nor = is_nor();
  bool ee = chip_family_is_eeprom(g_active.family);
  bool linear = nor || ee;      // one flat array read in units: no spare, ECC or BBM
  Serial.printf("[*] Starting full %s dump...\n", family_name(g_active.family));

  uint32_t total_pages = (uint32_t)cfg_total_blocks * cfg_pages_per_block;
  uint32_t total_bytes = total_pages * (uint32_t)cfg_page_size;

  // Each dump is a new read session: quad gets another chance even if the last
  // dump fell back to single.
  if (nor) nor_set_read_mode(cfg_read_mode);
  else if (!ee) nand_set_read_mode(cfg_read_mode);

  // Each SPI DMA read lands in a page-sized DMA-capable scratch buffer; completed
  // page-frames (data + 4-byte CRC seal) are copied into a larger batch buffer and
  // flushed to TCP in ONE write every `batch` pages. Batching cuts per-write
  // overhead; the batch size is the menu knob, clamped to what this board's free
  // heap can hold — computed at runtime, so the same binary self-sizes on any
  // ESP32 variant.
  size_t frame_size = (size_t)cfg_page_size + 4;
  int batch = sys_recommend_batch_pages(ESP.getFreeHeap(), frame_size, CONFIG_BATCH_MAX);
  if (cfg_batch_pages < batch) batch = cfg_batch_pages;
  if (batch < 1) batch = 1;

  uint8_t *scratch   = (uint8_t *)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
  uint8_t *batch_buf = (uint8_t *)malloc((size_t)batch * frame_size);
  while (!batch_buf && batch > 1) {           // back off if the heap can't hold it
    batch /= 2;
    batch_buf = (uint8_t *)malloc((size_t)batch * frame_size);
  }
  if (!scratch || !batch_buf) {
    Serial.println("[!] Buffer alloc failed!");
    free(scratch); free(batch_buf);
    return;
  }
  if (batch != cfg_batch_pages)
    Serial.printf("[*] Batch clamped to %d pages/write (memory).\n", batch);
  Serial.printf("[*] Streaming %d page(s)/write, frame %u B.\n", batch, (unsigned)frame_size);

  // ---- Send the 32-byte geometry header so the PC self-configures ----
  dump_geometry_t geo = {0};
  geo.page_size       = cfg_page_size;
  geo.spare_size      = cfg_spare_size;
  geo.pages_per_block = cfg_pages_per_block;
  geo.total_blocks    = cfg_total_blocks;
  geo.total_pages     = total_pages;
  geo.total_bytes     = total_bytes;
  geo.mfr_id          = ee ? 0 : id_view(g_active.family)[0];
  geo.dev_id          = ee ? 0 : id_view(g_active.family)[1];
  if (g_active.family == CHIP_FAMILY_I2C_EEPROM) geo.mfr_id = g_i2c_base;
  geo.page_addr_bits  = cfg_page_addr_bits;
  geo.flags = (cfg_ecc_on ? DUMP_FLAG_ECC_ON : 0)
            | (cfg_read_mode == NAND_READ_QUAD ? DUMP_FLAG_QUAD : 0)
            | (cfg_verify ? DUMP_FLAG_VERIFY : 0)
            | DUMP_FLAG_PAGECRC
            | (nor ? DUMP_FLAG_NOR : 0)
            | (ee ? DUMP_FLAG_EEPROM : 0)
            | (g_active.family == CHIP_FAMILY_I2C_EEPROM ? DUMP_FLAG_I2C : 0);
  uint8_t hdr[DUMP_HEADER_SIZE];
  dump_header_pack(hdr, &geo);
  if (wifi_transport_send(hdr, sizeof(hdr)) != sizeof(hdr)) {
    Serial.println("[!] Client dropped during header; aborting dump.");
    wifi_transport_close();
    free(scratch); free(batch_buf);
    return;
  }

  unsigned long startTime = millis();
  uint32_t pagesDone = 0, retryCount = 0, failedPages = 0;
  uint32_t eccUncorrectable = 0, eccRefresh = 0;   // meaningful only when ECC is on
  uint32_t singleRescued = 0, badBlocks = 0;
  bool blockMarkedBad = false;
  int fill = 0;   // page-frames currently held in batch_buf

  bool aborted = false;
  if (nor) nor_begin();   // 4-byte mode on B7h parts, undone by nor_end below
  for (int block = 0; block < cfg_total_blocks && !aborted; block++) {
    blockMarkedBad = false;
    for (int page = 0; page < cfg_pages_per_block; page++) {
      uint32_t row = nand_row_addr(block, page, cfg_page_addr_bits);

      if (ee) {
        // A serial EEPROM is one linear array too, read in units of page_size.
        uint32_t addr = (uint32_t)pagesDone * (uint32_t)cfg_page_size;
        if (verify) {
          if (eeprom_read_verified(addr, scratch, cfg_page_size, cfg_max_retries,
                                   &retryCount) == NAND_PAGE_UNSTABLE)
            failedPages++;
        } else if (!eeprom_read(addr, scratch, cfg_page_size)) {
          failedPages++;
        }
      } else if (nor) {
        // SPI NOR is one linear array: a "page" here is one read unit.
        uint32_t addr = (uint32_t)pagesDone * (uint32_t)cfg_page_size;
        if (verify) {
          nand_page_result_t r = nor_read_verified(addr, scratch, cfg_page_size,
                                                   cfg_max_retries, &retryCount);
          if (r == NAND_PAGE_UNSTABLE) failedPages++;
          else if (r == NAND_PAGE_OK_SINGLE) singleRescued++;
        } else {
          nor_read(addr, scratch, cfg_page_size);
        }
      } else if (verify) {
        nand_page_result_t r = nand_read_page_verified(row, scratch, cfg_page_size,
                                                       cfg_max_retries, &retryCount);
        if (r == NAND_PAGE_UNSTABLE) failedPages++;
        else if (r == NAND_PAGE_OK_SINGLE) singleRescued++;
      } else {
        nand_page_read_to_cache(row);
        nand_wait_ready();
        nand_read_cache(scratch, cfg_page_size);
      }

      // Factory bad-block marker: where, how wide and which polarity come from
      // the profile's bbm fields. Only meaningful on a raw (ECC off) read.
      if (!linear && !cfg_ecc_on && !blockMarkedBad &&
          nand_profile_is_bbm_page(&g_active, page, cfg_pages_per_block) &&
          nand_profile_marker_bad(&g_active, scratch, cfg_page_size, cfg_spare_size)) {
        blockMarkedBad = true;
        badBlocks++;
        if (badBlocks <= 20) Serial.printf("[BBM] block %d is marked bad\n", block);
      }

      // With ECC on, the chip corrects what it can and reports the outcome in
      // its status register. A CRC over corrected-or-not data can't reveal an
      // uncorrectable page, so surface it here — otherwise a damaged page passes
      // silently. The field and its meaning are vendor-specific, so the decode
      // is the profile's ecc_map. (The status is only valid with ECC enabled.)
      if (cfg_ecc_on && !linear) {
        nand_ecc_sev_t sev = nand_profile_ecc_severity(&g_active, nand_get_status());
        if (sev == NAND_ECC_UNCORRECTABLE) {
          eccUncorrectable++;
          if (eccUncorrectable <= 20)
            Serial.printf("[ECC] UNCORRECTABLE page %u (block %d, page %d)\n",
                          pagesDone, block, page);
        } else if (sev == NAND_ECC_CORRECTED_REFRESH) {
          eccRefresh++;
        }
      }

      // Seal the page: CRC32 over the data, appended little-endian. The PC
      // re-checks it, so ESP->PC corruption or truncation is caught and pinned
      // to this page instead of passing silently.
      uint32_t crc = dump_crc32(scratch, cfg_page_size);
      scratch[cfg_page_size + 0] = (uint8_t)crc;
      scratch[cfg_page_size + 1] = (uint8_t)(crc >> 8);
      scratch[cfg_page_size + 2] = (uint8_t)(crc >> 16);
      scratch[cfg_page_size + 3] = (uint8_t)(crc >> 24);

      memcpy(batch_buf + (size_t)fill * frame_size, scratch, frame_size);
      fill++;
      pagesDone++;

      if (fill == batch) {
        size_t n = (size_t)fill * frame_size;
        if (wifi_transport_send(batch_buf, n) != n) {
          Serial.printf("[!] Send failed near page %u (client gone?); aborting.\n", pagesDone);
          aborted = true;
          break;
        }
        fill = 0;
      }

      if (pagesDone % 1024 == 0) {
        float mb = (float)pagesDone * cfg_page_size / (1024.0 * 1024.0);
        float pct = (float)pagesDone / total_pages * 100.0;
        unsigned long el = (millis() - startTime) / 1000;
        float speed = el > 0 ? mb / el : 0;
        Serial.printf("[>] %u/%u (%.1f MB %.1f%%) %.2f MB/s retries:%u\n",
                      pagesDone, total_pages, mb, pct, speed, retryCount);
      }
    }
  }

  if (nor) nor_end();

  // Flush the final partial batch.
  if (!aborted && fill > 0) {
    size_t n = (size_t)fill * frame_size;
    if (wifi_transport_send(batch_buf, n) != n) {
      Serial.println("[!] Send failed on final flush; aborting.");
      aborted = true;
    }
  }

  wifi_transport_close();
  free(scratch);
  free(batch_buf);

  if (aborted) {
    Serial.printf("[!] Dump aborted after %u/%u pages (client disconnected).\n",
                  pagesDone, total_pages);
    return;
  }

  unsigned long elapsed = (millis() - startTime) / 1000;
  float totalMB = total_bytes / (1024.0 * 1024.0);
  Serial.printf("\n[+] Dump complete!\n");
  Serial.printf("[+] %.1f MB in %lu sec (%.2f MB/s)\n",
                totalMB, elapsed, elapsed > 0 ? totalMB / elapsed : 0);
  Serial.printf("[+] Retries: %u | Failed pages: %u\n", retryCount, failedPages);
  if (singleRescued)
    Serial.printf("[+] Quad fallback: %u page(s) re-read single%s\n", singleRescued,
                  (nor ? nor_get_read_mode() : nand_get_read_mode()) == NAND_READ_SINGLE &&
                  cfg_read_mode == NAND_READ_QUAD
                      ? "; the rest of the dump ran single" : "");
  if (!cfg_ecc_on && !linear)
    Serial.printf("[+] Bad-block markers (%s): %u block(s)\n", g_active.name, badBlocks);
  if (cfg_ecc_on) {
    Serial.printf("[+] ECC: %u uncorrectable, %u refresh-recommended pages\n",
                  eccUncorrectable, eccRefresh);
    if (eccUncorrectable > 20)
      Serial.printf("    (only the first 20 uncorrectable pages were listed above)\n");
  }
}
