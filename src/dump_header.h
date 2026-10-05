#ifndef DUMP_HEADER_H
#define DUMP_HEADER_H
#include <stdint.h>

#define DUMP_HEADER_SIZE   32
#define DUMP_PROTO_VERSION 2
#define DUMP_FLAG_ECC_ON   0x01
#define DUMP_FLAG_QUAD     0x02
#define DUMP_FLAG_VERIFY   0x04
#define DUMP_FLAG_PAGECRC  0x08   // v2: each page is followed by a 4-byte CRC32 seal
#define DUMP_FLAG_NOR      0x10   // SPI NOR: pages are 4 KiB read units, spare_size 0,
                                  // mfr/dev are the plain 9Fh ID's first two bytes
#define DUMP_FLAG_EEPROM   0x20   // serial EEPROM: pages are 256 B read units (or the
                                  // whole part), spare_size 0, mfr/dev are 0 ...
#define DUMP_FLAG_I2C      0x40   // ... except on I2C (24xx), where mfr_id is the
                                  // device address the part answered at

typedef struct {
  uint16_t page_size, spare_size, pages_per_block, total_blocks;
  uint32_t total_pages, total_bytes;
  uint8_t  mfr_id, dev_id, page_addr_bits, flags;
} dump_geometry_t;

uint32_t dump_crc32(const uint8_t *data, unsigned len);
void dump_header_pack(uint8_t buf[DUMP_HEADER_SIZE], const dump_geometry_t *g);

#endif // DUMP_HEADER_H
