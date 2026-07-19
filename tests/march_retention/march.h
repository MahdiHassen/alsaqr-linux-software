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
 * word granularity.  See march.c for the full element sequence.
 *
 * bg and inv must be bitwise complements.
 * Returns total mismatches found (values beyond max_errors counted, not stored).
 */
uint32_t march_c_minus(volatile uint64_t *base, uint32_t num_words,
                       uint64_t bg, uint64_t inv,
                       march_error_t *errs, uint32_t max_errors);

#endif /* MARCH_H */
