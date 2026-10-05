#include "sfdp.h"
#include <string.h>
#include <stdio.h>

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

bool sfdp_probe(sfdp_read_fn rd, void *ctx, sfdp_info_t *out) {
  // Header: signature, minor, major, NPH (number of parameter headers - 1), access protocol.
  uint8_t hdr[8];
  rd(ctx, 0, hdr, sizeof(hdr));
  if (le32(hdr) != SFDP_SIGNATURE || hdr[5] != 1) return false;   // major 1 only
  unsigned nph = (unsigned)hdr[6] + 1;
  if (nph > 16) nph = 16;

  // Parameter headers: ID LSB, minor, major, length (dwords), 24-bit pointer, ID MSB.
  // The BFPT is ID 0xFF00; take the newest revision the chip lists.
  uint32_t ptr = 0;
  uint8_t len = 0, maj = 0, min = 0;
  bool found = false;
  for (unsigned i = 0; i < nph; i++) {
    uint8_t ph[8];
    rd(ctx, 8 + 8 * i, ph, sizeof(ph));
    if (ph[0] != 0x00 || ph[7] != 0xFF || ph[2] != 1 || ph[3] < 9) continue;
    if (found && ph[1] <= min) continue;
    ptr = (uint32_t)ph[4] | ((uint32_t)ph[5] << 8) | ((uint32_t)ph[6] << 16);
    len = ph[3]; maj = ph[2]; min = ph[1];
    found = true;
  }
  if (!found) return false;

  uint8_t bfpt[SFDP_BFPT_MAX_DW * 4];
  unsigned dw = len < SFDP_BFPT_MAX_DW ? len : SFDP_BFPT_MAX_DW;
  memset(bfpt, 0, sizeof(bfpt));
  rd(ctx, ptr, bfpt, (int)(dw * 4));
#define DW(n) le32(bfpt + 4 * ((n) - 1))   // 1-based, as the standard numbers them

  memset(out, 0, sizeof(*out));
  out->major = maj; out->minor = min; out->bfpt_dwords = len;
  uint32_t d1 = DW(1), d2 = DW(2);
  out->addr_mode = (d1 >> 17) & 0x3;
  out->has_114 = (d1 >> 22) & 1;
  // Density: bit 31 clear -> (value + 1) bits; set -> 2^value bits.
  uint64_t bits = (d2 & 0x80000000u) ? ((d2 & 0x7FFFFFFFu) < 64 ? 1ull << (d2 & 0x7FFFFFFFu) : 0)
                                     : (uint64_t)d2 + 1;
  uint64_t bytes = bits / 8;
  out->size_bytes = bytes > 0 && bytes <= 0xFFFFFFFFull ? (uint32_t)bytes : 0;
  if (out->has_114) {
    uint32_t d3 = DW(3);
    out->op_114 = (uint8_t)(d3 >> 24);
    out->dummy_114 = (uint8_t)(((d3 >> 16) & 0x1F) + ((d3 >> 21) & 0x7));
  }
  if (dw >= 15) out->qer = (uint8_t)((DW(15) >> 20) & 0x7);
  if (dw >= 16) out->enter4 = (uint8_t)(DW(16) >> 24);
#undef DW
  return out->size_bytes != 0;
}

bool sfdp_build_profile(const sfdp_info_t *info, const uint8_t id[3], active_profile_t *out) {
  uint32_t size = info->size_bytes;
  if (size < 4096 || (size & (size - 1))) return false;
  if (info->addr_mode == 2) return false;   // 4-byte-only parts: 03h takes 4 bytes; not modeled

  active_profile_t p;
  nand_profile_manual_nor(&p, size);         // family defaults, 03h/6Bh, 3-byte address
  snprintf(p.name, sizeof(p.name), "SFDP-%02X%02X%02X", id[0], id[1], id[2]);
  p.id_mfr = id[0]; p.id_dev = id[1]; p.id_dev2 = id[2];
  p.id_flags = NAND_PROFILE_ID_HAS_DEV2;
  p.qer = info->qer <= NOR_QER_MAX ? info->qer : 0;
  if (info->has_114 && info->op_114 && info->dummy_114 <= NOR_MAX_DUMMY) {
    p.op_read_cache_x4 = info->op_114;
    p.dummy_x4 = info->dummy_114;
  }
  if (size > (1u << 24)) {
    // Prefer B7h (no new opcodes), then WREN + B7h, then the dedicated 4-byte
    // instruction set (13h/6Ch). Bank/extended-address registers: not supported.
    p.addr_bytes = 4;
    if (info->enter4 & 0x01) {
      p.addr4_mode = NOR_ADDR4_ENTER;
    } else if (info->enter4 & 0x02) {
      p.addr4_mode = NOR_ADDR4_ENTER_WREN;
    } else if (info->enter4 & 0x20) {
      p.addr4_mode = NOR_ADDR4_NATIVE;
      p.op_read_cache = 0x13;
      p.op_read_cache_x4 = 0x6C;
    } else {
      return false;
    }
  } else {
    p.addr_bytes = 3;
    p.addr4_mode = NOR_ADDR4_NONE;
  }
  if (nand_profile_check(&p, 0xFFFFFFFFu) != NAND_PRF_OK) return false;
  *out = p;
  return true;
}
