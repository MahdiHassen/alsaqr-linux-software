#include <stdio.h>
#include <stdint.h>
#include "fat16.h"
#include "fat_file.h"
#include "sdvbs_common.h"
#include "sdvb/benchmarks/mser/src/c/mser_top.h"

#define read_32b(addr)        (*(volatile uint32_t *)(long)(addr))
#define write_32b(addr, val_) (*(volatile uint32_t *)(long)(addr) = (val_))

#define PMU_TIMER_ADDR  0x10405000
#define PMU_PERIOD_ADDR 0x10405008
#define PMU_STATUS_ADDR 0x10406000

static uint64_t read_pmu_timer(void) {
    uint32_t lo = read_32b(PMU_TIMER_ADDR);
    uint32_t hi = read_32b(PMU_TIMER_ADDR + 4);
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    FAT16 vol;

    printf("\r\n=== MSER Benchmark (bare-metal FAT16) ===\r\n");

    if (fat16_mount(&vol) != 0) {
        printf("FAIL: fat16_mount\r\n");
        while (1) {}
    }
    printf("FAT mounted: bps=%u spc=%u root_sec=%u data_sec=%u\r\n",
           vol.bps, vol.spc, vol.root_sec, vol.data_sec);

    fat_file_init(&vol);

    write_32b(PMU_PERIOD_ADDR,     0xFFFFFFFF);
    write_32b(PMU_PERIOD_ADDR + 4, 0xFFFFFFFF);
    write_32b(PMU_STATUS_ADDR, 1);

    char *argv[] = { "mser", "MSER" };
    int   argc   = 2;

    uint64_t t_start = read_pmu_timer();
    sdvb_mser(argc, argv);
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
