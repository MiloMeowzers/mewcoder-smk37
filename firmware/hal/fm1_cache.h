/* SPDX-License-Identifier: GPL-3.0-only */
/* The AC79's caches (SMK-37 probe, smk37-devtools harmoniser-spec.md §13v-1). From the AC79 SDK's cache_way_config and
 * flush_all_dcache (cpu/wl82/liba/cpu.a, cache.c.o, read as LLVM IR):
 *   CACHE_CON 0x1EEE008: b14 idle, b8 / b9 the caches on (b9 by the flash mode)
 *   0x1EEE00C: the data cache's ways, three 8-bit masks (bit 7-i, 15-i, 23-i: way i kept)
 *   0x1EEE010: the instruction cache's ways, two 8-bit masks (bit i, i+8)
 *   data RAM 0x1F20000 (instruction ways 0-7) and 0x1F28000 (data ways 0-7), 4 KB a way; the data cache's tags at
 *   0x1F00000 (two words a line, bit 18 dirty: written back with `flush [addr]`)
 * The SDK's no-SDRAM build keeps data way 7 and uses ways 0-6 as RAM (28 KB at 0x1F28000). Needs fm1_flash.h (RAMFN,
 * FL_FAR). Undone by a power cycle. */
#pragma once
#include <stdint.h>

#define FM1_CC_CON (*(volatile uint32_t *)0x1EEE008u)
#define FM1_CC_DWAY (*(volatile uint32_t *)0x1EEE00Cu)
#define FM1_CC_IWAY (*(volatile uint32_t *)0x1EEE010u)
#define FM1_CC_DTAG ((volatile uint32_t *)0x01F00000u)
#define FM1_CC_RAM ((volatile uint32_t *)0x01F28000u)
#define FM1_CC_WORDS (28u * 1024u / 4u)

static inline void fm1_cache_regs(uint32_t *con, uint32_t *dways, uint32_t *iways)
{
    *con = FM1_CC_CON, *dways = FM1_CC_DWAY, *iways = FM1_CC_IWAY;
}
static inline int fm1_cache_dways_busy(void) { return (FM1_CC_DWAY & 0x7Fu) != 0u; }   /* data ways 0-6 caching */

/* in RAM, interrupts off, core 1 idle in its RAM loop, no calls: every data way's dirty line written back, the caches
 * off, the data cache down to way 7, the tags cleared (the caches start empty), the caches on again as they were */
static void RAMFN fm1_cache_free_ram(void)
{
    volatile uint32_t *p;
    uint32_t i, on;
    __asm__ volatile("csync\n\tssync" ::: "memory");
    for (i = 1u; i < 2048u; i += 2u) {
        uint32_t tg = FM1_CC_DTAG[i];
        if (tg & 0x40000u) {
            uint32_t a = ((tg << 12) & 0x7FFF000u) | ((i << 4) & 0xFE0u);
            __asm__ volatile("csync\n\tflush [%0]\n\tcsync" :: "r"(a) : "memory");
        }
    }
    __asm__ volatile("csync\n\tssync" ::: "memory");
    while (!(FM1_CC_CON & 0x4000u))
        ;
    on = FM1_CC_CON & 0x300u;
    FM1_CC_CON &= ~0x300u;
    FM1_CC_DWAY = (FM1_CC_DWAY & 0xFF000000u) | (1u << 7) | (1u << 15) | (1u << 23);
    for (p = (volatile uint32_t *)0x01F00000u; p < (volatile uint32_t *)0x01F04000u; p++)
        *p = 0;
    for (p = (volatile uint32_t *)0x01F0C000u; p < (volatile uint32_t *)0x01F0E200u; p++)
        *p = 0;
    for (p = (volatile uint32_t *)0x01F0F000u; p < (volatile uint32_t *)0x01F0F200u; p++)
        *p = 0;
    FM1_CC_CON |= on;
    __asm__ volatile("csync" ::: "memory");
}
/* from the main loop (no audio interrupt running: core 1 idle) */
static inline void fm1_cache_free_dways(void)
{
    __asm__ volatile("cli" ::: "memory");
    FL_FAR(fm1_cache_free_ram)();
    __asm__ volatile("csync\n\tsti" ::: "memory");
}
/* the 28 KB: a pattern written (write) and read back; the words that differ */
static uint32_t fm1_cache_ram_test(int write)
{
    uint32_t i, bad = 0;
    if (write)
        for (i = 0; i < FM1_CC_WORDS; i++)
            FM1_CC_RAM[i] = (i * 2654435761u) ^ 0xA5C3F00Du;
    for (i = 0; i < FM1_CC_WORDS; i++)
        bad += FM1_CC_RAM[i] != ((i * 2654435761u) ^ 0xA5C3F00Du);
    return bad;
}
