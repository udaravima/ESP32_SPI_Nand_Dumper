// Pure capability math (no Arduino deps) so it compiles in the host test env.
// The ESP-only queries/reporting live in sys_info_esp.cpp.
#include "sys_info.h"

int sys_recommend_batch_pages(size_t free_dma_bytes, size_t frame_size, int max_cap) {
  if (frame_size == 0) return 1;
  size_t usable = (free_dma_bytes > SYS_DMA_HEADROOM)
                    ? free_dma_bytes - SYS_DMA_HEADROOM
                    : frame_size;                 // too tight: allow exactly one page
  size_t pages = usable / frame_size;
  if (pages < 1) pages = 1;
  if (max_cap > 0 && pages > (size_t)max_cap) pages = (size_t)max_cap;
  return (int)pages;
}
