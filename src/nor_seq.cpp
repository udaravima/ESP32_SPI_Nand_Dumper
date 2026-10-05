#include "nor_seq.h"
#include <string.h>

void nor_seq_init(nor_seq_t *s, nor_bus_t bus) {
  memset(s, 0, sizeof(*s));
  s->bus = bus;
  s->op_read = NOR_OP_READ;
  s->op_read_x4 = NOR_OP_READ_X4;
  s->op_rdsr = NOR_OP_RDSR;
  s->addr_bytes = 3;
  s->dummy_x4 = 8;
}

void nor_seq_apply(nor_seq_t *s, const active_profile_t *p) {
  s->op_read = p->op_read_cache;
  s->op_read_x4 = p->op_read_cache_x4;
  s->op_rdsr = p->op_get_feat;
  s->addr_bytes = p->addr_bytes;
  s->addr4_mode = p->addr4_mode;
  s->dummy_x1 = p->dummy_x1;
  s->dummy_x4 = p->dummy_x4;
  s->qer = p->qer;
}

void nor_seq_set_quad(nor_seq_t *s, bool quad) {
  s->quad_on = quad;
  s->quad_fallbacks = 0;
}

static void cmd(nor_seq_t *s, uint8_t op) {
  s->bus.xfer(s->bus.ctx, op, 0, 0, 0, NULL, 0, false);
}

void nor_seq_release_power_down(nor_seq_t *s) { cmd(s, NOR_OP_RELEASE_PD); }

void nor_seq_read_id(nor_seq_t *s, uint8_t id[3]) {
  s->bus.xfer(s->bus.ctx, NOR_OP_READ_ID, 0, 0, 0, id, 3, false);
}

void nor_seq_read_sfdp(nor_seq_t *s, uint32_t addr, uint8_t *buf, int len) {
  s->bus.xfer(s->bus.ctx, NOR_OP_READ_SFDP, addr, 3, 8, buf, len, false);
}

uint8_t nor_seq_read_status(nor_seq_t *s, uint8_t op) {
  uint8_t v = 0;
  s->bus.xfer(s->bus.ctx, op, 0, 0, 0, &v, 1, false);
  return v;
}

int nor_seq_qe_state(nor_seq_t *s) {
  switch (s->qer) {
    case 2: return (nor_seq_read_status(s, s->op_rdsr) >> 6) & 1;        // SR1 bit 6
    case 3: return (nor_seq_read_status(s, NOR_OP_RDSR2_ALT) >> 7) & 1;  // SR2 bit 7 via 3Fh
    case 5: case 6: return (nor_seq_read_status(s, NOR_OP_RDSR2) >> 1) & 1;  // SR2 bit 1
    default: return -1;   // 0: no QE bit known; 1, 4: SR2 has no defined read command
  }
}

void nor_seq_begin(nor_seq_t *s) {
  if (s->addr4_mode == NOR_ADDR4_ENTER_WREN) cmd(s, NOR_OP_WREN);
  if (s->addr4_mode == NOR_ADDR4_ENTER || s->addr4_mode == NOR_ADDR4_ENTER_WREN) {
    cmd(s, NOR_OP_EN4B);
    s->in_4byte = true;
  }
}

void nor_seq_end(nor_seq_t *s) {
  if (!s->in_4byte) return;
  if (s->addr4_mode == NOR_ADDR4_ENTER_WREN) cmd(s, NOR_OP_WREN);
  cmd(s, NOR_OP_EX4B);
  s->in_4byte = false;
}

void nor_seq_read(nor_seq_t *s, uint32_t addr, uint8_t *buf, int len, bool quad) {
  s->bus.xfer(s->bus.ctx, quad ? s->op_read_x4 : s->op_read, addr, s->addr_bytes,
              quad ? s->dummy_x4 : s->dummy_x1, buf, len, quad);
}

// Read until two consecutive reads agree (at most max_retries extra reads).
static bool settle(nor_seq_t *s, uint32_t addr, uint8_t *buf, uint8_t *scratch, int len,
                   int max_retries, bool quad, uint32_t *retry_count) {
  nor_seq_read(s, addr, buf, len, quad);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    nor_seq_read(s, addr, scratch, len, quad);
    if (memcmp(buf, scratch, len) == 0) return true;
    if (retry_count) (*retry_count)++;
    memcpy(buf, scratch, len);
  }
  return false;
}

nand_page_result_t nor_seq_read_verified(nor_seq_t *s, uint32_t addr, uint8_t *buf,
                                         uint8_t *scratch, int len, int max_retries,
                                         uint32_t *retry_count) {
  if (settle(s, addr, buf, scratch, len, max_retries, s->quad_on, retry_count)) return NAND_PAGE_OK;
  if (!s->quad_on) return NAND_PAGE_UNSTABLE;
  if (!settle(s, addr, buf, scratch, len, max_retries, false, retry_count))
    return NAND_PAGE_UNSTABLE;
  if (++s->quad_fallbacks >= NAND_QUAD_FALLBACK_LIMIT) s->quad_on = false;
  return NAND_PAGE_OK_SINGLE;
}
