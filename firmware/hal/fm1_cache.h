/* SPDX-License-Identifier: GPL-3.0-only */
/* The AC79's caches (SMK-37 probe, smk37-devtools harmoniser-spec.md §13v-1). From the AC79 SDK's cache_way_config and
 * flush_all_dcache (cpu/wl82/liba/cpu.a, cache.c.o, read as LLVM IR):
 *   CACHE_CON 0x1EEE008: b14 idle, b8 / b9 the caches on (b9 by the flash mode)
 *   0x1EEE00C: the data cache's ways, three 8-bit masks (bit 7-i, 15-i, 23-i: way i kept)
 *   0x1EEE010: the instruction cache's ways, two 8-bit masks (bit i, i+8)
 *   data RAM 0x1F20000 (instruction ways 0-7) and 0x1F28000 (data ways 0-7), 4 KB a way; the data cache's tags at
 *   0x1F00000 (two words a line, bit 18 dirty: written back with `flush [addr]`)
 * The SDK's no-SDRAM build keeps data way 7 and uses ways 0-6 as RAM (28 KB at 0x1F28000); on this firmware the data
 * cache is in use (all ways, our RAM's dirty lines), so that is not taken. */
#pragma once
#include <stdint.h>

#define FM1_CC_CON (*(volatile uint32_t *)0x1EEE008u)
#define FM1_CC_DWAY (*(volatile uint32_t *)0x1EEE00Cu)
#define FM1_CC_IWAY (*(volatile uint32_t *)0x1EEE010u)

static inline void fm1_cache_regs(uint32_t *con, uint32_t *dways, uint32_t *iways)
{
    *con = FM1_CC_CON, *dways = FM1_CC_DWAY, *iways = FM1_CC_IWAY;
}

/* (Build 1.7: freeing the data cache's ways at run time removed: the data cache holds our RAM's dirty lines and the
 * SDK's runtime flush faulted the CPU twice, harmoniser-spec.md §13w; the read-only registers stay) */
