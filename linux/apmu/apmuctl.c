/*
 * apmuctl -- command-line control of the AlSaqr APMU from Linux userspace.
 *
 * A debugging tool over apmu_mmio (/dev/mem): load PE firmware into
 * ISPM/DSPM, start and stop the PE, program counters, and poke at the
 * scratchpads.
 *
 *   apmuctl info
 *   apmuctl load text.bin data.bin --run
 *   apmuctl counters
 *   apmuctl dspm 0 16
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "apmu_mmio.h"

static apmu_t A;
static unsigned opt_nc;      /* -c N   : counter count override          */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "apmuctl: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    apmu_mmio_close(&A);
    exit(1);
}

static uint64_t xnum(const char *s, const char *what)
{
    char *end;
    unsigned long long v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno || end == s || *end)
        die("bad %s: '%s' (decimal, or 0x-prefixed hex)", what, s);
    return (uint64_t)v;
}

static unsigned xidx(const char *s)
{
    uint64_t v = xnum(s, "counter index");
    if (v >= A.ncounters)
        die("counter index %llu out of range (0..%u); "
            "use -c N or APMU_NCOUNTERS if this bitstream has more",
            (unsigned long long)v, A.ncounters - 1);
    return (unsigned)v;
}

/* Accept either an absolute physical address inside the APMU window, or a
 * bare offset which is interpreted relative to `rel`. */
static uint64_t xaddr(const char *s, uint64_t rel, const char *what)
{
    uint64_t v = xnum(s, what);
    if (v < APMU_WIN_BASE)
        v += rel;
    if (!apmu_in_window(v, 4))
        die("address 0x%llx is outside the APMU window 0x%lx..0x%lx",
            (unsigned long long)v,
            (unsigned long)APMU_WIN_BASE, (unsigned long)APMU_WIN_END);
    if (v & 3)
        die("address 0x%llx is not 4-byte aligned (AXI4-Lite is 32-bit only)",
            (unsigned long long)v);
    return v;
}

static void *slurp(const char *path, size_t *lenp)
{
    struct stat st;
    uint8_t *buf;
    size_t got = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        die("open(%s): %s", path, strerror(errno));
    if (fstat(fd, &st) < 0)
        die("stat(%s): %s", path, strerror(errno));
    if (st.st_size == 0)
        die("%s is empty", path);

    buf = malloc((size_t)st.st_size);
    if (!buf)
        die("out of memory reading %s", path);
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            die("read(%s): %s", path, n < 0 ? strerror(errno) : "short read");
        got += (size_t)n;
    }
    close(fd);
    *lenp = got;
    return buf;
}

static void report_image(const apmu_image_t *im)
{
    printf("  .text  0x%08llx  %zu B\n",
           (unsigned long long)im->text_addr, im->text_size);
    if (im->data_size)
        printf("  .data  0x%08llx  %zu B\n",
               (unsigned long long)im->data_addr, im->data_size);
    if (im->bss_size)
        printf("  .bss   0x%08llx  %zu B (zeroed)\n",
               (unsigned long long)im->bss_addr, im->bss_size);
    printf("  entry  0x%08x\n", im->entry);
    printf("  readback: %s\n",
           im->verify_errors ? "MISMATCH" : "ok");
}

/* Start the PE the way every working firmware in the tree expects: free-running
 * timer first, then release the stall. */
/*
 * Release the stall and nothing else -- the same two register writes that
 * tests/pmu_test does, and that `example_bubble` proves on the board.
 *
 * An earlier version also set PERIOD to all-ones here, on the theory that
 * "most firmware assumes a free-running timer". That was wrong: the only
 * functions in tests/pmu_test that touch PERIOD are the two trace-collecting
 * case studies, and the bubble sort -- the one known to work -- never does.
 * Enabling MemGuard as a side effect of starting the PE is exactly the kind of
 * unproven extra write that kept the PE from executing before.
 */
static void start_pe(void)
{
    apmu_run(&A);
    printf("PE running\n");
}

/* ------------------------------------------------------------------ */

static void cmd_info(void)
{
    unsigned i;
    int halted = apmu_is_halted(&A);

    printf("APMU window   0x%08lx .. 0x%08lx  (%lu KiB, /dev/mem O_SYNC)\n",
           (unsigned long)APMU_WIN_BASE, (unsigned long)APMU_WIN_END,
           (unsigned long)APMU_WIN_SIZE / 1024);
    printf("  registers   0x%08lx  timer/period, status/boot, %u counter pages\n",
           (unsigned long)APMU_BASE, A.ncounters);
    printf("  ISPM        0x%08lx  %lu B\n",
           (unsigned long)APMU_ISPM_BASE, (unsigned long)APMU_ISPM_SIZE);
    printf("  DSPM        0x%08lx  %lu B (mailbox 0x0..0x%lx, image from 0x%lx)\n",
           (unsigned long)APMU_DSPM_BASE, (unsigned long)APMU_DSPM_SIZE,
           (unsigned long)APMU_DSPM_DATA_OFF, (unsigned long)APMU_DSPM_DATA_OFF);
    printf("\n");
    printf("PE state      %s\n", halted ? "HALTED (stall=1)" : "running (stall=0)");
    printf("boot addr     0x%08x\n", apmu_get_boot_addr(&A));
    /* The timer is not a general-purpose clock: pmu_top.sv gates it on
     * PERIOD != 0 and its only consumer is the MemGuard budget-replenish
     * pulse. Shown for reference; poke 0x10405008 to enable it. */
    printf("mg timer      0x%016llx%s\n",
           (unsigned long long)apmu_get_timer(&A),
           apmu_get_period(&A) == 0 ? "  (stopped: MemGuard period is 0)" : "");
    printf("mg period     0x%016llx\n", (unsigned long long)apmu_get_period(&A));
    printf("\nnon-zero counters:\n");
    for (i = 0; i < A.ncounters; i++) {
        uint32_t c = apmu_counter_get(&A, i);
        uint32_t s = apmu_event_sel_get(&A, i);
        if (c || s)
            printf("  [%2u] count=%-10u sel=0x%06x info=0x%08x budget=%u%s\n",
                   i, c & APMU_CNT_VALUE_MASK, s,
                   apmu_event_info_get(&A, i),
                   apmu_init_budget_get(&A, i),
                   (c & APMU_CNT_PENDING) ? " PENDING" : "");
    }
}

static void cmd_counters(void)
{
    unsigned i;
    printf("idx  %-12s %-10s %-10s %-10s flags\n",
           "counter", "event_sel", "event_info", "budget");
    for (i = 0; i < A.ncounters; i++) {
        uint32_t c = apmu_counter_get(&A, i);
        printf("%3u  %-12u 0x%08x 0x%08x %-10u %s%s\n", i,
               c & APMU_CNT_VALUE_MASK,
               apmu_event_sel_get(&A, i),
               apmu_event_info_get(&A, i),
               apmu_init_budget_get(&A, i),
               (c & APMU_CNT_PENDING)  ? "PENDING " : "",
               (c & APMU_CNT_OVERFLOW) ? "OVERFLOW" : "");
    }
}

static void dump_words(uint64_t base, uint64_t off, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i += 4) {
        unsigned j;
        printf("0x%08llx:", (unsigned long long)(base + off + (uint64_t)i * 4));
        for (j = 0; j < 4 && i + j < n; j++)
            printf(" %08x", apmu_rd32(&A, base + off + (uint64_t)(i + j) * 4));
        printf("\n");
    }
}

/* --------------------------------------------------------- example_bubble --
 * A direct port of test_pmu_core_bubble_sort() from
 * ../../tests/pmu_test/pmu_test_func.c, which is KNOWN to work on this
 * hardware from baremetal. Same program words, same write order, same strides,
 * and deliberately the same omissions -- it never writes PMC_BOOT_ADDR and
 * never sets PERIOD, relying on the reset value of boot_addr (= ISPM_BASE).
 *
 * Run it first on a board you are unsure of: if it passes, the /dev/mem path,
 * the loader and the register addresses are all good, and any later trouble is
 * in your own firmware rather than in apmuctl.
 *
 * Word 0 remains a NOP.  The fixed fetch interface executes it normally.
 */
#define BUBBLE_LEN 10

static uint32_t enc_lui(unsigned rd, uint32_t imm20)
{
    return (imm20 << 12) | (rd << 7) | 0x37u;
}

static uint32_t enc_addi(unsigned rd, unsigned rs1, int32_t imm12)
{
    return (((uint32_t)imm12 & 0xfffu) << 20) | (rs1 << 15) | (rd << 7) | 0x13u;
}

static void cmd_example_bubble(void)
{
    uint32_t prog[] = {
        0x33u,        /* 0: NOP */
        0x00000000u,  /* 1: lui  x1, (arr_base >> 12)   -- patched below */
        0x00000000u,  /* 2: addi x1, x1, (arr_base & 0xFFF) */
        0x00000000u,  /* 3: addi x2, x1, (LEN-1)*4 */
        0x8333u,   0x2b3u,    0x32203u,  0x432183u, 0x41f863u,
        0x332023u, 0x432223u, 0x100293u, 0x430313u, 0xfe2312e3u,
        0x28663u,  0xffc10113u, 0xfc1118e3u, 0x33u, 0xfe000ee3u,
    };
    const uint32_t arr = APMU_DSPM_BASE;
    uint32_t host[BUBBLE_LEN], dev[BUBBLE_LEN], prev[BUBBLE_LEN];
    unsigned i, j, n = sizeof(prog) / sizeof(prog[0]);
    int settled, pass;

    prog[1] = enc_lui(1, arr >> 12);
    prog[2] = enc_addi(1, 1, (int32_t)(arr & 0xfff));
    prog[3] = enc_addi(2, 1, (BUBBLE_LEN - 1) * 4);
    printf("  lui  x1, %u      -> %08x\n", arr >> 12, prog[1]);
    printf("  addi x1, x1, %u  -> %08x\n", arr & 0xfff, prog[2]);
    printf("  addi x2, x1, %u -> %08x\n", (BUBBLE_LEN - 1) * 4, prog[3]);

    apmu_halt(&A);

    for (i = 0; i < n; i++)
        apmu_wr32(&A, APMU_ISPM_BASE + 4 * i, prog[i]);
    for (i = 0; i < n; i++)
        if (apmu_rd32(&A, APMU_ISPM_BASE + 4 * i) != prog[i])
            die("ISPM readback mismatch at word %u", i);

    /* Values are arbitrary; they only have to be distinct and unsorted. */
    for (i = 0; i < BUBBLE_LEN; i++) {
        host[i] = 0x1000u + ((i * 7u) % BUBBLE_LEN) * 0x111u;
        apmu_wr32(&A, arr + 4 * i, host[i]);
    }
    for (i = 0; i < BUBBLE_LEN; i++)
        if (apmu_rd32(&A, arr + 4 * i) != host[i])
            die("DSPM readback mismatch at word %u", i);

    for (i = 0; i < BUBBLE_LEN; i++)          /* host-side golden sort */
        for (j = i + 1; j < BUBBLE_LEN; j++)
            if (host[j] < host[i]) {
                uint32_t t = host[i]; host[i] = host[j]; host[j] = t;
            }

    printf("  stall bit: after load=%d", apmu_is_halted(&A));
    apmu_run(&A);
    printf("  after run=%d\n", apmu_is_halted(&A));

    /* Poll until the array stops changing, exactly as the baremetal test does. */
    for (i = 0; i < BUBBLE_LEN; i++)
        prev[i] = ~0u;
    for (j = 0; j < 200000; j++) {
        settled = 1;
        for (i = 0; i < BUBBLE_LEN; i++) {
            dev[i] = apmu_rd32(&A, arr + 4 * i);
            if (dev[i] != prev[i])
                settled = 0;
            prev[i] = dev[i];
        }
        if (settled && j > 2)
            break;
    }

    pass = 1;
    printf("\n  %-12s %-12s %s\n", "device", "golden", "");
    for (i = 0; i < BUBBLE_LEN; i++) {
        int ok = dev[i] == host[i];
        if (!ok) pass = 0;
        printf("  %-12u %-12u %s\n", dev[i], host[i], ok ? "" : "  <-- MISMATCH");
    }
    apmu_halt(&A);

    printf("\n  %s\n", pass
        ? "PASS -- the PE ran a known-good program loaded from Linux. The\n"
          "  /dev/mem path, the loader and the register map are all correct."
        : "FAIL -- the PE did not sort the array, even though this exact program\n"
          "  works from baremetal. The difference is in driving the PMU from\n"
          "  Linux, not in the firmware.");
}

static void usage(FILE *f)
{
    fprintf(f,
"apmuctl -- control the AlSaqr APMU from Linux userspace (needs root)\n"
"\n"
"usage: apmuctl [-c N] <command> [args]\n"
"\n"
"  info                    address map, PE state, non-zero counters\n"
"  halt | run | reset      stall / release the PE. `run` restarts it at the\n"
"                          boot address -- it is a boot, not a resume\n"
"  load TEXT.bin [DATA.bin [DATA_ADDR]]\n"
"                          halt, write ISPM and DSPM, verify both readbacks.\n"
"                          DATA_ADDR defaults to the DSPM base\n"
"  example_bubble          load and run a known-good program -- proves the path\n"
"\n"
"  counters                table of every counter bundle\n"
"  counter I [VAL]         read, or set, counter I\n"
"  sel I [VAL]             ... its event-select register\n"
"  evinfo I [VAL]          ... its event-info register\n"
"  budget I [VAL]          ... its initial budget\n"
"  clear                   zero every counter bundle\n"
"\n"
"  peek ADDR [N]           read N words (default 1)\n"
"  poke ADDR VAL           write one word\n"
"  ispm OFF [N]            dump N words of ISPM at byte offset OFF\n"
"  dspm OFF [N]            dump N words of DSPM at byte offset OFF\n"
"  watch ADDR [US]         print a word whenever it changes (^C to stop)\n"
"\n"
"  -c N                    counter bundles this bitstream has (default %d,\n"
"                          or $APMU_NCOUNTERS). Must precede the command\n"
"\n"
"typical session\n"
"  apmuctl example_bubble\n"
"  apmuctl load text_section.bin data_rodata_bss.bin\n"
"  apmuctl ispm 0 8\n"
"  apmuctl run\n"
"  apmuctl dspm 0x1800 4\n"
"  apmuctl halt\n"
"\n"
"All accesses are 32-bit. ADDR is absolute or a bare offset into the region\n"
"named; OFF is a byte offset, N is a word count.\n",
        APMU_NCOUNTERS_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *cmd;
    int i;

    /* global flags, in any position before the command */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            opt_nc = (unsigned)strtoul(argv[i + 1], NULL, 0);
            argv[i] = NULL;     /* the flag */
            i++;                /* and its value, nulled by the tail below */
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            break;
        }
        argv[i] = NULL;
    }

    /* compact argv */
    {
        int w = 1;
        for (i = 1; i < argc; i++)
            if (argv[i])
                argv[w++] = argv[i];
        argc = w;
    }

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];

    if (apmu_mmio_open(&A, opt_nc) < 0) {
        fprintf(stderr, "apmuctl: %s\n", apmu_strerror());
        return 1;
    }

    if (!strcmp(cmd, "info")) {
        cmd_info();

    } else if (!strcmp(cmd, "example_bubble")) {
        cmd_example_bubble();

    } else if (!strcmp(cmd, "halt") || !strcmp(cmd, "stop")) {
        apmu_halt(&A);
        printf("PE halted\n");

    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "start")) {
        start_pe();

    } else if (!strcmp(cmd, "reset")) {
        apmu_halt(&A);
        printf("PE halted; next `run` restarts at 0x%08x\n", apmu_get_boot_addr(&A));

    } else if (!strcmp(cmd, "load")) {
        apmu_image_t im;
        void *text, *data = NULL;
        size_t tlen, dlen = 0;
        uint64_t daddr = 0;

        if (argc < 3) die("load needs at least TEXT.bin");
        text = slurp(argv[2], &tlen);
        if (argc >= 4)
            data = slurp(argv[3], &dlen);
        if (argc >= 5)
            daddr = xaddr(argv[4], APMU_DSPM_BASE, "data address");

        if (apmu_load_raw(&A, text, tlen, data, dlen, daddr, &im) < 0)
            die("%s", apmu_strerror());
        free(text);
        free(data);
        printf("loaded %s%s%s\n", argv[2], data ? " + " : "", data ? argv[3] : "");
        report_image(&im);

    } else if (!strcmp(cmd, "counters")) {
        cmd_counters();

    } else if (!strcmp(cmd, "clear")) {
        apmu_counters_reset(&A);
        printf("%u counter bundle(s) zeroed\n", A.ncounters);

    } else if (!strcmp(cmd, "counter") || !strcmp(cmd, "sel") ||
               !strcmp(cmd, "evinfo")  || !strcmp(cmd, "budget")) {
        unsigned idx;
        uint64_t addr;

        if (argc < 3) die("%s needs a counter index", cmd);
        idx = xidx(argv[2]);
        addr = !strcmp(cmd, "counter") ? APMU_COUNTER(idx)    :
               !strcmp(cmd, "sel")     ? APMU_EVENT_SEL(idx)  :
               !strcmp(cmd, "evinfo")  ? APMU_EVENT_INFO(idx) :
                                         APMU_INIT_BUDGET(idx);
        if (argc >= 4) {
            apmu_wr32(&A, addr, (uint32_t)xnum(argv[3], "value"));
            apmu_fence();
        }
        printf("0x%08x\n", apmu_rd32(&A, addr));

    } else if (!strcmp(cmd, "peek")) {
        uint64_t addr;
        unsigned n = 1;
        if (argc < 3) die("peek needs an address");
        addr = xaddr(argv[2], APMU_BASE, "address");
        if (argc >= 4) n = (unsigned)xnum(argv[3], "count");
        dump_words(addr, 0, n);

    } else if (!strcmp(cmd, "poke")) {
        uint64_t addr;
        if (argc < 4) die("poke needs an address and a value");
        addr = xaddr(argv[2], APMU_BASE, "address");
        apmu_wr32(&A, addr, (uint32_t)xnum(argv[3], "value"));
        apmu_fence();
        printf("0x%08llx <- 0x%08x\n", (unsigned long long)addr,
               apmu_rd32(&A, addr));

    } else if (!strcmp(cmd, "dspm") || !strcmp(cmd, "ispm")) {
        uint64_t base = !strcmp(cmd, "dspm") ? APMU_DSPM_BASE : APMU_ISPM_BASE;
        uint64_t size = !strcmp(cmd, "dspm") ? APMU_DSPM_SIZE : APMU_ISPM_SIZE;
        uint64_t addr = argc >= 3 ? xaddr(argv[2], base, "offset") : base;
        unsigned n    = argc >= 4 ? (unsigned)xnum(argv[3], "count") : 16;

        if (addr < base || addr >= base + size)
            die("0x%llx is not inside %s", (unsigned long long)addr, cmd);
        if ((uint64_t)n * 4 > base + size - addr)
            n = (unsigned)((base + size - addr) / 4);
        dump_words(addr, 0, n);

    } else if (!strcmp(cmd, "watch")) {
        uint64_t addr;
        unsigned us = 1000;
        uint32_t prev, cur;

        if (argc < 3) die("watch needs an address");
        addr = xaddr(argv[2], APMU_DSPM_BASE, "address");
        if (argc >= 4) us = (unsigned)xnum(argv[3], "poll interval (us)");

        prev = apmu_rd32(&A, addr);
        printf("0x%08llx = 0x%08x\n", (unsigned long long)addr, prev);
        for (;;) {
            usleep(us);
            cur = apmu_rd32(&A, addr);
            if (cur != prev) {
                printf("0x%08llx = 0x%08x\n", (unsigned long long)addr, cur);
                prev = cur;
            }
        }

    } else {
        fprintf(stderr, "apmuctl: unknown command '%s'\n\n", cmd);
        usage(stderr);
        apmu_mmio_close(&A);
        return 2;
    }

    apmu_mmio_close(&A);
    return 0;
}
