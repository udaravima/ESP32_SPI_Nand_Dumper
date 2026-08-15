#include <Arduino.h>
#include "nand_driver.h"
#include "nand_chips.h"
#include "nand_addr.h"
#include "dump_header.h"
#include "wifi_transport.h"

#define MAX_PAGE_SIZE 8192  // upper bound for buffer/transfer sizing

// ============ RUNTIME CONFIG (defaults) ============
static char     cfg_ssid[64]     = "GAE";
static char     cfg_pass[64]     = "omgRoopa1!";
static uint16_t cfg_tcp_port     = 3333;
static int      cfg_spi_clock_hz = 1000000;    // 1 MHz
static nand_read_mode_t cfg_read_mode = NAND_READ_SINGLE;
static bool     cfg_verify       = true;
static int      cfg_max_retries  = 5;
// NAND geometry (filled from the detected chip; overridable in the menu)
static int      cfg_page_size       = 2176;
static int      cfg_spare_size       = 128;
static int      cfg_pages_per_block = 64;
static int      cfg_total_blocks    = 2048;
static int      cfg_page_addr_bits  = 6;
static uint8_t  cfg_bad_mark        = 0x00;
static bool     cfg_ecc_on          = false;   // global policy: OFF/raw
// Detected chip
static uint16_t g_chip_id = 0;
static const nand_chip_t *g_chip = NULL;
// ===================================================

void cmd_dump(bool verify);

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
  Serial.println("  ESP32 SPI NAND Dumper v3.0 — Config");
  Serial.println("========================================");
  if (g_chip) Serial.printf("  Detected: %s (0x%02X 0x%02X)\n",
                            g_chip->name, g_chip_id >> 8, g_chip_id & 0xFF);
  else        Serial.printf("  Detected: UNKNOWN (0x%02X 0x%02X) — manual geometry\n",
                            g_chip_id >> 8, g_chip_id & 0xFF);
  Serial.println("  -- Network --");
  Serial.printf( "  [1] WiFi SSID:       %s\n", cfg_ssid);
  Serial.printf( "  [2] WiFi Password:   %s\n", mask_password(cfg_pass).c_str());
  Serial.printf( "  [3] TCP Port:        %u\n", cfg_tcp_port);
  Serial.println("  -- SPI --");
  Serial.printf( "  [4] SPI Clock (Hz):  %d\n", cfg_spi_clock_hz);
  Serial.printf( "  [5] Read Mode:       %s\n",
                 cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1");
  Serial.printf( "  [6] Verify Reads:    %s\n", cfg_verify ? "ON" : "OFF");
  Serial.printf( "  [7] Max Retries:     %d\n", cfg_max_retries);
  Serial.printf( "  [E] ECC on read:     %s\n", cfg_ecc_on ? "ON (corrected)" : "OFF (raw)");
  Serial.println("  -- NAND Geometry --");
  Serial.printf( "  [8] Page Size:       %d bytes (spare %d)\n", cfg_page_size, cfg_spare_size);
  Serial.printf( "  [9] Pages/Block:     %d\n", cfg_pages_per_block);
  Serial.printf( "  [0] Total Blocks:    %d\n", cfg_total_blocks);
  Serial.printf( "       Total:          %d pages, %.1f MB\n", total_pages, total_mb);
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

    if (choice == 'S') { Serial.println("[*] Starting with current settings..."); return; }

    switch (choice) {
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

// ============ SETUP ============

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println("  ESP32 SPI NAND Dumper v3.0");
  Serial.println("  Auto-detect + WiFi TCP");
  Serial.println("========================================");

  // ---- Bring SPI up slow, detect the chip ----
  nand_config_t nand_cfg = NAND_DEFAULT_CONFIG();
  nand_cfg.clock_hz  = 1000000;
  nand_cfg.read_mode = NAND_READ_SINGLE;
  if (nand_init(&nand_cfg, MAX_PAGE_SIZE) != ESP_OK) {
    Serial.println("[!] NAND init failed"); return;
  }

  g_chip_id = nand_read_id();
  g_chip = nand_chip_lookup(g_chip_id >> 8, g_chip_id & 0xFF);
  if (g_chip) {
    Serial.printf("[*] Detected %s (0x%02X 0x%02X)\n",
                  g_chip->name, g_chip_id >> 8, g_chip_id & 0xFF);
    cfg_page_size       = g_chip->page_size;
    cfg_spare_size      = g_chip->spare_size;
    cfg_pages_per_block = g_chip->pages_per_block;
    cfg_total_blocks    = g_chip->total_blocks;
    cfg_page_addr_bits  = g_chip->page_addr_bits;
    cfg_bad_mark        = g_chip->bad_block_mark;
    cfg_ecc_on          = g_chip->ecc_default_on;
  } else {
    Serial.printf("[!] Unknown chip 0x%02X 0x%02X — using manual defaults\n",
                  g_chip_id >> 8, g_chip_id & 0xFF);
  }

  // ---- Interactive config (pre-filled) ----
  config_menu();

  // ---- Apply final SPI mode + ECC state ----
  nand_set_read_mode(cfg_read_mode);
  nand_set_ecc(cfg_ecc_on);

  // ---- Quad self-test: verify a quad read matches a single read, else fall back ----
  if (cfg_read_mode == NAND_READ_QUAD) {
    if (g_chip && g_chip->has_qe_bit)
      nand_set_feature(g_chip->qe_feature_addr,
                       nand_get_feature(g_chip->qe_feature_addr) | g_chip->qe_bit);
    uint32_t probe = nand_row_addr(1, 0, cfg_page_addr_bits);
    if (!nand_quad_selftest(probe, cfg_page_size)) {
      Serial.println("[!] Quad self-test FAILED — falling back to single x1");
      cfg_read_mode = NAND_READ_SINGLE;
      nand_set_read_mode(NAND_READ_SINGLE);
    } else {
      Serial.println("[+] Quad self-test passed");
    }
  }

  Serial.printf("[*] Geometry: %d blocks x %d pages x %d bytes = %.1f MB\n",
                cfg_total_blocks, cfg_pages_per_block, cfg_page_size,
                (float)cfg_total_blocks * cfg_pages_per_block * cfg_page_size / (1024.0 * 1024.0));
  Serial.printf("[*] ECC: %s | Read: %s | Clock: %d Hz\n",
                cfg_ecc_on ? "ON" : "OFF",
                cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1",
                cfg_spi_clock_hz);

  // ---- WiFi transport ----
  wifi_transport_config_t wifi_cfg = { .ssid = cfg_ssid, .password = cfg_pass, .port = cfg_tcp_port };
  if (!wifi_transport_init(&wifi_cfg)) { Serial.println("[!] WiFi init failed!"); return; }

  wifi_transport_wait_client();
  wifi_transport_wait_trigger();

  cmd_dump(cfg_verify);
}

void loop() {}

// ---- Dump Command ----
void cmd_dump(bool verify) {
  Serial.println("[*] Starting full NAND dump...");

  uint32_t total_pages = (uint32_t)cfg_total_blocks * cfg_pages_per_block;
  uint32_t total_bytes = total_pages * (uint32_t)cfg_page_size;

  uint8_t *page_buf = (uint8_t *)heap_caps_malloc(cfg_page_size, MALLOC_CAP_DMA);
  if (!page_buf) { Serial.println("[!] Buffer alloc failed!"); return; }

  // ---- Send the 32-byte geometry header so the PC self-configures ----
  dump_geometry_t geo = {0};
  geo.page_size       = cfg_page_size;
  geo.spare_size      = cfg_spare_size;
  geo.pages_per_block = cfg_pages_per_block;
  geo.total_blocks    = cfg_total_blocks;
  geo.total_pages     = total_pages;
  geo.total_bytes     = total_bytes;
  geo.mfr_id          = g_chip_id >> 8;
  geo.dev_id          = g_chip_id & 0xFF;
  geo.page_addr_bits  = cfg_page_addr_bits;
  geo.flags = (cfg_ecc_on ? DUMP_FLAG_ECC_ON : 0)
            | (cfg_read_mode == NAND_READ_QUAD ? DUMP_FLAG_QUAD : 0)
            | (cfg_verify ? DUMP_FLAG_VERIFY : 0);
  uint8_t hdr[DUMP_HEADER_SIZE];
  dump_header_pack(hdr, &geo);
  wifi_transport_send(hdr, sizeof(hdr));

  unsigned long startTime = millis();
  uint32_t pagesDone = 0, retryCount = 0, failedPages = 0;

  for (int block = 0; block < cfg_total_blocks; block++) {
    for (int page = 0; page < cfg_pages_per_block; page++) {
      uint32_t row = nand_row_addr(block, page, cfg_page_addr_bits);

      if (verify) {
        if (!nand_read_page_verified(row, page_buf, cfg_page_size, cfg_max_retries, &retryCount))
          failedPages++;
      } else {
        nand_page_read_to_cache(row);
        nand_wait_ready();
        nand_read_cache(page_buf, cfg_page_size);
      }

      wifi_transport_send(page_buf, cfg_page_size);
      pagesDone++;

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

  wifi_transport_close();
  free(page_buf);

  unsigned long elapsed = (millis() - startTime) / 1000;
  float totalMB = total_bytes / (1024.0 * 1024.0);
  Serial.printf("\n[+] Dump complete!\n");
  Serial.printf("[+] %.1f MB in %lu sec (%.2f MB/s)\n",
                totalMB, elapsed, elapsed > 0 ? totalMB / elapsed : 0);
  Serial.printf("[+] Retries: %u | Failed pages: %u\n", retryCount, failedPages);
}
