// SPDX-License-Identifier: GPL-2.0
/*
 * llcpart: LLC way partitioning (CBQRI capacity controller) measured from Linux with a pointer chaser.
 *
 * Two actors on two harts with different RCIDs (the Ssqosid srmcfg CSR, 0x181, written per hart by this module):
 *   victim     a random pointer chain through victim_kb of lines (default 256 KB = 4 lines per LLC set, fits in 4 ways),
 *              chased with interrupts off on victim_cpu under rcid_v; cycles per hop from rdcycle, LLC read misses from
 *              the SoC-control counters (0x1A106008);
 *   aggressor  a kthread on aggr_cpu under rcid_a streaming through aggr_kb (default 4 MB, 64 lines per set: always misses).
 *
 * Cases printed to the kernel log on insmod:
 *   capacity  N ways   the victim alone with its RCID limited to 16/8/4/2/1 ways: the misses follow the mask;
 *   shared              all ways for everyone, aggressor running: the victim's lines get evicted (lines lost after);
 *   partitioned         victim ways 0..ways_v-1, aggressor the rest: the victim keeps its lines.
 *   Each aggressor case measures the victim while the aggressor streams, then leaves the aggressor alone for aggr_ms
 *   with the victim idle (as a descheduled task would be) and probes how many victim lines survived.
 * RCID 0, which every other hart and the kernel between measurements run under, is kept out of the victim's ways in
 * the partitioned cases: otherwise its few allocations evict the idle victim's lines, which are the least recently used.
 * The LLC's cached address range (SoC control 0x1A106018/1C, 0x8000_0000-0x8080_0000 at reset: only the kernel image)
 * is extended to the top of RAM on load, so the buffers are LLC-cached, and LEFT THAT WAY: shrinking the range on a
 * running system wedges the SoC (lines of the dropped region stay in the LLC, dirty ones included, and the next
 * access to them bypasses it; reproduced even with every hart quiesced under stop_machine). Widening is safe: the new
 * region had no lines in the LLC. The LLC is the single path to DRAM for every master, so caching all of RAM is
 * coherent; the netboot loader sets the full range before Linux starts, in which case this is a no-op.
 *
 * Hop latency under load is dominated by the coherence unit, which serialises the two harts' transactions
 * (hardware/docs/qos.md), so the number that shows the partition working is "lines lost", not cycles per hop.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/cpumask.h>
#include <linux/perf_event.h>
#include <asm/csr.h>

#define SOC_CTRL_PA     0x1A106000UL
#define SC_CNT_EN       0x04
#define SC_RD_MISS      0x08
#define SC_RD_HIT       0x0C
#define SC_CACHE_START  0x18
#define SC_CACHE_END    0x1C
#define LLC_CFG_PA      0x10401000UL
#define LLC_FLUSH       0x08            /* one bit per way; the LLC clears them when done */
#define LLC_SET_ASSO    0x20
#define LLC_NUM_LINES   0x28
#define CC              0x800           /* CBQRI capacity controller, in the same 4 KB window */
#define CC_ALLOC        0x18            /* OP[4:0] AT[7:5] RCID[19:8]; +4: STATUS[6:0] */
#define CC_MASK         0x20
#define OP_CONFIG       1
#define CSR_SRMCFG      0x181
#define LINE            64
#define RAM_TOP         0x9F000000UL

static int victim_kb = 256;   module_param(victim_kb, int, 0444); MODULE_PARM_DESC(victim_kb, "victim working set (KB, power of two, <= 4096)");
static int aggr_kb = 4096;    module_param(aggr_kb, int, 0444);   MODULE_PARM_DESC(aggr_kb, "aggressor stream (KB, power of two, <= 4096)");
static int hops = 200000;     module_param(hops, int, 0444);      MODULE_PARM_DESC(hops, "chase length per measurement");
static int victim_cpu = 0;    module_param(victim_cpu, int, 0444);
static int aggr_cpu = 1;      module_param(aggr_cpu, int, 0444);
static int rcid_v = 1;        module_param(rcid_v, int, 0444);
static int rcid_a = 2;        module_param(rcid_a, int, 0444);
static int ways_v = 4;        module_param(ways_v, int, 0444);    MODULE_PARM_DESC(ways_v, "victim ways in the partitioned case (aggressor gets the rest)");
static int sweep = 0;         module_param(sweep, int, 0444);     MODULE_PARM_DESC(sweep, "1: latency vs working set (8 KB..4 MB) for 16/8/4/2/1 ways instead of the cases");
static int aggr_ms = 300;     module_param(aggr_ms, int, 0444);   MODULE_PARM_DESC(aggr_ms, "aggressor alone, victim idle, before the probe (ms)");

static void __iomem *sc, *llc;
static u64 *victim; static unsigned victim_order; static unsigned nlines;
static u64 *aggr;   static unsigned aggr_order;
static u32 saved_start, saved_end;
static struct task_struct *aggr_task;
static volatile int aggr_stop;
static u64 aggr_sink;

/* ---- registers */
static int cc_set(unsigned rcid, unsigned at, u32 mask)
{
	writel(mask, llc + CC + CC_MASK);
	writel(OP_CONFIG | (at << 5) | (rcid << 8), llc + CC + CC_ALLOC);
	return readl(llc + CC + CC_ALLOC + 4) & 0x7F;      /* STATUS: 1 = success */
}
static int set_mask(unsigned rcid, u32 mask)                /* data and code of that RCID */
{
	int s0 = cc_set(rcid, 0, mask), s1 = cc_set(rcid, 1, mask);
	if (s0 != 1 || s1 != 1)
		pr_err("llcpart: CONFIG_LIMIT rcid %u mask 0x%04x: status %d/%d\n", rcid, mask, s0, s1);
	return (s0 == 1 && s1 == 1) ? 0 : -EIO;
}
static int llc_flush(void)
{
	int i;
	writel(0xFFFF, llc + LLC_FLUSH);
	for (i = 0; i < 100000; i++) {
		if (readl(llc + LLC_FLUSH) == 0)
			return 0;
		udelay(10);
	}
	pr_err("llcpart: LLC flush did not finish\n");
	return -EIO;
}
static inline u32 rd_miss(void) { return readl(sc + SC_RD_MISS); }

/* on this hart: write srmcfg, read it back (WARL: the RCID field must hold), clear it */
static void tag_check(void *arg)
{
	unsigned rcid = *(unsigned *)arg;
	unsigned long flags, v;
	local_irq_save(flags);
	csr_write(CSR_SRMCFG, rcid);
	v = csr_read(CSR_SRMCFG);
	csr_write(CSR_SRMCFG, 0);
	local_irq_restore(flags);
	pr_info("llcpart: cpu %d: srmcfg written %u, reads back 0x%lx %s\n", smp_processor_id(), rcid, v, (v & 0xFFF) == rcid ? "ok" : "MISMATCH");
}

/* ---- the victim chain: a random cycle through n lines of buf (Sattolo), one 64-bit pointer per line */
static void build_chain_n(u64 *buf, unsigned n)
{
	u32 *perm = kvmalloc_array(n, sizeof(u32), GFP_KERNEL);
	unsigned i;
	for (i = 0; i < n; i++)
		perm[i] = i;
	for (i = n - 1; i > 0; i--) {
		u32 j = get_random_u32() % i, t = perm[i];
		perm[i] = perm[j]; perm[j] = t;
	}
	for (i = 0; i < n; i++)
		buf[(size_t)perm[i] * (LINE / 8)] = (u64)(uintptr_t)&buf[(size_t)perm[(i + 1) % n] * (LINE / 8)];
	kvfree(perm);
}
static void build_chain(void) { build_chain_n(victim, nlines); }

static u64 chase(unsigned n, u32 *miss);

/* latency vs working set: the victim alone, its RCID limited to w ways, RCID 0 kept out of them */
static void run_sweep(unsigned asso)
{
	static const unsigned kbs[] = { 8, 16, 32, 64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096 };
	unsigned w, k;
	u64 *buf = (u64 *)__get_free_pages(GFP_KERNEL, get_order(4096 * 1024));
	if (!buf) { pr_err("llcpart: sweep: no 4 MB buffer\n"); return; }
	for (w = asso; w >= 1; w /= 2) {
		set_mask(rcid_v, (1u << w) - 1);
		set_mask(0, w == asso ? 0xFFFF : 0xFFFF & ~((1u << w) - 1));
		for (k = 0; k < ARRAY_SIZE(kbs); k++) {
			unsigned n = kbs[k] * 1024 / LINE, h = hops;
			u32 m;
			u64 c;
			build_chain_n(buf, n);
			llc_flush();
			victim = buf; nlines = n;
			chase(n, &m); chase(n, &m);                  /* warm: L1 and LLC hold what fits */
			if (h < 2 * n) h = 2 * n;
			c = chase(h, &m);
			pr_info("llcpart: sweep ways=%u kb=%u cyc=%llu.%02llu miss=%u hops=%u\n", w, kbs[k], c / h, (c * 100 / h) % 100, m, h);
		}
	}
	set_mask(rcid_v, 0xFFFF); set_mask(0, 0xFFFF);
	free_pages((unsigned long)buf, get_order(4096 * 1024));
}

/* chase n hops with interrupts off on this CPU under rcid_v; returns cycles, *miss = LLC read misses in the window */
static u64 chase(unsigned n, u32 *miss)
{
	unsigned long flags;
	u64 *p = victim, t0, t1;
	u32 m0;
	unsigned i;
	local_irq_save(flags);
	csr_write(CSR_SRMCFG, rcid_v);
	m0 = rd_miss();
	t0 = csr_read(CSR_CYCLE);
	for (i = 0; i < n; i++)
		p = (u64 *)(uintptr_t)READ_ONCE(*p);
	t1 = csr_read(CSR_CYCLE);
	*miss = rd_miss() - m0;
	csr_write(CSR_SRMCFG, 0);
	local_irq_restore(flags);
	aggr_sink += (u64)(uintptr_t)p;                      /* keep the chain alive */
	return t1 - t0;
}

/* ---- the aggressor: stream through aggr_kb, every line a miss */
static int aggr_fn(void *arg)
{
	unsigned n = (aggr_kb * 1024) / LINE, i = 0;
	u64 s = 0;
	csr_write(CSR_SRMCFG, rcid_a);
	while (!READ_ONCE(aggr_stop) && !kthread_should_stop()) {
		s += READ_ONCE(aggr[(size_t)i * (LINE / 8)]);
		if (++i == n) {
			i = 0;
			cond_resched();
		}
	}
	csr_write(CSR_SRMCFG, 0);
	aggr_sink += s;
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}
static int aggr_start(void)
{
	aggr_stop = 0;
	aggr_task = kthread_create(aggr_fn, NULL, "llcpart_aggr");
	if (IS_ERR(aggr_task))
		return PTR_ERR(aggr_task);
	kthread_bind(aggr_task, aggr_cpu);
	wake_up_process(aggr_task);
	msleep(20);                                          /* let it reach its loop */
	return 0;
}
static void aggr_end(void)
{
	WRITE_ONCE(aggr_stop, 1);
	kthread_stop(aggr_task);
	aggr_task = NULL;
}

/* one case: flush, warm the chain into the LLC, measure a window (aggressor running or not), then probe what survived */
static void run_case(const char *name, bool with_aggr)
{
	u32 m, mw, mp;
	u64 c, cp;
	llc_flush();
	chase(nlines, &m); chase(nlines, &m);                /* warm: the chain is in the LLC */
	if (with_aggr && aggr_start())
		return;
	c = chase(hops, &mw);
	if (with_aggr) {
		msleep(aggr_ms);                                 /* the aggressor alone, the victim idle */
		aggr_end();
	}
	cp = chase(nlines, &mp);                             /* probe: one pass, misses = lines lost */
	pr_info("llcpart: %-28s %4llu.%02llu cyc/hop  %7u LLC rd misses in %u hops | after: %5u of %u lines lost (%llu.%02llu cyc/hop)\n",
		name, c / hops, (c * 100 / hops) % 100, mw, hops, mp, nlines, cp / nlines, (cp * 100 / nlines) % 100);
}

static struct perf_event *cyc_ev;	/* keeps the cycle counter running on victim_cpu, see llcpart_init */

static void rd_cycle_on(void *arg) { *(u64 *)arg = csr_read(CSR_CYCLE); }

static int __init llcpart_init(void)
{
	u32 asso, sets, old_start, old_end;
	int w, ret;
	u32 vmask, amask;
	struct perf_event_attr attr;
	u64 c0, c1;

	if (victim_kb < 64 || victim_kb > 4096 || aggr_kb < 64 || aggr_kb > 4096 || (victim_kb & (victim_kb - 1)) || (aggr_kb & (aggr_kb - 1)))
		return -EINVAL;
	if (ways_v < 1 || ways_v > 15 || rcid_v < 1 || rcid_v > 15 || rcid_a < 1 || rcid_a > 15 || rcid_v == rcid_a)
		return -EINVAL;
	if (!cpu_online(victim_cpu) || !cpu_online(aggr_cpu) || victim_cpu == aggr_cpu)
		return -EINVAL;
	/* With the SBI PMU driver (CONFIG_RISCV_PMU_SBI) the kernel stops every counter when a CPU comes up and OpenSBI
	 * sets mcountinhibit, which CVA6 honours since the HPM series (he-soc 3c3b0fc0): rdcycle stands still unless perf
	 * has a cycles event on that CPU. Keep one alive on victim_cpu for the module's lifetime. */
	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CPU_CYCLES; attr.size = sizeof(attr); attr.pinned = 1;
	cyc_ev = perf_event_create_kernel_counter(&attr, victim_cpu, NULL, NULL, NULL);
	if (IS_ERR(cyc_ev)) {
		pr_warn("llcpart: no perf cycles event on cpu %d (%ld)\n", victim_cpu, PTR_ERR(cyc_ev));
		cyc_ev = NULL;
	}
	smp_call_function_single(victim_cpu, rd_cycle_on, &c0, 1);
	udelay(50);
	smp_call_function_single(victim_cpu, rd_cycle_on, &c1, 1);
	if (c1 == c0) {
		pr_err("llcpart: rdcycle does not advance on cpu %d (counter inhibited by the SBI PMU); not loading\n", victim_cpu);
		ret = -ENODEV; goto out_unmap;
	}
	sc = ioremap(SOC_CTRL_PA, 0x1000);
	llc = ioremap(LLC_CFG_PA, 0x1000);
	if (!sc || !llc) { ret = -ENOMEM; goto out_unmap; }
	asso = readl(llc + LLC_SET_ASSO); sets = readl(llc + LLC_NUM_LINES);

	victim_order = get_order(victim_kb * 1024);
	aggr_order = get_order(aggr_kb * 1024);
	victim = (u64 *)__get_free_pages(GFP_KERNEL, victim_order);
	aggr = (u64 *)__get_free_pages(GFP_KERNEL, aggr_order);
	if (!victim || !aggr) { pr_err("llcpart: no contiguous memory (orders %u/%u)\n", victim_order, aggr_order); ret = -ENOMEM; goto out_free; }
	nlines = (victim_kb * 1024) / LINE;
	memset(aggr, 0, aggr_kb * 1024);
	build_chain();

	/* the LLC caches only the kernel image at reset: cover the buffers (all of RAM) while we run */
	old_start = readl(sc + SC_CACHE_START); old_end = readl(sc + SC_CACHE_END);
	saved_start = old_start; saved_end = old_end;
	if (old_end < RAM_TOP)
		writel(RAM_TOP, sc + SC_CACHE_END);                  /* widen only; never shrink (see above) */
	writel(1, sc + SC_CNT_EN);
	pr_info("llcpart: LLC %u ways x %u sets x %u B = %u KB; cached range 0x%08x-0x%08x%s\n",
		asso, sets, LINE, asso * sets * LINE / 1024, old_start, readl(sc + SC_CACHE_END),
		old_end < RAM_TOP ? " (widened from the kernel image only; stays)" : "");
	pr_info("llcpart: victim %d KB = %u lines (%u per set) on cpu %d rcid %d (pa 0x%llx); aggressor %d KB on cpu %d rcid %d (pa 0x%llx); %d hops\n",
		victim_kb, nlines, nlines / sets, victim_cpu, rcid_v, (u64)virt_to_phys(victim), aggr_kb, aggr_cpu, rcid_a, (u64)virt_to_phys(aggr), hops);

	set_cpus_allowed_ptr(current, cpumask_of(victim_cpu));
	set_mask(rcid_v, 0xFFFF); set_mask(rcid_a, 0xFFFF);
	{
		unsigned rv = rcid_v, ra = rcid_a;
		smp_call_function_single(victim_cpu, tag_check, &rv, 1);
		smp_call_function_single(aggr_cpu, tag_check, &ra, 1);
	}

	if (sweep) {
		u64 *v = victim; unsigned n = nlines;
		run_sweep(asso);
		victim = v; nlines = n;
		pr_info("llcpart: sweep done\n");
		return 0;
	}

	/* capacity: the victim alone, its RCID limited to 16/8/4/2/1 ways */
	for (w = asso; w >= 1; w /= 2) {
		char name[32];
		set_mask(rcid_v, (1u << w) - 1);
		set_mask(0, w == asso ? 0xFFFF : 0xFFFF & ~((1u << w) - 1));   /* the rest of the system keeps out */
		snprintf(name, sizeof(name), "alone, %2d ways", w);
		run_case(name, false);
	}
	set_mask(rcid_v, 0xFFFF); set_mask(0, 0xFFFF);

	/* isolation: the aggressor on the other hart, with and without disjoint ways */
	run_case("shared 16 ways, aggr on", true);
	vmask = (1u << ways_v) - 1; amask = 0xFFFF & ~vmask;
	set_mask(rcid_v, vmask); set_mask(rcid_a, amask); set_mask(0, amask);
	{
		char name[40];
		snprintf(name, sizeof(name), "partitioned %d+%d, aggr on", ways_v, asso - ways_v);
		run_case(name, true);
	}
	set_mask(rcid_v, 0xFFFF); set_mask(rcid_a, 0xFFFF); set_mask(0, 0xFFFF);
	pr_info("llcpart: done\n");
	return 0;

out_free:
	if (victim) free_pages((unsigned long)victim, victim_order);
	if (aggr) free_pages((unsigned long)aggr, aggr_order);
out_unmap:
	if (sc) iounmap(sc);
	if (llc) iounmap(llc);
	if (cyc_ev) perf_event_release_kernel(cyc_ev);
	return ret;
}

static void __exit llcpart_exit(void)
{
	set_mask(rcid_v, 0xFFFF); set_mask(rcid_a, 0xFFFF); set_mask(0, 0xFFFF);
	free_pages((unsigned long)victim, victim_order);
	free_pages((unsigned long)aggr, aggr_order);
	iounmap(sc); iounmap(llc);
	if (cyc_ev) perf_event_release_kernel(cyc_ev);
	pr_info("llcpart: unloaded; masks reset, the LLC cached range stays 0x%08x-0x%08x\n", saved_start, (u32)RAM_TOP);
}

module_init(llcpart_init);
module_exit(llcpart_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AlSaqr LLC way partitioning (CBQRI) test: pointer chaser with per-hart RCIDs");
