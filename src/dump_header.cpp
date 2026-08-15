#include "dump_header.h"
#include <string.h>

// Bitwise CRC32 (reflected, poly 0xEDB88320) — matches Python zlib.crc32.
uint32_t dump_crc32(const uint8_t *data, unsigned len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

void dump_header_pack(uint8_t buf[DUMP_HEADER_SIZE], const dump_geometry_t *g) {
  memset(buf, 0, DUMP_HEADER_SIZE);
  memcpy(buf, "NANDMP", 6);
  buf[6] = DUMP_PROTO_VERSION;
  buf[7] = g->flags;
  put16(buf + 8,  g->page_size);
  put16(buf + 10, g->spare_size);
  put16(buf + 12, g->pages_per_block);
  put16(buf + 14, g->total_blocks);
  put32(buf + 16, g->total_pages);
  buf[20] = g->mfr_id;
  buf[21] = g->dev_id;
  buf[22] = g->page_addr_bits;
  buf[23] = 0; // reserved
  put32(buf + 24, g->total_bytes);
  put32(buf + 28, dump_crc32(buf, 28));
}
