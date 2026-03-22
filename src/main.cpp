#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>

// ============ CONFIGURATION ============
// WiFi credentials — UPDATE THESE
const char* WIFI_SSID = "HOPE_Insider";
const char* WIFI_PASS = "bnb9ebn8iFF";

// TCP server port
const uint16_t TCP_PORT = 3333;

// NAND geometry
#define CS_PIN 5
#define PAGE_SIZE 2112       // 2048 main + 64 spare
#define PAGES_PER_BLOCK 64
#define TOTAL_BLOCKS 1024

// SPI clock — lowered for signal integrity on breadboard wiring
#define SPI_CLOCK_HZ 5000000  // 1 MHz (safe for long/noisy wires)

// Read verification — read each page twice and compare
#define VERIFY_READS true
#define MAX_RETRIES  5
// =======================================

uint8_t pageBuffer[PAGE_SIZE];
uint8_t verifyBuffer[PAGE_SIZE];
WiFiServer server(TCP_PORT);

void sendCommand(uint8_t cmd);
void waitUntilReady();
void readPageToBuffer(uint16_t rowAddress, uint8_t* buf);

void setup() {
  // Serial is only used for debug messages now
  Serial.begin(115200);
  Serial.println("\n[*] ESP32 SPI NAND Dumper (WiFi Mode)");

  // SPI setup for NAND
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  Serial.printf("[*] SPI clock: %d Hz\n", SPI_CLOCK_HZ);

  // Connect to WiFi
  Serial.printf("[*] Connecting to WiFi '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[+] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[*] TCP server listening on port %d\n", TCP_PORT);

  // Start TCP server and wait for a client
  server.begin();
  server.setNoDelay(true);

  WiFiClient client;
  Serial.println("[*] Waiting for dump client to connect...");
  while (!(client = server.available())) {
    delay(10);
  }
  client.setNoDelay(true);
  Serial.println("[+] Client connected!");

  // Wait for 'G' trigger from the client
  while (true) {
    if (client.available() && client.read() == 'G') break;
    delay(1);
  }
  Serial.println("[*] Received GO trigger. Starting NAND dump...");

  // Reset the NAND to a known state
  sendCommand(0xFF);
  waitUntilReady();

  // The Extraction Loop
  unsigned long startTime = millis();
  uint32_t totalPages = (uint32_t)TOTAL_BLOCKS * PAGES_PER_BLOCK;
  uint32_t pagesDone = 0;
  uint32_t retryCount = 0;

  for (uint16_t block = 0; block < TOTAL_BLOCKS; block++) {
    for (uint8_t page = 0; page < PAGES_PER_BLOCK; page++) {

      // Calculate 16-bit Row Address: (Block << 6) | Page
      uint16_t rowAddress = (block << 6) | (page & 0x3F);

      // Read the page into pageBuffer
      readPageToBuffer(rowAddress, pageBuffer);

      if (VERIFY_READS) {
        // Verify: re-read and compare, retry on mismatch
        bool verified = false;
        for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
          readPageToBuffer(rowAddress, verifyBuffer);

          if (memcmp(pageBuffer, verifyBuffer, PAGE_SIZE) == 0) {
            verified = true;
            break;
          }

          // Mismatch — use the verify read as new reference and try again
          retryCount++;
          Serial.printf("[!] Mismatch at block %u page %u (retry %d)\n", block, page, attempt + 1);
          memcpy(pageBuffer, verifyBuffer, PAGE_SIZE);
        }

        if (!verified) {
          Serial.printf("[!!] FAILED verification at block %u page %u after %d retries!\n",
                        block, page, MAX_RETRIES);
          // Send the last read anyway — best effort
        }
      }

      // Send page over TCP (flow control handled by TCP)
      client.write(pageBuffer, PAGE_SIZE);

      pagesDone++;
      // Print progress every 1024 pages (~2 MB)
      if (pagesDone % 1024 == 0) {
        float mb = (float)pagesDone * PAGE_SIZE / (1024.0 * 1024.0);
        float pct = (float)pagesDone / totalPages * 100.0;
        Serial.printf("[>] %u/%u pages (%.1f MB, %.1f%%) retries: %u\n",
                      pagesDone, totalPages, mb, pct, retryCount);
      }
    }
  }

  // Flush and close
  client.flush();
  delay(100);
  client.stop();

  unsigned long elapsed = (millis() - startTime) / 1000;
  float totalMB = (float)totalPages * PAGE_SIZE / (1024.0 * 1024.0);
  Serial.printf("[+] Dump complete! %.1f MB in %lu seconds\n", totalMB, elapsed);
  Serial.printf("[+] Total retries needed: %u\n", retryCount);
}

void loop() {
  // Nothing after dump
}

// Read a single page from NAND cache into the given buffer
void readPageToBuffer(uint16_t rowAddress, uint8_t* buf) {
  // Stage data from Array to Cache (PAGE READ 13h)
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x13);
  SPI.transfer(0x00);
  SPI.transfer((rowAddress >> 8) & 0xFF);
  SPI.transfer(rowAddress & 0xFF);
  digitalWrite(CS_PIN, HIGH);
  waitUntilReady();

  // Read from Cache (0Bh fast read)
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x0B);
  SPI.transfer(0x00);  // Dummy + Upper Column Address
  SPI.transfer(0x00);  // Lower Column Address
  SPI.transfer(0x00);  // Dummy byte for fast read
  for (int i = 0; i < PAGE_SIZE; i++) {
    buf[i] = SPI.transfer(0x00);
  }
  digitalWrite(CS_PIN, HIGH);
}

void sendCommand(uint8_t cmd) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(cmd);
  digitalWrite(CS_PIN, HIGH);
}

void waitUntilReady() {
  uint8_t status = 0x01;
  while (status & 0x01) {
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(0x0F);
    SPI.transfer(0xC0);
    status = SPI.transfer(0x00);
    digitalWrite(CS_PIN, HIGH);
  }
}