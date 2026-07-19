#include "march.h"

/*
 * mem_fence()
 *
 * Full RISC-V memory fence.  Ensures all previous stores are globally visible
 * before any subsequent load or store is issued.  Required between March
 * elements so that reads in element Mk actually observe the writes from Mk-1,
 * and not stale values from a store buffer or write-combining buffer.
 */
static void mem_fence(void) {
    asm volatile ("fence" ::: "memory");
}

/*
 * log_error()
 *
 * Appends one error record to errs[] if n < max, then returns n+1.
 */
static uint32_t log_error(march_error_t *errs, uint32_t n, uint32_t max,
                           uint64_t addr, uint64_t expected, uint64_t actual) {
    if (n < max) {
        errs[n].addr_lo     = (uint32_t)(addr);
        errs[n].addr_hi     = (uint32_t)(addr     >> 32);
        errs[n].expected_lo = (uint32_t)(expected);
        errs[n].expected_hi = (uint32_t)(expected >> 32);
        errs[n].actual_lo   = (uint32_t)(actual);
        errs[n].actual_hi   = (uint32_t)(actual   >> 32);
    }
    return n + 1;
}

/* -------------------------------------------------------------------------
 * march_c_minus — March C- (M0..M5) over num_words 64-bit words.
 *   M0: up(w bg)   M1: up(r bg,w inv)   M2: up(r inv,w bg)
 *   M3: dn(r bg,w inv)   M4: dn(r inv,w bg)   M5: up(r bg)
 * ------------------------------------------------------------------------- */
uint32_t march_c_minus(volatile uint64_t *base, uint32_t num_words,
                       uint64_t bg, uint64_t inv,
                       march_error_t *errs, uint32_t max_errors) {
    uint32_t errors = 0;
    uint64_t got;
    int32_t  i;

    /* M0: up(w bg) */
    for (uint32_t j = 0; j < num_words; j++)
        base[j] = bg;
    mem_fence();

    /* M1: up(r bg, w inv) */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], bg, got);
        base[j] = inv;
    }
    mem_fence();

    /* M2: up(r inv, w bg) */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != inv)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], inv, got);
        base[j] = bg;
    }
    mem_fence();

    /* M3: dn(r bg, w inv) */
    for (i = (int32_t)num_words - 1; i >= 0; i--) {
        got = base[i];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[i], bg, got);
        base[i] = inv;
    }
    mem_fence();

    /* M4: dn(r inv, w bg) */
    for (i = (int32_t)num_words - 1; i >= 0; i--) {
        got = base[i];
        if (got != inv)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[i], inv, got);
        base[i] = bg;
    }
    mem_fence();

    /* M5: up(r bg) — final verification */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], bg, got);
    }

    return errors;
}
