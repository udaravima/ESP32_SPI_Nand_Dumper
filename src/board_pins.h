#ifndef BOARD_PINS_H
#define BOARD_PINS_H
#include "driver/spi_master.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3 FSPI (SPI2) IOMUX pins. GPIO9-14 are plain I/O — not flash/PSRAM
  // (those are GPIO26-37), not strapping (0/3/45/46), not USB (19/20) or
  // UART0 (43/44). Per ESP32-S3 datasheet Table 2-1.
  #define NAND_PIN_CLK  12   // FSPICLK
  #define NAND_PIN_D0   11   // FSPID  (MOSI / SIO0)
  #define NAND_PIN_D1   13   // FSPIQ  (MISO / SIO1)
  #define NAND_PIN_D2   14   // FSPIWP (SIO2, quad only)
  #define NAND_PIN_D3    9   // FSPIHD (SIO3, quad only)
  #define NAND_PIN_CS   10   // FSPICS0
  #define NAND_SPI_HOST SPI2_HOST
#else
  // ESP32-classic VSPI (SPI3) IOMUX pins.
  #define NAND_PIN_CLK  18
  #define NAND_PIN_D0   23
  #define NAND_PIN_D1   19
  #define NAND_PIN_D2   22
  #define NAND_PIN_D3   21
  #define NAND_PIN_CS    5
  #define NAND_SPI_HOST SPI3_HOST
#endif

#endif // BOARD_PINS_H
