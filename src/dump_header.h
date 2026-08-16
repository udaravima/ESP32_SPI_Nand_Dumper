#ifndef DUMP_HEADER_H
#define DUMP_HEADER_H
#include <stdint.h>

#define DUMP_HEADER_SIZE   32
#define DUMP_PROTO_VERSION 2
#define DUMP_FLAG_ECC_ON   0x01
#define DUMP_FLAG_QUAD     0x02
#define DUMP_FLAG_VERIFY   0x04
#define DUMP_FLAG_PAGECRC  0x08   // v2: each page is followed by a 4-byte CRC32 seal

typedef struct {
  uint16_t page_size, spare_size, pages_per_block, total_blocks;
  uint32_t total_pages, total_bytes;
  uint8_t  mfr_id, dev_id, page_addr_bits, flags;
} dump_geometry_t;

uint32_t dump_crc32(const uint8_t *data, unsigned len);
void dump_header_pack(uint8_t buf[DUMP_HEADER_SIZE], const dump_geometry_t *g);

#endif // DUMP_HEADER_H
