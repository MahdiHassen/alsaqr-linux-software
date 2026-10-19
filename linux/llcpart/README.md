# llcpart: LLC way partitioning measured from Linux

A kernel module that checks the CBQRI capacity controller of the LLC (way partitioning, `hardware/docs/qos.md` in
he-soc) with a pointer chaser, using a different RCID per hart. Needs the bundle's Linux (6.1, `CONFIG_MODULES`) booted
on the VCU118 with the Ethernet link up (`root@10.42.0.2`).

```
make                 # llcpart.ko, built against cva6-sdk/buildroot/output/build/linux-v6.1.183
make run             # scp to the board, insmod, print the results (dmesg | grep llcpart)
make run PARAMS="victim_kb=512 ways_v=8 aggr_cpu=2"
make sweep           # latency vs working set for 16/8/4/2/1 ways -> llcpart_sweep.txt, llcpart_sweep.png
ssh root@10.42.0.2 rmmod llcpart      # resets the way masks
```

By hand on the board: `/root/insmodp /root/llcpart.ko sweep=1` (or any `param=value`), `rmmod llcpart`, `dmesg`.
`insmodp` is a 20-line static insmod shipped here because the bundle's busybox (modprobe-small) drops module
parameters; the next bundle build has the full insmod.

What it does, on insmod:

1. Extends the LLC's cached address range (SoC control `0x1A106018/1C`, only the kernel image at reset) to the top
   of RAM so its buffers are LLC-cached, and enables the LLC hit/miss counters. The range stays widened: shrinking
   it on a running system wedges the SoC (the dropped region's lines, dirty ones included, stay in the LLC while
   new accesses bypass it; reproduced even under `stop_machine`). Widening is safe, and the netboot loader now
   sets the full range before Linux, so on a fresh boot this step is a no-op.
2. Builds a random pointer cycle through `victim_kb` (default 256 KB = 4096 lines, 4 per LLC set) and chases it with
   interrupts off on `victim_cpu` under `rcid_v` (srmcfg CSR 0x181, written on that hart for the duration of each
   measurement). Cycles per hop come from `rdcycle`, LLC read misses from the SoC-control counter.
3. Capacity: the victim alone with its RCID limited to 16, 8, 4, 2 and 1 ways. 4096 lines fit in 4 ways; with 2 or
   1 the chase misses on every hop.
4. Isolation: a kthread on `aggr_cpu` under `rcid_a` streams through `aggr_kb` (default 4 MB, 64 lines per set).
   The victim is measured while the aggressor runs, then the aggressor runs alone for `aggr_ms` with the victim idle,
   and one more pass over the victim's lines counts how many were lost. Once with all ways shared, once with the
   victim in ways `0..ways_v-1` and the aggressor in the rest.

Read the "lines lost" column: shared, the aggressor evicts the idle victim's lines; partitioned, it cannot. Cycles
per hop under load say less, because the coherence unit serialises the two harts' transactions whatever the masks
(the bare-metal `t_part` in he-soc measured the same).

RCID 0, which every other hart and the kernel between measurements run under, is kept out of the victim's ways in
the restricted cases; otherwise its few allocations evict the idle victim's lines, which are the least recently used.

## Results on the VCU118 (2026-10-19, bitstream ethernet, Linux 6.1.183)

`make sweep` (llcpart_sweep.png): L1 hits at 4.1 cycles up to 16 KB, LLC hits at 48.5, DRAM at 74; the working
set at which the chase falls off the LLC follows the ways exactly:

| ways | 16 | 8 | 4 | 2 | 1 |
|---|---|---|---|---|---|
| last size with ~0 misses | 1024 KB | 512 KB | 256 KB | 128 KB | 64 KB |

`make run` (victim 256 KB on cpu 0 RCID 1, aggressor 4 MB on cpu 1 RCID 2, 300 ms alone):

| case | cyc/hop | lines lost of 4096 |
|---|---|---|
| alone, 16 / 8 / 4 ways | 59 | 171 / 131 / 42 |
| alone, 2 / 1 ways | 84 | 3845 / 4123 (thrash) |
| shared 16 ways, aggressor on | 97 | 4220 |
| partitioned 4 + 12, aggressor on | 97 | 320-520 |

Under load both cases cost ~97 cycles per hop: the coherence unit serialises the two harts (see he-soc
`hardware/docs/qos.md`). The partition shows in what survives.
