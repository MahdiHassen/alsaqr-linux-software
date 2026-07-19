#include "encoding.h"
#include <stdio.h>
#include <stdint.h>
#include "utils.h"
#include "march.c"

/* =========================================================================
 * March Retention Test  —  DRAM data-retention / refresh check
 *
 * The one thing March C- cannot do: March reads data back within seconds of
 * writing it, so it never tests whether the DRAM controller's REFRESH keeps
 * cells alive over time.  This test does:
 *
 *   Phase 1  (STRESS_SECONDS)  : March C- stress over the tracked region.
 *   FINAL WRITE                : fill the whole tracked region with RET_PATTERN.
 *   FLUSH                      : evict caches so the pattern is really in DRAM.
 *   Phase 2  (IDLE_SECONDS)    : idle — no core traffic at all; only the DRAM
 *                                controller's autonomous refresh runs.
 *   Phase 3  (VERIFY)          : read the region back FROM DRAM and count any
 *                                bit flips vs RET_PATTERN.  <-- the measurement
 *   Phase 3b (POST_MARCH_SECONDS, optional) : keep marching afterwards.
 *
 * WHY THE FLUSH MATTERS (same trap that clobbered the DTB): the LLC is
 * write-back.  If the pattern is still dirty in cache when we idle, (a) it is
 * not actually in the DRAM cells, and (b) the verify would hit the cache
 * (SRAM — never refreshed, never leaks) and always "pass".  So before idling
 * we thrash the cache with a big disjoint read sweep: that writes every dirty
 * pattern line back to DRAM and clears them from cache, so the verify reads
 * come straight from the DRAM cells.
 *
 * RESULT: 0 flips  -> refresh keeps cells alive -> OpenDRAM refresh path OK.
 *         N flips  -> refresh broken/marginal; the XOR shows which bits.
 *                     1->0 flips = classic charge leak.
 *
 * Single core: hart 0 does everything; harts 1-3 park in wfi so nothing
 * touches DRAM during the idle window.
 * ========================================================================= */

/* ------------------------------------------------------------------ TUNING */
/* SMOKE = 1 : fast validation — small region + short phases (~1 min total).
 *             Run this FIRST to confirm PASS + serial + result dump.
 * SMOKE = 0 : the real run — full ~378 MB region, 1 hour each phase.
 * A single March pass over the full region takes ~8 min, so the region must
 * be small for a quick smoke test. */
#define SMOKE  0

#if SMOKE
  #define STRESS_SECONDS   20UL
  #define IDLE_SECONDS     20UL
  #define TRK_A_END_ADDR   0x80800000UL   /* seg A =  4 MB */
  #define TRK_B_END_ADDR   0x85900000UL   /* seg B =  8 MB */
#else
  #define STRESS_SECONDS   3600UL         /* 1 hour */
  #define IDLE_SECONDS     3600UL         /* 1 hour */
  #define TRK_A_END_ADDR   0x85000000UL   /* seg A = 76 MB  */
  #define TRK_B_END_ADDR   0x98000000UL   /* seg B = 302 MB */
#endif

#define POST_MARCH_SECONDS  3600UL   /* phase 3b: third hour of march after readback */
#define FREQ_HZ             20000000UL   /* opendram2 SoC clock = 20 MHz */

/* Retention pattern written before the idle window.  Rerun with variants:
 *   0xFFFF... all-1s (classic charge retention)   0x0000... all-0s
 *   0xAAAA... / 0x5555... checkerboard (retention + coupling) */
// #define RET_PATTERN         0xFFFFFFFFFFFFFFFFULL
#define RET_PATTERN         0x0000000000000000ULL

/* --------------------------------------------------------------- MEMORY MAP
 *  0x80000000-0x80400000  4 MB    code + stacks (untouched)
 *  0x80400000-0x85000000  76 MB   tracked segment A
 *  0x85000000-0x85100000  1 MB    result struct (load.sh dumps here)
 *  0x85100000-0x98000000  302 MB  tracked segment B
 *  0x98000000-0x99000000  16 MB   scratch (cache-flush sweep; >> ~1.1 MiB cache)
 *  0x99000000-0xA0000000  112 MB  unused
 * ------------------------------------------------------------------------- */
#define TRK_A_BASE   ((volatile uint64_t *)0x80400000UL)
#define TRK_A_END    TRK_A_END_ADDR
#define TRK_A_WORDS  ((uint32_t)((TRK_A_END - 0x80400000UL) / 8))

#define RESULT_ADDR  0x85000000UL

#define TRK_B_BASE   ((volatile uint64_t *)0x85100000UL)
#define TRK_B_END    TRK_B_END_ADDR
#define TRK_B_WORDS  ((uint32_t)((TRK_B_END - 0x85100000UL) / 8))

#define TRACKED_MB   ((uint32_t)(((TRK_A_END - 0x80400000UL) + (TRK_B_END - 0x85100000UL)) >> 20))

#define SCRATCH_BASE  ((volatile uint64_t *)0x98000000UL)
#define SCRATCH_WORDS ((uint32_t)((16UL * 1024 * 1024) / 8))   /* 16 MB */

#define MAGIC_RUNNING 0x600DC0DEUL
#define MAGIC_DONE    0xDEADDEADUL

#define STRESS_CYCLES  ((uint64_t)STRESS_SECONDS     * FREQ_HZ)
#define IDLE_CYCLES    ((uint64_t)IDLE_SECONDS       * FREQ_HZ)
#define POST_CYCLES    ((uint64_t)POST_MARCH_SECONDS * FREQ_HZ)

typedef struct {
    uint32_t magic;
    uint32_t phase;          /* 1 stress, 2 idle, 3 verify, 4 done */
    uint32_t stress_passes;
    uint32_t stress_errors;  /* March errors seen while stressing (should be 0) */
    uint32_t pattern_hi, pattern_lo;
    uint32_t tracked_mb;
    uint32_t idle_seconds;
    uint32_t total_flips;    /* <-- THE RESULT (retention bit flips)            */
    uint32_t idle_cyc_hi, idle_cyc_lo;  /* measured idle duration in cycles     */
    march_error_t flips[MARCH_MAX_ERRORS];
} ret_result_t;

static volatile ret_result_t *const R = (volatile ret_result_t *)RESULT_ADDR;

/* ------------------------------------------------------------------ HELPERS */
static inline uint64_t rdcycle64(void) {
    uint64_t c;
    asm volatile ("rdcycle %0" : "=r" (c));
    return c;
}

/* Cache flush by thrash: reading SCRATCH (16 MB >> ~1.1 MiB total cache) evicts
 * every dirty tracked line -> write-back to DRAM -> and clears them from cache.
 * No cache-management instruction needed. */
static void cache_flush(void) {
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < SCRATCH_WORDS; i++)
        sink += SCRATCH_BASE[i];
    asm volatile ("fence" ::: "memory");
    (void)sink;
}

/* Busy-wait `cycles` core cycles WITHOUT touching DRAM: the loop body is empty
 * (lives in L1 I-cache) and rdcycle is a CSR read.  Only DRAM activity during
 * this window is the controller's autonomous refresh. */
static void idle_spin(uint64_t cycles) {
    uint64_t start = rdcycle64();
    while ((rdcycle64() - start) < cycles) { /* no memory access */ }
}

static void fill_pattern(volatile uint64_t *base, uint32_t nwords, uint64_t pat) {
    for (uint32_t i = 0; i < nwords; i++)
        base[i] = pat;
    asm volatile ("fence" ::: "memory");
}

/* Read a region back and count/log mismatches vs pat.  `already` is the flip
 * count from prior segments, so error slots continue across segments. */
static uint32_t verify_pattern(volatile uint64_t *base, uint32_t nwords, uint64_t pat,
                               march_error_t *errs, uint32_t max, uint32_t already) {
    uint32_t flips = 0;
    for (uint32_t i = 0; i < nwords; i++) {
        uint64_t got = base[i];
        if (got != pat) {
            uint32_t slot = already + flips;
            if (slot < max) {
                uint64_t a = (uint64_t)(uintptr_t)&base[i];
                errs[slot].addr_lo     = (uint32_t)a;   errs[slot].addr_hi     = (uint32_t)(a   >> 32);
                errs[slot].expected_lo = (uint32_t)pat; errs[slot].expected_hi = (uint32_t)(pat >> 32);
                errs[slot].actual_lo   = (uint32_t)got; errs[slot].actual_hi   = (uint32_t)(got >> 32);
            }
            flips++;
        }
    }
    return flips;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char const *argv[]) {
    uint32_t mhartid;
    asm volatile ("csrr %0, 0xF14" : "=r" (mhartid));
    if (mhartid != 0) { while (1) asm volatile ("wfi"); }   /* single-core */

    /* No UART/FLL setup: the UART is hardwired to 38400 in the bitstream, so
     * printf just works. (Matches Mahdi's mser/sift/stitch mains. The old
     * FPGA_EMULATION uart_set_cfg block actually mis-programmed the divisor
     * and killed serial output.) */

    R->magic         = MAGIC_RUNNING;
    R->phase         = 1;
    R->stress_passes = 0;
    R->stress_errors = 0;
    R->pattern_hi    = (uint32_t)(RET_PATTERN >> 32);
    R->pattern_lo    = (uint32_t)(RET_PATTERN);
    R->tracked_mb    = TRACKED_MB;
    R->idle_seconds  = (uint32_t)IDLE_SECONDS;
    R->total_flips   = 0;
    R->idle_cyc_hi   = 0;
    R->idle_cyc_lo   = 0;
    asm volatile ("fence" ::: "memory");

    printf("=== March Retention Test ===\r\n");
    printf("Tracked : %u MB\r\n", TRACKED_MB);
    printf("Pattern : 0x%08x%08x\r\n", (uint32_t)(RET_PATTERN >> 32), (uint32_t)RET_PATTERN);
    printf("Stress  : %u s   Idle: %u s\r\n", (uint32_t)STRESS_SECONDS, (uint32_t)IDLE_SECONDS);
    printf("(result struct at 0x%08x — read via load.sh dump)\r\n\r\n", (uint32_t)RESULT_ADDR);
    uart_wait_tx_done();

    march_error_t errs[MARCH_MAX_ERRORS];
    static const uint64_t bg[2] = { 0x0000000000000000ULL, 0xAAAAAAAAAAAAAAAAULL };
    static const uint64_t iv[2] = { 0xFFFFFFFFFFFFFFFFULL, 0x5555555555555555ULL };

    /* -------------------------------------------------- PHASE 1: stress */
    uint64_t t0 = rdcycle64();
    uint32_t passes = 0;
    while ((rdcycle64() - t0) < STRESS_CYCLES) {
        uint32_t p = passes & 1u;                 /* alternate pattern each pass */
        uint32_t na = march_c_minus(TRK_A_BASE, TRK_A_WORDS, bg[p], iv[p], errs, MARCH_MAX_ERRORS);
        uint32_t nb = march_c_minus(TRK_B_BASE, TRK_B_WORDS, bg[p], iv[p], errs, MARCH_MAX_ERRORS);
        passes++;
        R->stress_passes = passes;
        R->stress_errors += na + nb;
        asm volatile ("fence" ::: "memory");
        printf("[stress] pass %u (0x%02x)  march-errors so far=%u\r\n",
               passes, (uint32_t)(bg[p] & 0xff), R->stress_errors);
        uart_wait_tx_done();
    }

    /* --------------------------------------------- FINAL WRITE of pattern */
    printf("[write] filling %u MB with 0x%08x%08x ...\r\n",
           TRACKED_MB, (uint32_t)(RET_PATTERN >> 32), (uint32_t)RET_PATTERN);
    uart_wait_tx_done();
    fill_pattern(TRK_A_BASE, TRK_A_WORDS, RET_PATTERN);
    fill_pattern(TRK_B_BASE, TRK_B_WORDS, RET_PATTERN);

    /* --------------------------------------------------- FLUSH to DRAM */
    printf("[flush] evicting caches -> DRAM (scratch sweep)\r\n");
    uart_wait_tx_done();
    cache_flush();

    /* --------------------------------------------------- PHASE 2: idle */
    R->phase = 2;
    asm volatile ("fence" ::: "memory");
    printf("[idle]  %u s — refresh only, zero core traffic\r\n", (uint32_t)IDLE_SECONDS);
    uart_wait_tx_done();
    uint64_t i0 = rdcycle64();
    idle_spin(IDLE_CYCLES);
    uint64_t idle_cyc = rdcycle64() - i0;

    /* ------------------------------------------------- PHASE 3: verify */
    R->phase = 3;
    asm volatile ("fence" ::: "memory");
    printf("[verify] reading back from DRAM, counting flips ...\r\n");
    uart_wait_tx_done();
    uint32_t flips = 0;
    flips += verify_pattern(TRK_A_BASE, TRK_A_WORDS, RET_PATTERN, errs, MARCH_MAX_ERRORS, flips);
    flips += verify_pattern(TRK_B_BASE, TRK_B_WORDS, RET_PATTERN, errs, MARCH_MAX_ERRORS, flips);

    /* record + flush result so JTAG dump (bypasses cache) reads the real value */
    R->total_flips = flips;
    R->idle_cyc_hi = (uint32_t)(idle_cyc >> 32);
    R->idle_cyc_lo = (uint32_t)(idle_cyc);
    uint32_t show = (flips < MARCH_MAX_ERRORS) ? flips : MARCH_MAX_ERRORS;
    for (uint32_t k = 0; k < show; k++)
        R->flips[k] = errs[k];
    R->magic = MAGIC_DONE;
    R->phase = 4;
    asm volatile ("fence" ::: "memory");
    cache_flush();

    /* ------------------------------------------------------- report */
    printf("\r\n=== RETENTION RESULT ===\r\n");
    printf("idle=%u s  tracked=%u MB  pattern=0x%08x%08x\r\n",
           (uint32_t)IDLE_SECONDS, TRACKED_MB,
           (uint32_t)(RET_PATTERN >> 32), (uint32_t)RET_PATTERN);
    if (flips == 0) {
        printf("PASS: 0 bit flips after idle -> DRAM refresh OK\r\n");
    } else {
        printf("FAIL: %u bit flips!\r\n", flips);
        for (uint32_t k = 0; k < show; k++) {
            uint32_t xh = errs[k].expected_hi ^ errs[k].actual_hi;
            uint32_t xl = errs[k].expected_lo ^ errs[k].actual_lo;
            printf("  [%02u] addr=0x%08x%08x exp=0x%08x%08x got=0x%08x%08x xor=0x%08x%08x\r\n",
                   k, errs[k].addr_hi, errs[k].addr_lo,
                   errs[k].expected_hi, errs[k].expected_lo,
                   errs[k].actual_hi, errs[k].actual_lo, xh, xl);
        }
        if (flips > MARCH_MAX_ERRORS)
            printf("  ...(%u more not shown)\r\n", flips - MARCH_MAX_ERRORS);
    }
    uart_wait_tx_done();

    /* ------------------------------------------ PHASE 3b: optional march */
    if (POST_CYCLES) {
        printf("[post] continuing march for %u s\r\n", (uint32_t)POST_MARCH_SECONDS);
        uart_wait_tx_done();
        uint64_t q0 = rdcycle64();
        uint32_t qp = 0;
        while ((rdcycle64() - q0) < POST_CYCLES) {
            uint32_t p = qp & 1u;
            march_c_minus(TRK_A_BASE, TRK_A_WORDS, bg[p], iv[p], errs, MARCH_MAX_ERRORS);
            march_c_minus(TRK_B_BASE, TRK_B_WORDS, bg[p], iv[p], errs, MARCH_MAX_ERRORS);
            qp++;
        }
    }

    printf("[done]\r\n");
    uart_wait_tx_done();
    while (1) asm volatile ("wfi");
    return 0;
}
