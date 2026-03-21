#include <Arduino.h>
#include <SPI.h>

#define CS_PIN 5
#define PAGE_SIZE 2112 // 2048 main + 64 spare [cite: 39]
#define PAGES_PER_BLOCK 64 // [cite: 94]
#define TOTAL_BLOCKS 1024 // [cite: 94]

uint8_t pageBuffer[PAGE_SIZE];

void sendCommand(uint8_t cmd);
void waitUntilReady(); 

void setup() {
  // Max out the baud rate to prevent serial bottlenecking
  Serial.begin(2000000); 
  
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  
  // Standard SPI setup: 10MHz, MSB First, SPI Mode 0 [cite: 204]
  SPI.begin();
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

  // Automation logic: Wait for the Python script to say 'GO'
  while(Serial.read() != 'G') {
    delay(10);
  }

  // 1. Reset the NAND to a known state [cite: 196]
  sendCommand(0xFF); 
  waitUntilReady();

  // 2. The Extraction Loop
  for (uint16_t block = 0; block < TOTAL_BLOCKS; block++) {
    for (uint8_t page = 0; page < PAGES_PER_BLOCK; page++) {
      
      // Calculate 16-bit Row Address: (Block << 6) | Page [cite: 176-184]
      uint16_t rowAddress = (block << 6) | (page & 0x3F);

      // STEP A: Stage data from Array to Cache (13h) [cite: 360, 365]
      digitalWrite(CS_PIN, LOW);
      SPI.transfer(0x13); // PAGE READ command
      SPI.transfer(0x00); // 8 dummy bits
      SPI.transfer((rowAddress >> 8) & 0xFF); // Upper 8 address bits
      SPI.transfer(rowAddress & 0xFF);        // Lower 8 address bits
      digitalWrite(CS_PIN, HIGH);

      // Wait for data to hit the cache (tR time) [cite: 366]
      waitUntilReady();

      // STEP B: Read from Cache to ESP32 (03h) [cite: 368]
      digitalWrite(CS_PIN, LOW);
      SPI.transfer(0x03); // READ FROM CACHE command
      SPI.transfer(0x00); // Dummy bits + Upper Col Address (000h) [cite: 462-466]
      SPI.transfer(0x00); // Lower Col Address
      SPI.transfer(0x00); // 1 Dummy byte required for 03h [cite: 465]

      // Clock out all 2112 bytes directly into our buffer
      for (int i = 0; i < PAGE_SIZE; i++) {
        pageBuffer[i] = SPI.transfer(0x00);
      }
      digitalWrite(CS_PIN, HIGH);

      // STEP C: Blast the buffer over Serial to the PC
      Serial.write(pageBuffer, PAGE_SIZE);
    }
  }
}

void loop() {
  // Do nothing after dump finishes
}

void sendCommand(uint8_t cmd) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(cmd);
  digitalWrite(CS_PIN, HIGH);
}

void waitUntilReady() {
  uint8_t status = 0x01;
  // Poll Status Register (C0h) [cite: 351] via GET FEATURES (0Fh) [cite: 367]
  while (status & 0x01) { // Check OIP (Operation In Progress) bit [cite: 1210]
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(0x0F); 
    SPI.transfer(0xC0);
    status = SPI.transfer(0x00);
    digitalWrite(CS_PIN, HIGH);
  }
}