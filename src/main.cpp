#include <Arduino.h>
#include "nand_driver.h"
#include "wifi_transport.h"

// ============ CONFIGURATION ============
const char* WIFI_SSID = "HOPE_Insider";
const char* WIFI_PASS = "bnb9ebn8iFF";
const uint16_t TCP_PORT = 3333;

#define SPI_CLOCK_HZ    10000000       // 10 MHz
#define READ_MODE       NAND_READ_SINGLE
#define VERIFY_READS    true
#define MAX_RETRIES     5
// =======================================

void cmd_dump(bool verify);

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  ESP32 SPI NAND Dumper v2.0");
  Serial.println("  Quad SPI + WiFi TCP");
  Serial.println("========================================");

  // ---- Initialize NAND ----
  nand_config_t nand_cfg = NAND_DEFAULT_CONFIG();
  nand_cfg.clock_hz = SPI_CLOCK_HZ;
  nand_cfg.read_mode = READ_MODE;

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
  Serial.printf("[*] SPI clock: %d Hz\n", SPI_CLOCK_HZ);
  Serial.printf("[*] Geometry: %d blocks x %d pages x %d bytes = %.1f MB\n",
                NAND_TOTAL_BLOCKS, NAND_PAGES_PER_BLOCK, NAND_PAGE_SIZE,
                NAND_TOTAL_BYTES / (1024.0 * 1024.0));

  // ---- Initialize WiFi Transport ----
  wifi_transport_config_t wifi_cfg = {
    .ssid = WIFI_SSID,
    .password = WIFI_PASS,
    .port = TCP_PORT,
  };

  if (!wifi_transport_init(&wifi_cfg)) {
    Serial.println("[!] WiFi init failed!");
    return;
  }

  wifi_transport_wait_client();
  wifi_transport_wait_trigger();

  // ---- Run dump ----
  cmd_dump(VERIFY_READS);
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
        bool ok = nand_read_page_verified(row, page_buf, MAX_RETRIES, &retryCount);
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