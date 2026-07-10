#include "encoding.h"
#include <stdio.h>
#include <stdint.h>
#include "utils.h"
#include "march.c"

/* =========================================================================
 * DRAM layout — zero address overlap between any two cores.
 *
 * If cores shared any address, the CCU coherence protocol would satisfy
 * the request via L1-to-L1 snoop transfer, never reaching DRAM.
 * Exclusive partitioning eliminates all coherence traffic, so every
 * dirty eviction propagates L1 -> LLC -> DRAM.
 *
 *  0x80000000 - 0x80400000  4 MB  protected: ELF code + stacks (all 4 harts)
 *  0x80400000 - 0x85000000  76 MB core-0 march segment A
 *  0x85000000 - 0x85100000  1 MB  result area  (load.sh dumps from here)
 *  0x85100000 - 0x88000000  47 MB core-0 march segment B
 *  0x88000000 - 0x90000000  128 MB core-1 interference (exclusive)
 *  0x90000000 - 0x98000000  128 MB core-2 interference (exclusive)
 *  0x98000000 - 0xA0000000  128 MB core-3 interference (exclusive)
 *
 * DRAM accessible via HYAXI: 0x80000000 + 512 MB = 0xA0000000
 * Total verified by march:   ~123 MB  (segments A + B)
 * Total hit by interference: ~384 MB  (3 x 128 MB, write + read, not verified)
 * Total DRAM exercised:      ~507 MB / 512 MB  (~99%)
 * Untested:                  4 MB code + 1 MB result = 5 MB
 *
 * All working sets >> 1 MiB LLC (32 KB L1 x4 + 1 MiB LLC = ~1.125 MiB total).
 * ========================================================================= */

/* Core-0 march — two contiguous segments around the result area */
#define MARCH_A_BASE    ((volatile uint64_t *)0x80400000UL)
#define MARCH_A_END     0x85000000UL
#define MARCH_A_BYTES   (MARCH_A_END - 0x80400000UL)
#define MARCH_A_WORDS   (MARCH_A_BYTES / sizeof(uint64_t))   /* ~9.96 M words */

#define MARCH_B_BASE    ((volatile uint64_t *)0x85100000UL)
#define MARCH_B_END     0x88000000UL
#define MARCH_B_BYTES   (MARCH_B_END - 0x85100000UL)
#define MARCH_B_WORDS   (MARCH_B_BYTES / sizeof(uint64_t))   /* ~6.16 M words */

#define MARCH_TOTAL_MB  ((uint32_t)((MARCH_A_BYTES + MARCH_B_BYTES) >> 20))

/* Result dump — kept at 0x85000000 so load.sh needs no changes */
#define RESULT_ADDR     0x85000000UL

/* Interference — 128 MB each, exclusive to each hart */
#define INTF_BASE_1     ((volatile uint64_t *)0x88000000UL)
#define INTF_BASE_2     ((volatile uint64_t *)0x90000000UL)
#define INTF_BASE_3     ((volatile uint64_t *)0x98000000UL)
#define INTF_BYTES      (128UL * 1024UL * 1024UL)
#define INTF_WORDS      (INTF_BYTES / sizeof(uint64_t))       /* 16 M words */

#define MAGIC_RUNNING   0x600DC0DEUL
#define MAGIC_STOPPED   0xDEADDEADUL

typedef struct {
    uint32_t magic;
    uint32_t pass;
    uint32_t total_errors;
    uint32_t pass_errors;
    uint32_t cycles_hi;
    uint32_t cycles_lo;
    uint32_t march_a_base;    /* 0x80400000 */
    uint32_t march_a_mb;
    uint32_t march_b_base;    /* 0x85100000 */
    uint32_t march_b_mb;
    march_error_t errors[MARCH_MAX_ERRORS];
} result_t;

static volatile result_t *const result = (volatile result_t *)RESULT_ADDR;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline uint64_t rdcycle64(void) {
    uint64_t c;
    asm volatile ("rdcycle %0" : "=r" (c));
    return c;
}

static void result_update(uint32_t pass, uint32_t total_errors,
                          uint32_t pass_errors, uint64_t cycles,
                          march_error_t *errs) {
    result->pass         = pass;
    result->total_errors = total_errors;
    result->pass_errors  = pass_errors;
    result->cycles_hi    = (uint32_t)(cycles >> 32);
    result->cycles_lo    = (uint32_t)(cycles);
    uint32_t n = (pass_errors < MARCH_MAX_ERRORS) ? pass_errors : MARCH_MAX_ERRORS;
    for (uint32_t k = 0; k < n; k++)
        result->errors[k] = errs[k];
    asm volatile ("fence" ::: "memory");
}

/* -------------------------------------------------------------------------
 * Interference generator — harts 1-3.
 *
 * Each hart owns an exclusive 128 MB region.  No other hart ever touches
 * these addresses, so the coherence protocol has nothing to do: every
 * store evicts L1 -> LLC -> DRAM and every load fetches from DRAM.
 *
 * Mixed write+read passes: writes dirty lines into DRAM via eviction;
 * reads fetch them back from DRAM (128 MB >> LLC so lines are gone).
 * Together they stress the DDR controller's read/write queues and force
 * refresh commands to interleave with real traffic.
 * ------------------------------------------------------------------------- */
static void interference_loop(volatile uint64_t *base, uint32_t num_words,
                               uint32_t seed) {
    uint64_t val = (uint64_t)seed * 0x9E3779B97F4A7C15ULL;
    while (1) {
        /* Write pass */
        for (uint32_t i = 0; i < num_words; i++) {
            base[i] = val;
            val ^= val << 13;
            val ^= val >> 7;
            val ^= val << 17;
        }
        asm volatile ("fence" ::: "memory");

        /* Read pass — forces DRAM reads (128 MB >> LLC, all lines evicted) */
        volatile uint64_t sink = 0;
        for (uint32_t i = 0; i < num_words; i++)
            sink ^= base[i];
        asm volatile ("fence" ::: "memory");
        (void)sink;
    }
}

/* -------------------------------------------------------------------------
 * march_pass — run March C- over both segments for one pattern pair.
 *
 * Errors from segment A fill errs[] first; segment B uses remaining slots.
 * Total error count is always accurate even when the buffer saturates.
 * ------------------------------------------------------------------------- */
static uint32_t march_pass(uint64_t bg, uint64_t inv, march_error_t *errs) {
    uint32_t na = march_c_minus(MARCH_A_BASE, (uint32_t)MARCH_A_WORDS,
                                bg, inv, errs, MARCH_MAX_ERRORS);
    uint32_t slot = (na < MARCH_MAX_ERRORS) ? na : MARCH_MAX_ERRORS;
    uint32_t nb = march_c_minus(MARCH_B_BASE, (uint32_t)MARCH_B_WORDS,
                                bg, inv, errs + slot, MARCH_MAX_ERRORS - slot);
    return na + nb;
}

/* =========================================================================
 * main — entry point for all harts
 * ========================================================================= */
int main(int argc, char const *argv[]) {
    uint32_t mhartid;
    asm volatile ("csrr %0, 0xF14" : "=r" (mhartid));

    /* Harts 1-3 immediately enter their exclusive interference regions.
     * Each region is disjoint from every other hart's region — the CCU
     * sees no sharing, so all traffic goes directly to DRAM. */
    if (mhartid == 1) interference_loop(INTF_BASE_1, (uint32_t)INTF_WORDS, 0xDEADBEEF);
    if (mhartid == 2) interference_loop(INTF_BASE_2, (uint32_t)INTF_WORDS, 0xCAFEBABE);
    if (mhartid == 3) interference_loop(INTF_BASE_3, (uint32_t)INTF_WORDS, 0xFEEDFACE);
    if (mhartid > 3) { while (1) asm volatile ("wfi"); }

    /* Hart 0 only below this point */
#ifdef FPGA_EMULATION
    uint32_t baud_rate = 9600;
    uint32_t test_freq = 100000000;
#else
    set_flls();
    uint32_t baud_rate = 115200;
    uint32_t test_freq = 50000000;
#endif
    uart_set_cfg(0, (test_freq / baud_rate) >> 4);

    result->magic        = MAGIC_RUNNING;
    result->pass         = 0;
    result->total_errors = 0;
    result->pass_errors  = 0;
    result->cycles_hi    = 0;
    result->cycles_lo    = 0;
    result->march_a_base = 0x80400000U;
    result->march_a_mb   = (uint32_t)(MARCH_A_BYTES >> 20);
    result->march_b_base = 0x85100000U;
    result->march_b_mb   = (uint32_t)(MARCH_B_BYTES >> 20);
    asm volatile ("fence" ::: "memory");

    printf("=== March C- Full DRAM Coverage Test ===\r\n");
    printf("Seg A : 0x80400000  %u MB\r\n",    (uint32_t)(MARCH_A_BYTES >> 20));
    printf("Seg B : 0x85100000  %u MB\r\n",    (uint32_t)(MARCH_B_BYTES >> 20));
    printf("March : %u MB verified / 512 MB\r\n", MARCH_TOTAL_MB);
    printf("Intf  : 3x 128 MB exclusive (0x88/0x90/0x98 M)\r\n");
    printf("Est.  : ~10 min per pass at FPGA speed\r\n\r\n");
    uart_wait_tx_done();

    static const uint64_t bg_pat[2]  = {
        0x0000000000000000ULL,
        0xAAAAAAAAAAAAAAAAULL,
    };
    static const uint64_t inv_pat[2] = {
        0xFFFFFFFFFFFFFFFFULL,
        0x5555555555555555ULL,
    };
    static const char *pat_name[2] = { "0x00/0xFF", "0xAA/0x55" };

    march_error_t errs[MARCH_MAX_ERRORS];
    uint32_t total_errors = 0;
    uint32_t pass = 0;

    while (1) {
        uint32_t pass_errors = 0;
        uint64_t t_start = rdcycle64();

        for (int p = 0; p < 2; p++) {
            uint32_t n = march_pass(bg_pat[p], inv_pat[p], errs);
            if (n > 0) {
                printf("[pass %u / %s] %u ERRORS:\r\n",
                       pass + 1, pat_name[p], n);
                uint32_t show = (n < MARCH_MAX_ERRORS) ? n : MARCH_MAX_ERRORS;
                for (uint32_t e = 0; e < show; e++)
                    printf("  [%02u] addr=0x%08x%08x  exp=0x%08x%08x"
                           "  got=0x%08x%08x\r\n",
                           e,
                           errs[e].addr_hi,     errs[e].addr_lo,
                           errs[e].expected_hi, errs[e].expected_lo,
                           errs[e].actual_hi,   errs[e].actual_lo);
                if (n > MARCH_MAX_ERRORS)
                    printf("  ...(%u more not shown)\r\n", n - MARCH_MAX_ERRORS);
                uart_wait_tx_done();
            }
            pass_errors += n;
        }

        uint64_t cycles = rdcycle64() - t_start;
        pass++;
        total_errors += pass_errors;
        result_update(pass, total_errors, pass_errors, cycles, errs);

        uint32_t cyc_lo = (uint32_t)(cycles);
        uint32_t cyc_hi = (uint32_t)(cycles >> 32);
        if (cyc_hi)
            printf("[pass %u] errors=%u  total=%u  cycles=0x%x_%08x\r\n",
                   pass, pass_errors, total_errors, cyc_hi, cyc_lo);
        else
            printf("[pass %u] errors=%u  total=%u  cycles=%u\r\n",
                   pass, pass_errors, total_errors, cyc_lo);
        uart_wait_tx_done();
    }

    result->magic = MAGIC_STOPPED;
    asm volatile ("fence" ::: "memory");
    return 0;
}
