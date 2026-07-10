#ifndef MARCH_H
#define MARCH_H

#include <stdint.h>

/*
 * Maximum number of error records stored per pass.
 * Errors beyond this limit are counted but not recorded.
 */
#define MARCH_MAX_ERRORS 16

/*
 * A single error record.  Fields are split into hi/lo halves so the struct
 * is composed entirely of uint32_t — avoids alignment surprises when the
 * struct is written at an arbitrary DRAM address via a volatile pointer.
 */
typedef struct {
    uint32_t addr_hi;
    uint32_t addr_lo;
    uint32_t expected_hi;
    uint32_t expected_lo;
    uint32_t actual_hi;
    uint32_t actual_lo;
} march_error_t;

/*
 * march_c_minus()
 *
 * Executes the March C- algorithm over [base, base+num_words) using 64-bit
 * word granularity.
 *
 * March C- element sequence (↑ = ascending, ↓ = descending address order):
 *   M0: ↑(w bg)           write background to every word
 *   M1: ↑(r bg, w inv)    verify bg, write inv
 *   M2: ↑(r inv, w bg)    verify inv, write bg
 *   M3: ↓(r bg, w inv)    verify bg, write inv
 *   M4: ↓(r inv, w bg)    verify inv, write bg
 *   M5: ↑(r bg)           final read-back
 *
 * bg and inv must be bitwise complements.  Canonical choices:
 *   bg=0x0000000000000000  inv=0xFFFFFFFFFFFFFFFF   (all-zeros background)
 *   bg=0xAAAAAAAAAAAAAAAA  inv=0x5555555555555555   (checkerboard background)
 *
 * Parameters:
 *   base       - pointer to first 64-bit word of the test region
 *   num_words  - number of 64-bit words to test
 *   bg         - background pattern written in M0
 *   inv        - complement of bg
 *   errs       - caller-allocated array of at least max_errors entries
 *   max_errors - maximum entries to write into errs[]
 *
 * Returns:
 *   Total number of mismatches found.  Values beyond max_errors are counted
 *   but not written into errs[].
 */
uint32_t march_c_minus(volatile uint64_t *base, uint32_t num_words,
                       uint64_t bg, uint64_t inv,
                       march_error_t *errs, uint32_t max_errors);

#endif /* MARCH_H */
