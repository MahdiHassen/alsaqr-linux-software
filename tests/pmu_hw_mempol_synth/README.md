# Hardware Token Bucket Regulation
This directory holds **Case Study 1** of the research paper, modified to use  hardware token buckets. Original software mempol is in `pmu_mempol_synth` directory. 

`alsaqr_adil_token_bucket.bit` bitstream is generated from adil_ece699 branch in he-soc GitHub repo, from "Token Bucket v1.0" (`b2d09e2`) commit.

# Commands

## Token Bucket Version
```
cd alsaqr-software
source source.sh
make load_bitstream BITSTREAM=bitstreams/alsaqr_adil_token_bucket.bit HW_TARGET=xilinx_tcf/Digilent/210308B3AC77 
./scripts/build.sh tests/pmu_hw_mempol_synth pmu_bench 
./scripts/load.sh tests/pmu_hw_mempol_synth/pmu_bench.riscv
```

## Original Mempol Version (For Reference)
```
cd alsaqr-software
source source.sh
make load_bitstream BITSTREAM=bitstreams/alsaqr_opendram2.bit HW_TARGET=xilinx_tcf/Digilent/210308B3AC77 
./scripts/build.sh tests/pmu_mempol_synth pmu_bench 
./scripts/load.sh tests/pmu_mempol_synth/pmu_bench.riscv --time 60000
```
--time 60000 is required to get 15 sec CSV dump at the end. If it still doesn't print anything, type `monitor resume` into gdb and wait.

# Code Breakdown

## Files
| File | Role |
|---|---|
| `pmu_bench.c` | The benchmark. Runs on CVA6. |
| `pmu_defines.h` | APMU counter map and event encodings. |
| `tb_defines.h` | Token bucket register map. |
| `pmu_firmware/` | No-op Ibex firmware, embedded into the CVA6 binary. |
## Flow
All four cores enter `main()` and branch on `mhartid`.

1. Core 0 loads the Ibex firmware and waits for its `STATUS_STARTED` handshake. The firmware does nothing else. Regulation is entirely in hardware.
2. Core 0 releases cores 1-3. They park and wait.
3. Core 0 opens the LLC window `0x8000_0000`-`0x8300_0000` and partitions it 4 ways per core (256 KB each).
4. Core 0 programs all four token buckets, but leaves them disabled.
5. Core 0 configures APMU counters 4-19: LLC_RD, LLC_WR, MEM_RD, MEM_WR, one per core.
6. Core 0 primes its own partition, then signals cores 1-3 to start interfering.
7. Buckets are armed. Core 0 runs six stages. Cores 1-3 loop until core 0 is done.
8. Buckets are disabled. Results are printed over UART.


## Stages
Core 0 is the CUA. Each stage runs 1 untimed warm-up pass plus 10 timed passes.

| # | Tag | Function | Working set | Access |
|---|---|---|---|---|
| 1 | `LLC_RW` | `llc_stage_work` | 128 KB | stores |
| 2 | `DRAM_RW` | `dram_stage_work` | 512 KB | stores |
| 3 | `LLC2_RW` | `llc_stage_work` | 128 KB | stores |
| 4 | `LLC_R` | `llc_stage_work_read` | 128 KB | loads |
| 5 | `DRAM_R` | `dram_stage_work_read` | 512 KB | loads |
| 6 | `LLC2_R` | `llc_stage_work_read` | 128 KB | loads |

128 KB fits the partition, so those stages stay LLC-resident. 512 KB is twice the partition, so those stages miss to DRAM. 
## Token bucket
One bucket per core, all programmed the same. The parameter translation from software mempol is in the comment block at the top of `pmu_bench.c`. In short: `AR_COST = aLLCr + aMEMr = 16120` because the bucket sits on the core port and cannot tell an LLC hit from a DRAM miss, so every read is charged its worst case.

# Reproducing Results

## Running a sweep
Interference is selected by comment, in the cores 1-3 loop at the bottom of `pmu_bench.c`. Uncomment exactly one line:

```c
while (!g_core_done[0]) {
    //read_interference_cache(mhartid);   // LLC-resident loads
    //read_interference_mem(mhartid);     // DRAM loads
    write_interference_mem(mhartid);      // DRAM stores
    //write_interference_cache(mhartid);  // LLC-resident stores
}
```

That is four cases. To get isolation case, comment out all the lines. Rebuild and reload for each one:

```
./scripts/build.sh tests/pmu_hw_mempol_synth pmu_bench
./scripts/load.sh tests/pmu_hw_mempol_synth/pmu_bench.riscv
```

For an unregulated baseline, comment out the `tb_enable_all()`  before stage 1 and run the same 5 cases again.

## Collecting data
Capture the UART output to a file. The number to record per stage is `Latency: N cc/instr`, from the stage timing summary:

```
  Stage 1 (LLC):  4901131 cycles total, 490113 avg cc/iter, Latency: 239 cc/instr 
  Stage 2 (DRAM): 19560561 cycles total, 1956056 avg cc/iter, Latency: 238 cc/instr 
```

Latency is cycles per cache-line access, already averaged over the 10 timed passes.
Note that it is an integer division, so it is truncated and carries no fractional precision.
## Reference graphs
`results/` holds the graphs to compare against:

| File | Explanation |
|---|---|
| `fig5_orig.jpg` | Fig. 5 from the paper. |
| `token_bucket.png` / `.pdf` | This test, hardware token bucket. |
| `mempol_enabled.png` / `.pdf` | Software mempol, regulation on. |
| `mempol_disabled.png` / `.pdf` | Software mempol, regulation off. |

# (Token Bucket Version) Expected UART Output with MEM Read/Write Interference (write_interference_mem) uncommented. 
```
========================================
Hardware Token Bucket LLC/DRAM Benchmark
========================================
[1] Loading PMU firmware...
    Firmware loaded (text=956, data=4 bytes)
[2] Waiting for PMU to start...
    PMU started!
[2] Configuring PMU counters...
    Released cores 1-3
    Waiting for cores 1-3 to park...
    All cores ready!
    LLC enabled + partitioned (4 ways/core, 16-way)
    Token buckets: limit=344000 refill=86/1cyc aw=4400 ar=16120 (cores 0-3)
    Counters 4-7: LLC_RD, 8-11: LLC_WR, 12-15: MEM_RD, 16-19: MEM_WR per core
[3] Priming caches and starting interference...
    Setup complete, token buckets armed below for the whole run
[4a] Waiting for cores 1-3 to signal done...
    Core 1 done
    Core 2 done
    Core 3 done

[4b] All cores finished!

--- Stage timing summary ---

CUA Read/Write 
  Stage 1 (LLC):  4901131 cycles total, 490113 avg cc/iter, Latency: 239 cc/instr 
  Stage 2 (DRAM): 19560561 cycles total, 1956056 avg cc/iter, Latency: 238 cc/instr 
  Stage 3 (LLC):  4899340 cycles total, 489934 avg cc/iter, Latency: 239 cc/instr 

CUA Read:
  Stage 4 (LLC):  3853032 cycles total, 385303 avg cc/iter, Latency: 188 cc/instr 
  Stage 5 (DRAM): 15371852 cycles total, 1537185 avg cc/iter, Latency: 187 cc/instr 
  Stage 6 (LLC):  3851978 cycles total, 385197 avg cc/iter, Latency: 188 cc/instr 

========================================
Interferer totals, whole run (cores 1-3)
========================================
Core 1: LLC_RD=253963, LLC_WR=253697, MEM_RD=253954, MEM_WR=249867, total=1011481
Core 2: LLC_RD=253963, LLC_WR=253698, MEM_RD=253952, MEM_WR=250580, total=1012193
Core 3: LLC_RD=253964, LLC_WR=253697, MEM_RD=253952, MEM_WR=249856, total=1011469
========================================

 iter | stage   |    LLC_RD |    LLC_WR |    MEM_RD |    MEM_WR |    cycles
------+---------+-----------+-----------+-----------+-----------+-----------
   0* | LLC_RW  |      2055 |      1796 |         9 |         2 |    471051
   1  | LLC_RW  |      2048 |      2048 |         0 |         0 |    486782
   2  | LLC_RW  |      2048 |      2048 |         0 |         0 |    486862
   3  | LLC_RW  |      2048 |      2048 |         0 |         0 |    486781
   4  | LLC_RW  |      2048 |      2048 |         0 |         0 |    486781
   5  | LLC_RW  |      2048 |      2048 |         0 |         0 |    486840
   6  | LLC_RW  |      2048 |      2048 |         1 |         0 |    486782
   7  | LLC_RW  |      2047 |      2047 |         0 |         0 |    486550
   8  | LLC_RW  |      2047 |      2048 |         1 |         0 |    486549
   9  | LLC_RW  |      2047 |      2048 |         0 |         0 |    486644
  10  | LLC_RW  |      2047 |      2048 |         0 |         0 |    486640
  11* | DRAM_RW |      8198 |      8193 |      4117 |      4105 |   1953202
  12  | DRAM_RW |      8192 |      8192 |      8193 |      8192 |   1952832
  13  | DRAM_RW |      8192 |      8192 |      8192 |      8187 |   1952862
  14  | DRAM_RW |      8192 |      8192 |      8193 |      8193 |   1952851
  15  | DRAM_RW |      8192 |      8192 |      8192 |      8190 |   1952780
  16  | DRAM_RW |      8192 |      8192 |      8193 |      8194 |   1952820
  17  | DRAM_RW |      8192 |      8192 |      8192 |      8190 |   1952815
  18  | DRAM_RW |      8192 |      8192 |      8193 |      8193 |   1952817
  19  | DRAM_RW |      8192 |      8192 |      8192 |      8191 |   1952873
  20  | DRAM_RW |      8192 |      8192 |      8193 |      8194 |   1952880
  21  | DRAM_RW |      8192 |      8192 |      8192 |      8191 |   1952804
  22* | LLC2_RW |      2053 |      2048 |      2050 |      2052 |    487369
  23  | LLC2_RW |      2047 |      2047 |         0 |         0 |    486609
  24  | LLC2_RW |      2047 |      2048 |         1 |         1 |    486607
  25  | LLC2_RW |      2047 |      2048 |         0 |         0 |    486594
  26  | LLC2_RW |      2047 |      2048 |         0 |         0 |    486590
  27  | LLC2_RW |      2048 |      2049 |         0 |         0 |    486862
  28  | LLC2_RW |      2048 |      2048 |         0 |         0 |    486862
  29  | LLC2_RW |      2048 |      2048 |         0 |         0 |    486782
  30  | LLC2_RW |      2048 |      2048 |         0 |         0 |    486782
  31  | LLC2_RW |      2048 |      2048 |         0 |         0 |    486883
  32  | LLC2_RW |      2048 |      2048 |         0 |         0 |    486882
  33* | LLC_R   |      2053 |         1 |        15 |        10 |    381079
  34  | LLC_R   |      2047 |         2 |         1 |         0 |    383962
  35  | LLC_R   |      2047 |         2 |         0 |         0 |    383877
  36  | LLC_R   |      2047 |         2 |         1 |         0 |    383879
  37  | LLC_R   |      2047 |         2 |         0 |         0 |    383874
  38  | LLC_R   |      2047 |         2 |         1 |         0 |    383858
  39  | LLC_R   |      2047 |         2 |         0 |         0 |    383892
  40  | LLC_R   |      2047 |         2 |         1 |         0 |    383875
  41  | LLC_R   |      2045 |         3 |         0 |         0 |    383538
  42  | LLC_R   |      2047 |         3 |         0 |         0 |    383956
  43  | LLC_R   |      2047 |         2 |         0 |         0 |    383926
  44* | DRAM_R  |      8196 |         2 |      4121 |      2042 |   1634791
  45  | DRAM_R  |      8192 |         2 |      8192 |         2 |   1535761
  46  | DRAM_R  |      8192 |         1 |      8193 |         1 |   1535674
  47  | DRAM_R  |      8192 |         1 |      8192 |         1 |   1535681
  48  | DRAM_R  |      8192 |         1 |      8193 |         1 |   1535667
  49  | DRAM_R  |      8192 |         2 |      8192 |         2 |   1535712
  50  | DRAM_R  |      8192 |         2 |      8193 |         2 |   1535757
  51  | DRAM_R  |      8192 |         3 |      8192 |         3 |   1535830
  52  | DRAM_R  |      8192 |         3 |      8193 |         3 |   1535811
  53  | DRAM_R  |      8192 |         3 |      8192 |         3 |   1535811
  54  | DRAM_R  |      8192 |         3 |      8193 |         3 |   1535787
  55* | LLC2_R  |      2052 |         3 |      2050 |         0 |    384543
  56  | LLC2_R  |      2048 |         3 |         1 |         0 |    384175
  57  | LLC2_R  |      2047 |         2 |         0 |         0 |    383885
  58  | LLC2_R  |      2047 |         2 |         0 |         0 |    383868
  59  | LLC2_R  |      2048 |         1 |         0 |         0 |    383992
  60  | LLC2_R  |      2048 |         1 |         0 |         0 |    384009
  61  | LLC2_R  |      2048 |         0 |         0 |         0 |    383942
  62  | LLC2_R  |      2048 |         0 |         0 |         0 |    383956
  63  | LLC2_R  |      2048 |         0 |         0 |         0 |    384004
  64  | LLC2_R  |      2048 |         0 |         0 |         0 |    384044
  65  | LLC2_R  |      2048 |         1 |         0 |         0 |    383993
  (* = untimed warm-up pass, excluded from the summary above)

```
