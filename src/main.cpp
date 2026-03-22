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
// =======================================

uint8_t pageBuffer[PAGE_SIZE];
WiFiServer server(TCP_PORT);

void sendCommand(uint8_t cmd);
void waitUntilReady();

void setup() {
  // Serial is only used for debug messages now
  Serial.begin(115200);
  Serial.println("\n[*] ESP32 SPI NAND Dumper (WiFi Mode)");

  // SPI setup for NAND: 10MHz, MSB First, SPI Mode 0
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

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

  for (uint16_t block = 0; block < TOTAL_BLOCKS; block++) {
    for (uint8_t page = 0; page < PAGES_PER_BLOCK; page++) {

      // Calculate 16-bit Row Address: (Block << 6) | Page
      uint16_t rowAddress = (block << 6) | (page & 0x3F);

      // STEP A: Stage data from Array to Cache (13h)
      digitalWrite(CS_PIN, LOW);
      SPI.transfer(0x13);
      SPI.transfer(0x00);
      SPI.transfer((rowAddress >> 8) & 0xFF);
      SPI.transfer(rowAddress & 0xFF);
      digitalWrite(CS_PIN, HIGH);
      waitUntilReady();

      // STEP B: Read from Cache (0Bh fast read)
      digitalWrite(CS_PIN, LOW);
      SPI.transfer(0x0B);
      SPI.transfer(0x00); // Dummy + Upper Column Address
      SPI.transfer(0x00); // Lower Column Address
      SPI.transfer(0x00); // Dummy byte for fast read
      for (int i = 0; i < PAGE_SIZE; i++) {
        pageBuffer[i] = SPI.transfer(0x00);
      }
      digitalWrite(CS_PIN, HIGH);

      // STEP C: Send page over TCP (flow control handled by TCP)
      client.write(pageBuffer, PAGE_SIZE);

      pagesDone++;
      // Print progress every 1024 pages (~2 MB)
      if (pagesDone % 1024 == 0) {
        float mb = (float)pagesDone * PAGE_SIZE / (1024.0 * 1024.0);
        float pct = (float)pagesDone / totalPages * 100.0;
        Serial.printf("[>] %u/%u pages (%.1f MB, %.1f%%)\n", pagesDone, totalPages, mb, pct);
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
}

void loop() {
  // Nothing after dump
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