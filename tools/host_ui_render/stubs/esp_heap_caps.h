#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DEFAULT 4
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned caps) { (void)align; (void)caps; return malloc(size); }
static inline void heap_caps_free(void *p) { free(p); }
