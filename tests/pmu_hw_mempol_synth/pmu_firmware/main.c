/*
 * PMU Firmware - no-op.
 *
 * In this benchmark the hardware token buckets do all the bandwidth
 * regulation, so the APMU has no task to perform. The counters it exposes are
 * read directly over AXI-lite by CVA6 core 0 and increment in hardware whether
 * or not this core is executing.
 *
 * The mempol token-bucket implementation this replaces is still available in
 * tests/pmu_mempol_synth/pmu_firmware/main.c.
 */

#include <stdint.h>
#include "include/pmu_hw_desc.h"

// DSPM communication address (must match pmu_bench.c)
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)

// Status codes (must match pmu_bench.c)
#define STATUS_STARTED          0xBB000001

// Memory access macro
#define WRITE_MEM(addr, value)  (*((volatile uint32_t*)(addr)) = (value))

int main() {
    // Startup NOPs
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");

    // Signal PMU started
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_STARTED);

    return 0;
}
