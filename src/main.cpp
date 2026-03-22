#include <Arduino.h>
#include "nand_driver.h"
#include "wifi_transport.h"

// ============ RUNTIME CONFIG (defaults) ============
static char     cfg_ssid[64]     = "GAE";
static char     cfg_pass[64]     = "omgRoopa1!";
static uint16_t cfg_tcp_port     = 3333;
static int      cfg_spi_clock_hz = 1000000;    // 1 MHz
static nand_read_mode_t cfg_read_mode = NAND_READ_SINGLE;
static bool     cfg_verify       = true;
static int      cfg_max_retries  = 5;
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
        Serial.println();  // echo newline
        // Drain any trailing \r or \n (handles \r\n, \n\r, etc.)
        delay(5);
        while (Serial.available()) {
          char next = Serial.peek();
          if (next == '\r' || next == '\n') {
            Serial.read();  // consume it
          } else {
            break;
          }
        }
        break;
      } else if (c == 127 || c == 8) {  // backspace
        if (line.length() > 0) {
          line.remove(line.length() - 1);
          Serial.print("\b \b");
        }
      } else {
        line += c;
        Serial.print(c);  // echo
      }
    }
    delay(1);
  }
  line.trim();
  return line;
}

// Mask a password string for display
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

void show_menu() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  ESP32 SPI NAND Dumper v2.1 — Config");
  Serial.println("========================================");
  Serial.printf( "  [1] WiFi SSID:      %s\n", cfg_ssid);
  Serial.printf( "  [2] WiFi Password:  %s\n", mask_password(cfg_pass).c_str());
  Serial.printf( "  [3] TCP Port:       %u\n", cfg_tcp_port);
  Serial.printf( "  [4] SPI Clock (Hz): %d\n", cfg_spi_clock_hz);
  Serial.printf( "  [5] Read Mode:      %s\n",
                 cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1");
  Serial.printf( "  [6] Verify Reads:   %s\n", cfg_verify ? "ON" : "OFF");
  Serial.printf( "  [7] Max Retries:    %d\n", cfg_max_retries);
  Serial.println("  ----------------------------------------");
  Serial.println("  [S] START dump with above settings");
  Serial.println("========================================");
  Serial.print(  "  Select> ");
}

void config_menu() {
  show_menu();

  while (true) {
    String input = read_serial_line();
    if (input.length() == 0) {
      Serial.print("  Select> ");
      continue;
    }

    char choice = toupper(input.charAt(0));

    if (choice == 'S') {
      Serial.println("[*] Starting with current settings...");
      return;
    }

    switch (choice) {
      case '1':
        Serial.print("  Enter new SSID: ");
        { String val = read_serial_line();
          if (val.length() > 0) strncpy(cfg_ssid, val.c_str(), sizeof(cfg_ssid) - 1);
        }
        break;

      case '2':
        Serial.print("  Enter new Password: ");
        { String val = read_serial_line();
          if (val.length() > 0) strncpy(cfg_pass, val.c_str(), sizeof(cfg_pass) - 1);
        }
        break;

      case '3':
        Serial.print("  Enter new TCP Port: ");
        { String val = read_serial_line();
          int v = val.toInt();
          if (v > 0 && v <= 65535) cfg_tcp_port = (uint16_t)v;
          else Serial.println("  [!] Invalid port (1-65535)");
        }
        break;

      case '4':
        Serial.println("  SPI Clock presets:");
        Serial.println("    [a] 1 MHz   [b] 5 MHz   [c] 10 MHz");
        Serial.println("    [d] 20 MHz  [e] 40 MHz  [x] Custom");
        Serial.print("  Pick> ");
        { String val = read_serial_line();
          char p = toupper(val.charAt(0));
          switch (p) {
            case 'A': cfg_spi_clock_hz = 1000000; break;
            case 'B': cfg_spi_clock_hz = 5000000; break;
            case 'C': cfg_spi_clock_hz = 10000000; break;
            case 'D': cfg_spi_clock_hz = 20000000; break;
            case 'E': cfg_spi_clock_hz = 40000000; break;
            case 'X':
              Serial.print("  Enter Hz: ");
              { String hz = read_serial_line();
                int v = hz.toInt();
                if (v > 0) cfg_spi_clock_hz = v;
                else Serial.println("  [!] Invalid frequency");
              }
              break;
            default:
              Serial.println("  [!] Invalid choice");
              break;
          }
        }
        break;

      case '5':
        cfg_read_mode = (cfg_read_mode == NAND_READ_QUAD)
                         ? NAND_READ_SINGLE : NAND_READ_QUAD;
        Serial.printf("  Read mode set to: %s\n",
                      cfg_read_mode == NAND_READ_QUAD ? "Quad x4" : "Single x1");
        break;

      case '6':
        cfg_verify = !cfg_verify;
        Serial.printf("  Verify reads: %s\n", cfg_verify ? "ON" : "OFF");
        break;

      case '7':
        Serial.print("  Enter max retries: ");
        { String val = read_serial_line();
          int v = val.toInt();
          if (v >= 0 && v <= 100) cfg_max_retries = v;
          else Serial.println("  [!] Invalid (0-100)");
        }
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
  delay(500);  // let serial settle

  Serial.println("\n========================================");
  Serial.println("  ESP32 SPI NAND Dumper v2.1");
  Serial.println("  Quad SPI + WiFi TCP");
  Serial.println("========================================");

  // ---- Interactive config ----
  config_menu();

  // ---- Initialize NAND ----
  nand_config_t nand_cfg = NAND_DEFAULT_CONFIG();
  nand_cfg.clock_hz  = cfg_spi_clock_hz;
  nand_cfg.read_mode = cfg_read_mode;

  esp_err_t ret = nand_init(&nand_cfg);
  if (ret != ESP_OK) {
    Serial.printf("[!] NAND init failed: %s\n", esp_err_to_name(ret));
    return;
  }

  // Print NAND info
  uint16_t id = nand_read_id();
  Serial.printf("[*] NAND ID: 0x%04X (Mfr: 0x%02X, Dev: 0x%02X)\n",
                id, id >> 8, id & 0xFF);
  Serial.printf("[*] Read mode: %s\n",
                nand_get_read_mode() == NAND_READ_QUAD ? "Quad x4" : "Single x1");
  Serial.printf("[*] SPI clock: %d Hz\n", cfg_spi_clock_hz);
  Serial.printf("[*] Geometry: %d blocks x %d pages x %d bytes = %.1f MB\n",
                NAND_TOTAL_BLOCKS, NAND_PAGES_PER_BLOCK, NAND_PAGE_SIZE,
                NAND_TOTAL_BYTES / (1024.0 * 1024.0));

  // ---- Initialize WiFi Transport ----
  wifi_transport_config_t wifi_cfg = {
    .ssid = cfg_ssid,
    .password = cfg_pass,
    .port = cfg_tcp_port,
  };

  if (!wifi_transport_init(&wifi_cfg)) {
    Serial.println("[!] WiFi init failed!");
    return;
  }

  wifi_transport_wait_client();
  wifi_transport_wait_trigger();

  // ---- Run dump ----
  cmd_dump(cfg_verify);
}

void loop() {}

// ---- Dump Command ----
void cmd_dump(bool verify) {
  Serial.println("[*] Starting full NAND dump...");

  // DMA-capable page buffer
  uint8_t *page_buf = (uint8_t*)heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
  if (!page_buf) {
    Serial.println("[!] Buffer alloc failed!");
    return;
  }

  unsigned long startTime = millis();
  uint32_t pagesDone = 0;
  uint32_t retryCount = 0;
  uint32_t failedPages = 0;

  for (uint16_t block = 0; block < NAND_TOTAL_BLOCKS; block++) {
    for (uint8_t page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
      uint16_t row = (block << 6) | (page & 0x3F);

      if (verify) {
        bool ok = nand_read_page_verified(row, page_buf, cfg_max_retries, &retryCount);
        if (!ok) failedPages++;
      } else {
        nand_page_read_to_cache(row);
        nand_wait_ready();
        nand_read_cache(page_buf, NAND_PAGE_SIZE);
      }

      wifi_transport_send(page_buf, NAND_PAGE_SIZE);
      pagesDone++;

      if (pagesDone % 1024 == 0) {
        float mb = (float)pagesDone * NAND_PAGE_SIZE / (1024.0 * 1024.0);
        float pct = (float)pagesDone / NAND_TOTAL_PAGES * 100.0;
        unsigned long el = (millis() - startTime) / 1000;
        float speed = el > 0 ? mb / el : 0;
        Serial.printf("[>] %u/%u (%.1f MB %.1f%%) %.2f MB/s retries:%u\n",
                      pagesDone, NAND_TOTAL_PAGES, mb, pct, speed, retryCount);
      }
    }
  }

  wifi_transport_close();
  free(page_buf);

  unsigned long elapsed = (millis() - startTime) / 1000;
  float totalMB = NAND_TOTAL_BYTES / (1024.0 * 1024.0);
  Serial.printf("\n[+] Dump complete!\n");
  Serial.printf("[+] %.1f MB in %lu sec (%.2f MB/s)\n",
                totalMB, elapsed, elapsed > 0 ? totalMB / elapsed : 0);
  Serial.printf("[+] Retries: %u | Failed pages: %u\n", retryCount, failedPages);
}