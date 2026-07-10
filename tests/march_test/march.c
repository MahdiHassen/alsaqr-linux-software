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
 * The caller passes n (current count) and receives n+1; this avoids a
 * separate bounds check at every call site.
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
 * march_c_minus — see march.h for full description.
 * ------------------------------------------------------------------------- */
uint32_t march_c_minus(volatile uint64_t *base, uint32_t num_words,
                       uint64_t bg, uint64_t inv,
                       march_error_t *errs, uint32_t max_errors) {
    uint32_t errors = 0;
    uint64_t got;
    int32_t  i;          /* signed for the descending loops */

    /* ------------------------------------------------------------------
     * M0: ↑(w bg)
     *
     * Initialise every word to the background pattern.  This also confirms
     * that writes reach DRAM before we begin verifying in M1.
     * ------------------------------------------------------------------ */
    for (uint32_t j = 0; j < num_words; j++)
        base[j] = bg;
    mem_fence();

    /* ------------------------------------------------------------------
     * M1: ↑(r bg, w inv) — ascending
     *
     * For each word: verify the background written in M0, then overwrite
     * with the inverted pattern.  A mismatch here indicates a stuck-at or
     * transition fault introduced during the M0 write sweep.
     * ------------------------------------------------------------------ */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], bg, got);
        base[j] = inv;
    }
    mem_fence();

    /* ------------------------------------------------------------------
     * M2: ↑(r inv, w bg) — ascending
     *
     * Verify that M1's write of inv arrived correctly, then restore bg.
     * Coupling faults where a write to cell j flips a later cell j' are
     * detectable here (j' is read before it is itself written in this pass).
     * ------------------------------------------------------------------ */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != inv)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], inv, got);
        base[j] = bg;
    }
    mem_fence();

    /* ------------------------------------------------------------------
     * M3: ↓(r bg, w inv) — descending
     *
     * Reverse direction covers coupling faults that only manifest when the
     * aggressor address is higher than the victim address (idempotent
     * coupling faults, linked coupling faults).
     * ------------------------------------------------------------------ */
    for (i = (int32_t)num_words - 1; i >= 0; i--) {
        got = base[i];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[i], bg, got);
        base[i] = inv;
    }
    mem_fence();

    /* ------------------------------------------------------------------
     * M4: ↓(r inv, w bg) — descending
     * ------------------------------------------------------------------ */
    for (i = (int32_t)num_words - 1; i >= 0; i--) {
        got = base[i];
        if (got != inv)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[i], inv, got);
        base[i] = bg;
    }
    mem_fence();

    /* ------------------------------------------------------------------
     * M5: ↑(r bg) — final verification, ascending
     *
     * Confirms that no address-decoder or coupling fault corrupted a word
     * during M4's descending write sweep.  No write — this is the last
     * element.
     * ------------------------------------------------------------------ */
    for (uint32_t j = 0; j < num_words; j++) {
        got = base[j];
        if (got != bg)
            errors = log_error(errs, errors, max_errors,
                               (uint64_t)(uintptr_t)&base[j], bg, got);
    }

    return errors;
}
