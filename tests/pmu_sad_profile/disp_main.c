#include <stdio.h>
#include <stdint.h>
#include "fat16.h"
#include "fat_file.h"
#include "sdvbs_common.h"
#include "sdvb/benchmarks/disparity/src/c/disparity_top.h"
#include "utils.h"

/* Mirror the pmu_test.c MMIO accessors and addresses */
#define read_32b(addr)        (*(volatile uint32_t *)(long)(addr))
#define write_32b(addr, val_) (*(volatile uint32_t *)(long)(addr) = (val_))

#define PMU_TIMER_ADDR  0x10405000  /* 64-bit timer: lo at +0, hi at +4 */
#define PMU_PERIOD_ADDR 0x10405008  /* 64-bit period: lo at +0, hi at +4 */
#define PMU_STATUS_ADDR 0x10406000

/* LLC setup — same as pmu_bench.c */
static void enable_llc(void) {
    write_32b(0x1C + 0x1A106000, 0xA0000000);
}

static void partition_cache_4cores(void) {
    write_32b(0x10401040, 0xF0FF0FFF);
    write_32b(0x10401044, 0xFF0FFFF0);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

static uint64_t read_pmu_timer(void) {
    uint32_t lo = read_32b(PMU_TIMER_ADDR);
    uint32_t hi = read_32b(PMU_TIMER_ADDR + 4);
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    FAT16 vol;

#ifdef FPGA_EMULATION
    uint32_t baud_rate = 9600;
    uint32_t test_freq = 100000000;
#else
    set_flls();
    uint32_t baud_rate = 115200;
    uint32_t test_freq = 50000000;
#endif
    uart_set_cfg(0, (test_freq / baud_rate) >> 4);

    enable_llc();
    partition_cache_4cores();

    printf("\r\n=== Disparity Benchmark (bare-metal FAT16) ===\r\n");

    if (fat16_mount(&vol) != 0) {
        printf("FAIL: fat16_mount - no FAT image loaded at 0x82100000\r\n");
        while (1) {}
    }
    printf("FAT mounted: bps=%u spc=%u root_sec=%u data_sec=%u\r\n",
           vol.bps, vol.spc, vol.root_sec, vol.data_sec);

    fat_file_init(&vol);

    /* Set PMU period to max (prevent overflow interrupt) and start timer */
    write_32b(PMU_PERIOD_ADDR,     0xFFFFFFFF);
    write_32b(PMU_PERIOD_ADDR + 4, 0xFFFFFFFF);
    write_32b(PMU_STATUS_ADDR, 1);

    char *argv[] = { "disp", "DISP" };
    int   argc   = 2;

    uint64_t t_start = read_pmu_timer();
    sdvb_disparity(argc, argv);
    uint64_t t_end = read_pmu_timer();

    write_32b(PMU_STATUS_ADDR, 0);

    uint64_t cycles = t_end - t_start;
    uint32_t cyc_hi = (uint32_t)(cycles >> 32);
    uint32_t cyc_lo = (uint32_t)(cycles & 0xFFFFFFFF);

    if (cyc_hi)
        printf("PMU cycles\t\t- %u%08u\r\n", cyc_hi, cyc_lo);
    else
        printf("PMU cycles\t\t- %u\r\n", cyc_lo);

    printf("=== Done ===\r\n");
    while (1) {}
    return 0;
}
