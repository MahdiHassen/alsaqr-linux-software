/*
 * apmu_regs.h -- APMU (AlSaqr Performance Monitoring Unit) register map.
 *
 * Single source of truth for the Linux-userspace side. Cross-checked against:
 *   - he-soc/hardware/ip_list/apmu/src/pmu_top.sv       (struct layout + remap)
 *   - he-soc/hardware/ip_list/apmu/README.md            (register table)
 *   - he-soc/hardware/host/axi_lite_subsystem.sv:207    (SoC decode window)
 *   - he-soc/hardware/host/host_domain.sv:731-734       (ISPM/DSPM sizes)
 *   - alsaqr-software/tests/pmu_test/pmu_defines.h      (baremetal header)
 *
 * WHY THE ADDRESSES LOOK ODD -- the 4 KiB page remap
 * -------------------------------------------------
 * pmu_top.sv:519-530 rewrites incoming AXI4-Lite addresses:
 *
 *     addr[3:0]   = addr[3:0]        (kept)
 *     addr[11:4]  = addr[19:12]      (page number becomes the register index)
 *
 * So the *internal* packed register struct (timer@0x00, period@0x08,
 * status@0x10, boot@0x14, then counter bundles at 0x20 + i*0x10) is exposed
 * externally with each 16-byte group on its own 4 KiB page. That is deliberate:
 * it lets an OS map exactly one counter into one process without exposing the
 * rest of the PMU. It is also why Status lives at base+0x1000 rather than
 * base+0x10.
 *
 * Deriving the external address of internal offset N:
 *     ext = APMU_BASE + ((N >> 4) << 12) + (N & 0xf)
 */

#ifndef APMU_REGS_H
#define APMU_REGS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Region bases                                                        */
/* ------------------------------------------------------------------ */

/* SoC AXI4-Lite decode window for the whole APMU IP is
 * 0x1040_5000 .. 0x1060_5000 (axi_lite_subsystem.sv:207). Everything below
 * lives inside it. */
#define APMU_BASE           0x10405000UL

/* PMU bundle: timer, period, status, boot address. */
#define APMU_TIMER          (APMU_BASE + 0x0000UL)  /* 64-bit, RO, two 32-bit reads  */
#define APMU_TIMER_LO       (APMU_BASE + 0x0000UL)
#define APMU_TIMER_HI       (APMU_BASE + 0x0004UL)
#define APMU_PERIOD         (APMU_BASE + 0x0008UL)  /* 64-bit MemGuard period        */
#define APMU_PERIOD_LO      (APMU_BASE + 0x0008UL)
#define APMU_PERIOD_HI      (APMU_BASE + 0x000cUL)
#define APMU_STATUS         (APMU_BASE + 0x1000UL)  /* bit0 = stall (1 = PE halted)  */
#define APMU_BOOT_ADDR      (APMU_BASE + 0x1004UL)  /* PE reset vector               */

#define APMU_STATUS_STALL   0x1u

/* Counter bundles: one 4 KiB page each, starting at the page after the two
 * PMU-bundle pages. */
#define APMU_COUNTER_BASE   (APMU_BASE + 0x2000UL)
#define APMU_COUNTER_STRIDE 0x1000UL
#define APMU_COUNTER(i)     (APMU_COUNTER_BASE + (uint64_t)(i) * APMU_COUNTER_STRIDE)
#define APMU_EVENT_SEL(i)   (APMU_COUNTER(i) + 0x4UL)
#define APMU_EVENT_INFO(i)  (APMU_COUNTER(i) + 0x8UL)
#define APMU_INIT_BUDGET(i) (APMU_COUNTER(i) + 0xcUL)

/* Counter value bits. pmu_top.sv counter_t = {pending, counter[30:0]};
 * the surrounding comment also reserves bit30 for overflow. */
#define APMU_CNT_PENDING    (1u << 31)
#define APMU_CNT_OVERFLOW   (1u << 30)
#define APMU_CNT_VALUE_MASK 0x3fffffffu

/* Hardware counter count. host_domain.sv:206 synthesises 32; the address map
 * is sized for exactly 32 (APMU_BASE + (32+2)*0x1000 == ISPM base, below).
 * The baremetal pmu_defines.h in this tree says 12, which is what an older
 * bitstream had. Bitstreams differ -- use `apmuctl probe` on the board rather
 * than trusting either number, and override with -c/APMU_NCOUNTERS. */
#define APMU_NCOUNTERS_MAX      32
#define APMU_NCOUNTERS_DEFAULT  32

/* Scratchpads. Sizes from host_domain.sv: ISPM_NUM_WORDS=2048, DSPM_NUM_WORDS=32768,
 * one word = 4 B. */
#define APMU_ISPM_BASE      0x10427000UL
#define APMU_ISPM_SIZE      0x2000UL      /*   8 KiB */
#define APMU_DSPM_BASE      0x10429000UL
#define APMU_DSPM_SIZE      0x20000UL     /* 128 KiB */

/* Old pmu_firmware convention (data image at DSPM+0x200). apmu-os uses the
 * layout in apmu_abi.h instead. */
#define APMU_DSPM_DATA_OFF  0x200UL

/* One contiguous mmap covers registers + ISPM + DSPM.
 * APMU_BASE + (32+2)*0x1000 == 0x10427000 == APMU_ISPM_BASE, so the register
 * window abuts ISPM with no gap. */
#define APMU_WIN_BASE       APMU_BASE
#define APMU_WIN_END        (APMU_DSPM_BASE + APMU_DSPM_SIZE)   /* 0x10449000 */
#define APMU_WIN_SIZE       (APMU_WIN_END - APMU_WIN_BASE)      /* 0x44000    */

/* ------------------------------------------------------------------ */
/* Event selection / event info encodings                              */
/* 9-port bitstreams; same values as apmu-os pmu_hw_desc.h.            */
/* ------------------------------------------------------------------ */

/* Event select: port, source, event; one value/mask nibble pair each.
 * Ports: 1-4 CVA6 EVU of core 0-3, 5-8 core 0-3 <-> LLC, 9 LLC <-> DRAM.
 * Events: 1 read req, 2 write req, 3 read resp, 4 write resp.
 * On port 9 the source is the initiating core (core 0 = source 0 verified). */

/* Core <-> LLC */
#define APMU_LLC_RD_REQ_CORE(n)  (0x5F001F + ((uint32_t)(n) << 20))
#define APMU_LLC_WR_REQ_CORE(n)  (0x5F002F + ((uint32_t)(n) << 20))
#define APMU_LLC_RD_RES_CORE(n)  (0x5F003F + ((uint32_t)(n) << 20))
#define APMU_LLC_WR_RES_CORE(n)  (0x5F004F + ((uint32_t)(n) << 20))

/* LLC <-> main memory */
#define APMU_MEM_RD_REQ          0x9F001F
#define APMU_MEM_WR_REQ          0x9F002F
#define APMU_MEM_RD_RES          0x9F003F
#define APMU_MEM_WR_RES          0x9F004F
#define APMU_MEM_RD_RES_CORE(n)  (0x9F0F3F + ((uint32_t)(n) << 12))
#define APMU_MEM_WR_RES_CORE(n)  (0x9F0F4F + ((uint32_t)(n) << 12))

/* Event info register */
#define APMU_ADD_RESP_LAT        0x8001E0
#define APMU_ADD_MEM_ONLY        0x808E10
#define APMU_OVERFLOW_EN         0x1000000

/* PLIC source numbers for the PMU's per-counter interrupt lines.
 * ariane_peripherals.sv:154: irq_sources[155 + i] = pmu_intr_i[i], and PLIC
 * source numbering is 1-based, so irq_sources[155] is source 156.
 * NOTE: nothing in the shipped device tree claims these. Linux masks them and
 * userspace cannot receive them. */
#define APMU_PLIC_IRQ_BASE       156
#define APMU_PLIC_IRQ(i)         (APMU_PLIC_IRQ_BASE + (i))

#endif /* APMU_REGS_H */
