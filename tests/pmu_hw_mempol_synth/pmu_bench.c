/*
 * Hardware token-bucket memory-bandwidth regulation benchmark.
 *
 * Core 0 (the CUA) runs six timed stages -- LLC-resident and DRAM-streaming,
 * store and load variants -- while cores 1-3 run a continuous interference
 * workload. All four cores are throttled by the per-core hardware token
 * buckets, which are programmed once during setup and enabled for the whole
 * measured run.
 *
 * The APMU is used only for its performance counter bundles, which count
 * LLC/MEM reads and writes per core. Its Ibex core is still booted, but the
 * firmware it runs is a no-op that only writes STATUS_STARTED so core 0 can
 * confirm the APMU is alive; it takes no part in the regulation.
 */

#include "encoding.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
//#include "iopmp.h" // Disabled 
#include "pmu_defines.h"
#include "tb_defines.h"

// 32-bit MMIO accessors (previously pulled in via pmu_test_func.h)
#define read_32b(addr)         (*(volatile uint32_t *)(long)(addr))
#define write_32b(addr, val_)  (*(volatile uint32_t *)(long)(addr) = val_)

// Cacheable memory base addresses per core (in LLC range 0x80000000-0xA0000000)
#define CORE0_DATA_BASE   0x81000000
#define CORE1_DATA_BASE   0x81100000
#define CORE2_DATA_BASE   0x81200000
#define CORE3_DATA_BASE   0x81300000

#define CACHE_LINE_SIZE 64 // This is in bytes

// DSPM communication address (must match firmware)
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)

// Status codes (must match firmware)
#define STATUS_STARTED          0xBB000001

// Sentinel values for the core-release handshake (see g_go below)
#define GO_IDLE    0xDEAD0000u
#define GO_RUN     0xCAFE0001u

// Working-set sizes, in 64 B cache lines. The per-core LLC partition is
// 4 ways x 64 KB = 256 KB, so WORK_READ_LINES exactly fills it and
// DRAM_READ_LINES is twice its size.
#define WORK_READ_LINES   4096   // 256 KB - fills one LLC partition
#define LLC_READ_LINES    2048   // 128 KB - half a partition, stays resident
#define DRAM_READ_LINES   8192   // 512 KB - 2x the partition, forces DRAM

#define STAGE_WARMUP      1      // untimed passes at the head of each stage
#define STAGE_ITERATIONS  10     
#define NUMBER_OF_STAGES  6
#define TOTAL_STAGE_ITERS ((STAGE_WARMUP + STAGE_ITERATIONS) * NUMBER_OF_STAGES)

// External symbols for PMU firmware
extern unsigned char _binary_text_section_bin_start[];
extern unsigned char _binary_text_section_bin_end[];
extern unsigned char _binary_data_rodata_bss_bin_start[];
extern unsigned char _binary_data_rodata_bss_bin_end[];

// *********************************************************************
// LLC Config and Partitioning 
// *********************************************************************

// Enables the LLC for 0x8000_0000 - 0x8300_0000
void enable_llc(void) {
    write_32b(0x1C + 0x1A106000, 0x83000000);
}

// Partition 16-way LLC: 4 ways per core
void partition_cache_4cores(void) {
    write_32b(0x10401040, 0xF0FF0FFF);
    write_32b(0x10401044, 0xFF0FFFF0);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// *********************************************************************
// Token Bucket Defines and Functions
//
// Parameter translation from software mempol:
//
//   software mempol                        hardware bucket
//   -------------------------------------  ------------------------------
//   V = aLLCr*LLCr + aLLCw*LLCw            The bucket sees only the core
//     + aMEMr*MEMr + aMEMw*MEMw            port and cannot tell an LLC hit
//                                          from a DRAM miss, so each request
//                                          is charged its worst case:
//     aLLCr=10000, aMEMr=6120           -> AR_COST = 10000 + 6120 = 16120
//     aLLCw=0,     aMEMw=4400           -> AW_COST =     0 + 4400 =  4400
//
//   token += B*P - V   (B=86, P=2000)   -> REFILL_TOKENS = 86, REFILL_INTERVAL=1
//   cap = w*P*B        (w=2)            -> TOKEN_LIMIT = 2 * 2000 * 86 = 344000
//
// B = 86 is 0.0086 requests per cycle, carried in the same scale (10000 units = one LLC read request)
// *********************************************************************

#define TB_TOKEN_LIMIT      344000   
#define TB_REFILL_TOKENS    86       
#define TB_REFILL_INTERVAL  1        
#define TB_AW_COST          4400     
#define TB_AR_COST          16120    

#define TB_CORE_ADDR(core)  (TB_CFG_BASE_ADDR + (core) * TB_CORE_BYTES)

// Configures one bucket. Leaves it disabled.
void tb_configure_core(uint32_t core, uint32_t limit, uint32_t refill,
                       uint32_t interval, uint32_t aw_cost, uint32_t ar_cost) {
    uint64_t base = TB_CORE_ADDR(core);
    write_32b(base + TOKEN_LIMIT_OFFSET,     limit);
    write_32b(base + REFILL_TOKENS_OFFSET,   refill);
    write_32b(base + REFILL_INTERVAL_OFFSET, interval);
    write_32b(base + AW_COST_OFFSET,         aw_cost);
    write_32b(base + AR_COST_OFFSET,         ar_cost);
    write_32b(base + CTRL_OFFSET,            CTRL_DISABLE);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// All four cores get the same budget.
void tb_configure_all(void) {
    for (uint32_t core = 0; core < NUM_CORES; core++) {
        tb_configure_core(core, TB_TOKEN_LIMIT, TB_REFILL_TOKENS,
                          TB_REFILL_INTERVAL, TB_AW_COST, TB_AR_COST);
    }
    printf("    Token buckets: limit=%u refill=%u/%ucyc aw=%u ar=%u (cores 0-%u)\r\n",
           TB_TOKEN_LIMIT, TB_REFILL_TOKENS, TB_REFILL_INTERVAL,
           TB_AW_COST, TB_AR_COST, NUM_CORES - 1);
}

void tb_enable_all(void) {
    for (uint32_t core = 0; core < NUM_CORES; core++)
        write_32b(TB_CORE_ADDR(core) + CTRL_OFFSET, CTRL_ENABLE);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

void tb_disable_all(void) {
    for (uint32_t core = 0; core < NUM_CORES; core++)
        write_32b(TB_CORE_ADDR(core) + CTRL_OFFSET, CTRL_DISABLE);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// *********************************************************************
// Global Cross-Core Variables and APMU Counter Functions 
// *********************************************************************

volatile uint32_t g_go = GO_IDLE;
volatile uint32_t g_core_ready[NUM_CORES] = {0, 0, 0, 0};

volatile uint32_t g_core_done[NUM_CORES] = {0, 0, 0, 0};  // Set when a core finishes work
volatile uint32_t g_interference_go = 0;  // Core 0 sets this to start interference on cores 1-3

// Reset the performance counters for one core
// (cnt 4-7: LLCr, 8-11: LLCw, 12-15: MEMr, 16-19: MEMw)
void reset_counters(uint32_t core) {
    uint32_t offsets[4] = {4, 8, 12, 16};
    for (uint32_t g = 0; g < 4; g++) {
        write_32b(COUNTER_BASE_ADDR + (offsets[g] + core) * COUNTER_BUNDLE_SIZE, 0x00000000);
    }
    asm volatile ("fence iorw, iorw" ::: "memory");
}

void print_interferer_counters(void) {
    printf("\r\n========================================\r\n");
    printf("Interferer totals, whole run (cores 1-3)\r\n");
    printf("========================================\r\n");
    for (uint32_t core = 1; core < NUM_CORES; core++) {
        uint32_t llc_rd = read_32b(COUNTER_BASE_ADDR + (4  + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t llc_wr = read_32b(COUNTER_BASE_ADDR + (8  + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t mem_rd = read_32b(COUNTER_BASE_ADDR + (12 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t mem_wr = read_32b(COUNTER_BASE_ADDR + (16 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        printf("Core %u: LLC_RD=%u, LLC_WR=%u, MEM_RD=%u, MEM_WR=%u, total=%u\r\n",
               core, llc_rd, llc_wr, mem_rd, mem_wr,
               llc_rd + llc_wr + mem_rd + mem_wr);
        uart_wait_tx_done();
    }
    printf("========================================\r\n");
}

// *********************************************************************
// Cache Priming and Interference cores' work functions
// *********************************************************************

static uint32_t* const core_data_base[NUM_CORES] = {
    (uint32_t*)CORE0_DATA_BASE, (uint32_t*)CORE1_DATA_BASE,
    (uint32_t*)CORE2_DATA_BASE, (uint32_t*)CORE3_DATA_BASE
};

void prime_cache(uint32_t core){
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0; // Volatile here might be redundant.
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))]; // 64B/8B = 8 uint64 elements per cache line
}

void read_interference_cache(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < 4096; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
} // 4096 lines = 256 KB, fits the LLC partition, so these stay LLC-resident.

void read_interference_mem(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < 8192; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
} // 8192 lines = 512 KB, twice the partition, so every access misses to DRAM.

void write_interference_cache(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    for (uint32_t i = 0; i < 4096; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
}

void write_interference_mem(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    for (uint32_t i = 0; i < DRAM_READ_LINES; i++)   
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
} // Stores are write-allocate on CVA6: each line costs an RFO read plus a
  // later writeback, i.e. two bucket charges, not one.

// **********************************************
// CUA Work Functions
// **********************************************

//-----CUA Read/Write Groups on results/fig5_orig.jpg of Research Paper------

// Stage 1 & 3: LLC-resident region (LLC_READ_LINES * 64 B = 128 KB), inside
// the 256 KB per-core LLC partition, so every access is an LLC hit.
void llc_stage_work(void) {
    volatile uint64_t *data = (volatile uint64_t *)CORE0_DATA_BASE;
    for (uint32_t i = 0; i < LLC_READ_LINES; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
} 

// Stage 2: region twice the LLC partition — every access misses out to DRAM
void dram_stage_work(void) {
    volatile uint64_t *data = (volatile uint64_t *)CORE0_DATA_BASE;
    for (uint32_t i = 0; i < DRAM_READ_LINES; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
}


//------CUA Read Groups on results/fig5_orig.jpg of Research Paper------

void llc_stage_work_read(void) {
    volatile uint64_t *data = (volatile uint64_t *)CORE0_DATA_BASE;
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < LLC_READ_LINES; i++) 
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
}

void dram_stage_work_read(void) {
    volatile uint64_t *data = (volatile uint64_t *)CORE0_DATA_BASE;
    volatile uint64_t dummy;
    for (uint32_t i = 0; i < DRAM_READ_LINES; i++) 
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
}

// *********************************************************************
// Trap Handler and Counter IRQ Disable Function
// *********************************************************************
void __attribute__((naked, aligned(4))) handle_trap(void) {
    asm volatile ("1: j 1b\n\t");
}

// Just in case.
void disarm_counter_irqs(void) {
    for (uint32_t core = 0; core < NUM_CORES; core++) {
        write_32b(EVENT_INFO_BASE_ADDR + core * COUNTER_BUNDLE_SIZE, 0);
        write_32b(COUNTER_BASE_ADDR    + core * COUNTER_BUNDLE_SIZE, 0);
    }
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// *********************************************************************
// Thread Entry - Override weak version to let all cores proceed to main()
// *********************************************************************
void thread_entry(int cid, int nc) {
    // Return immediately - all cores will proceed to main()
    // where we handle per-core logic with mhartid checks
    return;
}

// *********************************************************************
// Main Function
// *********************************************************************
int main(int argc, char const *argv[]) {

    uint32_t mhartid;
    asm volatile (
        "csrr %0, 0xF14\n"
        : "=r" (mhartid)
    );

    // *******************************************************************
    // Core 0
    // *******************************************************************
    if (mhartid == 0) {
        #ifndef FPGA_EMULATION
        uint32_t baud_rate = 115200;
        uint32_t test_freq = 50000000;
        uart_set_cfg(0, (test_freq / baud_rate) >> 4);
        #endif

        printf("\r\n");
        printf("========================================\r\n");
        printf("Hardware Token Bucket LLC/DRAM Benchmark\r\n");
        printf("========================================\r\n");

        if (g_go != GO_IDLE) {
            printf("    *** WARNING: g_go = 0x%08x at entry, expected 0x%08x.\r\n", (uint32_t)g_go, GO_IDLE);
            printf("    *** .data was not reloaded; cores 1-3 started unregulated.\r\n");
            uart_wait_tx_done();
        }

        // Explicitly reinitialize globals (in case BSS not cleared)
        for (uint32_t i = 0; i < NUM_CORES; i++) {
            g_core_done[i] = 0;
            g_core_ready[i] = 0;
        }
        g_interference_go = 0;
        asm volatile ("fence iorw, iorw" ::: "memory");


        // ************************************************************************
        // Load PMU Firmware
        // ************************************************************************
        // APMU: performance counters only
        //
        // Regulation is done entirely by the hardware token buckets. The Ibex is
        // still loaded and released here, but its firmware is a no-op that just
        // writes STATUS_STARTED; the handshake below is a liveness check on the
        // APMU, not part of the measurement. The counter bundles are memory
        // mapped and are read directly by core 0 over AXI-lite.

        printf("[1] Loading PMU firmware...\r\n");

        uint32_t text_size = _binary_text_section_bin_end - _binary_text_section_bin_start;
        uint32_t data_size = _binary_data_rodata_bss_bin_end - _binary_data_rodata_bss_bin_start;

        // Halt PMU
        write_32b(PMC_STATUS_ADDR, 1);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Configure PMU timer period to max so it free-runs
        write_32b(PERIOD_ADDR, 0xFFFFFFFF);
        write_32b(PERIOD_ADDR + 4, 0xFFFFFFFF);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Clear DSPM communication area
        for (uint32_t i = 0; i < 0x200; i += 4) {
            write_32b(DSPM_BASE_ADDR + i, 0);
        }

        // Load firmware
        if (text_size > 0 && text_size < 4096) {
            memcpy((void*)ISPM_BASE_ADDR, _binary_text_section_bin_start, text_size);
            memcpy((void*)(DSPM_BASE_ADDR + 0x200), _binary_data_rodata_bss_bin_start, data_size);
            asm volatile ("fence iorw, iorw" ::: "memory");
            printf("    Firmware loaded (text=%u, data=%u bytes)\r\n", text_size, data_size);
        } else {
            printf("    ERROR: Invalid firmware size (text=%u)\r\n", text_size);
            while(1) asm volatile ("wfi");
        }

        uint16_t  md = 0;
        uintptr_t start_raddr = 0x1C000000;

        // IOPMP disabled cause it prevents token bucket read/writes
        // // Configure IOPMP
        // enable_iopmp();
        // mdcfg_entry_config(5, 0);
        // srcmd_entry_config(&md, 1, 0, 0);

        // // Put 1 TOR entry allowing reads and writes
        // //set_entry_off(start_raddr, ACCESS_NONE, 0);
        // set_entry_tor(start_raddr + 64, ACCESS_READ | ACCESS_WRITE, 1);

        // Start PMU
        write_32b(PMC_BOOT_ADDR, ISPM_BASE_ADDR);
        write_32b(PMC_STATUS_ADDR, 0);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Wait for PMU to start
        printf("[2] Waiting for PMU to start...\r\n");
        uint32_t timeout = 100000;
        while (read_32b(DSPM_STATUS_ADDR) != STATUS_STARTED && timeout > 0) {
            timeout--;
            asm volatile ("nop");
        }
        if (timeout == 0) {
            printf("    TIMEOUT waiting for PMU (status=0x%08x)\r\n", read_32b(DSPM_STATUS_ADDR));
            while(1) asm volatile ("wfi");
        }
        printf("    PMU started!\r\n");
        uart_wait_tx_done();

        // ************************************************************************
        // PMU counter configuration for all cores
        // ************************************************************************
        printf("[2] Configuring PMU counters...\r\n");
        disarm_counter_irqs();

        // Release cores 1-3.
        g_go = GO_RUN;
        asm volatile ("fence iorw, iorw" ::: "memory");
        printf("    Released cores 1-3\r\n");

        printf("    Waiting for cores 1-3 to park...\r\n");
        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint32_t timeout = 50000000;
            while (!g_core_ready[core] && timeout > 0) {
                timeout--;
                asm volatile ("nop");
            }
            if (timeout == 0) {
                printf("    FATAL: Core %u did not signal ready -- aborting\r\n", core);
                uart_wait_tx_done();
                while(1) asm volatile ("wfi");
            }
        }
        printf("    All cores ready!\r\n");

        // ************************************************************************
        // Enable LLC and configure counters 4-7 for LLC_RD_REQ per core
        // ************************************************************************
        enable_llc();
        partition_cache_4cores();
        printf("    LLC enabled + partitioned (4 ways/core, 16-way)\r\n");

        // Token Buckets are configured, but kept disabled.
        tb_configure_all();

        // cnt 0-3 send interrupts to cores 0-3
        // cnt 4-7 -> LLCr
        // cnt 8-11 -> LLCw
        // cnt 12-15 -> MEMr
        // cnt 16-19 -> MEMw
        uint32_t llc_rd_events[NUM_CORES] = {
            LLC_RD_REQ_CORE_0, LLC_RD_REQ_CORE_1,
            LLC_RD_REQ_CORE_2, LLC_RD_REQ_CORE_3
        };
        uint32_t llc_wr_events[NUM_CORES] = {
            LLC_WR_REQ_CORE_0, LLC_WR_REQ_CORE_1,
            LLC_WR_REQ_CORE_2, LLC_WR_REQ_CORE_3
        };
        uint32_t mem_rd_events[NUM_CORES] = {
            MEM_RD_REQ_CORE_0, MEM_RD_REQ_CORE_1,
            MEM_RD_REQ_CORE_2, MEM_RD_REQ_CORE_3
        };
        uint32_t mem_wr_events[NUM_CORES] = {
            MEM_WR_REQ_CORE_0, MEM_WR_REQ_CORE_1,
            MEM_WR_REQ_CORE_2, MEM_WR_REQ_CORE_3
        };
        uint32_t *event_groups[4] = {llc_rd_events, llc_wr_events, mem_rd_events, mem_wr_events};
        uint32_t group_base[4] = {4, 8, 12, 16};


        for (uint32_t g = 0; g < 4; g++) {
            for (uint32_t core = 0; core < NUM_CORES; core++) {
                uint32_t cnt_idx = group_base[g] + core;
                write_32b(EVENT_SEL_BASE_ADDR  + cnt_idx * COUNTER_BUNDLE_SIZE, event_groups[g][core]);
                write_32b(EVENT_INFO_BASE_ADDR + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
                write_32b(COUNTER_BASE_ADDR    + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
            }
        }
        printf("    Counters 4-7: LLC_RD, 8-11: LLC_WR, 12-15: MEM_RD, 16-19: MEM_WR per core\r\n");


        // ************************************************************************
        // Setup phase: prime core 0's partition and start the interferers, all
        // before the token buckets are armed, so setup traffic is unthrottled
        // ************************************************************************
        printf("[3] Priming caches and starting interference...\r\n");
        uart_wait_tx_done();

        prime_cache(0);

        // Reset performance counters so PMU starts with clean measurements
        for (uint32_t core = 0; core < NUM_CORES; core++)
            reset_counters(core);

        // Signal cores 1-3 to start interference work
        g_interference_go = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Brief delay for cores 1-3 to begin their work loops
        for (volatile uint32_t i = 0; i < 1000; i++) asm volatile ("nop"); //SIM CHANGE

        printf("    Setup complete, token buckets armed below for the whole run\r\n");
        uart_wait_tx_done();

        // Buffers for deferred CSV print -- filled during work, printed at the end through UART.
        uint32_t buf_llcrd[TOTAL_STAGE_ITERS], buf_llcwr[TOTAL_STAGE_ITERS];
        uint32_t buf_memrd[TOTAL_STAGE_ITERS], buf_memwr[TOTAL_STAGE_ITERS];
        uint32_t buf_cyc[TOTAL_STAGE_ITERS];
        uint8_t  buf_stage[TOTAL_STAGE_ITERS];   // stage tag, 0-5

        uint32_t global_iter = 0;
        uint64_t t_s1, t_s1_end, t_s2, t_s2_end, t_s3, t_s3_end;
        uint64_t t_s4, t_s4_end, t_s5, t_s5_end, t_s6, t_s6_end;

        tb_enable_all();

        // Stage 1: LLC — warm hits. STAGE_WARMUP head iterations are untimed and
        // excluded from the summary; they are still dumped in the table, tagged '*'.
        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s1));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0)); //PMU cycle counters replaced with CVA6 mcycle.
            llc_stage_work();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 0;
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s1_end));

        // Stage 2: DRAM — working set exceeds the partition, MEM_RD/MEM_WR spike
        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s2));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0));
            dram_stage_work();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 1;
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s2_end));

        // Stage 3: LLC — back to a resident working set, DRAM traffic drops away
        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s3));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0));
            llc_stage_work();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 2;
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s3_end));

        //Read Stages 
        prime_cache(0);
        
        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s4));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0));
            llc_stage_work_read();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 3;
            
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s4_end));

        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s5));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0));
            dram_stage_work_read();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 4;
            
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s5_end));

        for (uint32_t i = 0; i < STAGE_WARMUP + STAGE_ITERATIONS; i++, global_iter++) {
            if (i == STAGE_WARMUP)   // warm-up done, start the stage clock
                asm volatile ("csrr %0, mcycle" : "=r"(t_s6));
            reset_counters(0);
            uint64_t t0;
            asm volatile ("csrr %0, mcycle" : "=r"(t0));
            llc_stage_work_read();
            uint64_t t1;
            asm volatile ("csrr %0, mcycle" : "=r"(t1));
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 5;
            
        }
        asm volatile ("csrr %0, mcycle" : "=r"(t_s6_end));

        // All stages done. Disarm so reporting and CSV dumping run free.
        tb_disable_all();

        // Core 0 is done - signal completion
        g_core_done[0] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // ************************************************************************
        // Wait for ALL cores to finish before reporting results
        // ************************************************************************
        printf("[4a] Waiting for cores 1-3 to signal done...\r\n");
        uart_wait_tx_done();

        uint32_t all_done = 1;
        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint32_t timeout = 50000000;
            while (!g_core_done[core] && timeout > 0) {
                timeout--;
                asm volatile ("nop");
            }
            if (timeout == 0) {
                all_done = 0;
                printf("    WARNING: Core %u did not finish\r\n", core);
            } else {
                printf("    Core %u done\r\n", core);
            }
            uart_wait_tx_done();
        }

        if (all_done)
            printf("\r\n[4b] All cores finished!\r\n");
        else
            printf("\r\n[4b] WARNING: some interferers still running -- "
                   "totals below are not run-length totals.\r\n");

        // Latency calculations depend on LLC_READ_LINES and DRAM_READ_LINES defines.
        printf("\r\n--- Stage timing summary ---\r\n");
        printf("\r\nCUA Read/Write \r\n");
        printf("  Stage 1 (LLC):  %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s1_end - t_s1), (uint32_t)(t_s1_end - t_s1) / STAGE_ITERATIONS, ((uint32_t)(t_s1_end - t_s1) / (STAGE_ITERATIONS*LLC_READ_LINES)));
        printf("  Stage 2 (DRAM): %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s2_end - t_s2), (uint32_t)(t_s2_end - t_s2) / STAGE_ITERATIONS, ((uint32_t)(t_s2_end - t_s2) / (STAGE_ITERATIONS*DRAM_READ_LINES)));
        printf("  Stage 3 (LLC):  %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s3_end - t_s3), (uint32_t)(t_s3_end - t_s3) / STAGE_ITERATIONS, ((uint32_t)(t_s3_end - t_s3) / (STAGE_ITERATIONS*LLC_READ_LINES)));

        printf("\r\nCUA Read:\r\n");
        printf("  Stage 4 (LLC):  %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s4_end - t_s4), (uint32_t)(t_s4_end - t_s4) / STAGE_ITERATIONS, ((uint32_t)(t_s4_end - t_s4) / (STAGE_ITERATIONS*LLC_READ_LINES)));
        printf("  Stage 5 (DRAM): %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s5_end - t_s5), (uint32_t)(t_s5_end - t_s5) / STAGE_ITERATIONS, ((uint32_t)(t_s5_end - t_s5) / (STAGE_ITERATIONS*DRAM_READ_LINES)));
        printf("  Stage 6 (LLC):  %u cycles total, %u avg cc/iter, Latency: %u cc/instr \r\n",
               (uint32_t)(t_s6_end - t_s6), (uint32_t)(t_s6_end - t_s6) / STAGE_ITERATIONS, ((uint32_t)(t_s6_end - t_s6) / (STAGE_ITERATIONS*LLC_READ_LINES)));       

        print_interferer_counters();

        // Dump buffered CVA6 per-stage iterations
        static const char * const stage_names[] = {"LLC_RW", "DRAM_RW", "LLC2_RW", "LLC_R", "DRAM_R", "LLC2_R"};
        printf("\r\n iter | stage   |    LLC_RD |    LLC_WR |    MEM_RD |    MEM_WR |    cycles\r\n");
        printf("------+---------+-----------+-----------+-----------+-----------+-----------\r\n");
        for (uint32_t r = 0; r < TOTAL_STAGE_ITERS; r++) {
            uint32_t in_stage = r % (STAGE_WARMUP + STAGE_ITERATIONS);
            printf("%4u%c | %-7s | %9u | %9u | %9u | %9u | %9u\r\n",
                r, (in_stage < STAGE_WARMUP) ? '*' : ' ', stage_names[buf_stage[r]],
                buf_llcrd[r], buf_llcwr[r], buf_memrd[r], buf_memwr[r],
                buf_cyc[r]);
            uart_wait_tx_done();
        }
        printf("  (* = untimed warm-up pass, excluded from the summary above)\r\n");

        uart_wait_tx_done();

    // *******************************************************************
    // Cores 1-3: interference workload, throttled by their own token bucket
    // *******************************************************************
    } else if (
        mhartid == 1  
       || mhartid == 2 
       || mhartid == 3
        ) {
        // Core 0 Handshake
        while (g_go != GO_RUN) {
            asm volatile ("nop");
        }

        // Parked and ready to interfere
        g_core_ready[mhartid] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Wait for core 0 to signal interference start
        while (!g_interference_go) {
            asm volatile ("nop");
        }

        // Run interference until core 0 finishes
        // To obtain Fig.5 of research paper, interference cases should be swept.
        // Uncomment one of the functions, record data and move on to the next function.
        while (!g_core_done[0]) {
            //read_interference_cache(mhartid);
            //read_interference_mem(mhartid);

            write_interference_mem(mhartid);
            //write_interference_cache(mhartid);
        }

        g_core_done[mhartid] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Idle cleanly
        while(1) asm volatile ("wfi");

    }

    return 0;
}
