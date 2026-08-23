/*
 * libapmu -- see libapmu.h for the contract and the rationale.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "libapmu.h"

/* ------------------------------------------------------------------ */
/* error reporting                                                     */
/* ------------------------------------------------------------------ */

static char apmu_err[256];

const char *apmu_strerror(void)
{
    return apmu_err[0] ? apmu_err : "no error";
}

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(apmu_err, sizeof(apmu_err), fmt, ap);
    va_end(ap);
    return -1;
}

/* ------------------------------------------------------------------ */
/* mapping                                                             */
/* ------------------------------------------------------------------ */

int apmu_open(apmu_t *a, unsigned ncounters)
{
    return apmu_open_at(a, ncounters, 0);
}

int apmu_open_at(apmu_t *a, unsigned ncounters, uint64_t base)
{
    const char *env, *dev;
    int64_t delta;

    if (!a)
        return fail("apmu_open: NULL handle");

    memset(a, 0, sizeof(*a));
    a->fd = -1;

    if (ncounters == 0) {
        env = getenv("APMU_NCOUNTERS");
        ncounters = env ? (unsigned)strtoul(env, NULL, 0) : APMU_NCOUNTERS_DEFAULT;
    }
    if (ncounters == 0 || ncounters > APMU_NCOUNTERS_MAX)
        return fail("apmu_open: ncounters=%u out of range 1..%d",
                    ncounters, APMU_NCOUNTERS_MAX);
    a->ncounters = ncounters;

    if (base == 0) {
        env = getenv("APMU_BASE_ADDR");
        base = env ? (uint64_t)strtoull(env, NULL, 0) : APMU_BASE;
    }
    if (base & 0xfff)
        return fail("apmu_open: base 0x%llx is not 4 KiB aligned",
                    (unsigned long long)base);
    delta = (int64_t)base - (int64_t)APMU_BASE;
    a->base_delta = delta;

    /*
     * O_SYNC is what asks drivers/char/mem.c for an uncached mapping
     * (phys_mem_access_prot -> pgprot_noncached). Keep it even though CVA6's
     * hardwired PMAs already make this range non-cacheable: the flag is the
     * portable statement of intent and costs nothing.
     */
    /* APMU_DEVMEM exists so the library can be exercised against a plain file
     * off-target (see test_elf.c). On the board, leave it unset. */
    dev = getenv("APMU_DEVMEM");
    if (!dev)
        dev = "/dev/mem";

    a->fd = open(dev, O_RDWR | O_SYNC);
    if (a->fd < 0)
        return fail("open(%s): %s%s", dev, strerror(errno),
                    errno == EACCES ? " -- need root" :
                    errno == ENOENT
                      ? " -- devtmpfs normally creates this node at boot;"
                        " check that /dev is mounted and CONFIG_DEVMEM=y"
                      : "");

    {
        long     ps    = sysconf(_SC_PAGESIZE);
        uint64_t align = (ps > 0) ? (uint64_t)ps : 4096u;
        uint64_t win   = (uint64_t)((int64_t)APMU_WIN_BASE + a->base_delta);
        uint64_t start = win & ~(align - 1);
        size_t   pad   = (size_t)(win - start);
        size_t   len   = (size_t)((pad + APMU_WIN_SIZE + align - 1) & ~(align - 1));

        a->map_raw = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                          a->fd, (off_t)start);
        if (a->map_raw == MAP_FAILED) {
            int e = errno;
            close(a->fd);
            a->fd = -1;
            a->map_raw = NULL;
            return fail("mmap(%s, 0x%llx, 0x%zx): %s", dev,
                        (unsigned long long)start, len, strerror(e));
        }
        a->map_raw_size = len;
        /* ->map is where the CANONICAL APMU_WIN_BASE lands, so every address in
         * the rest of the library stays at its documented value regardless of
         * where the block actually sits. */
        a->map = (volatile uint8_t *)a->map_raw + pad;
    }

    apmu_err[0] = '\0';
    return 0;
}

void apmu_close(apmu_t *a)
{
    if (!a)
        return;
    if (a->map_raw && a->map_raw != MAP_FAILED)
        munmap(a->map_raw, a->map_raw_size);
    if (a->fd >= 0)
        close(a->fd);
    a->map_raw = NULL;
    a->map = NULL;
    a->fd = -1;
}

/* ------------------------------------------------------------------ */
/* raw MMIO                                                            */
/* ------------------------------------------------------------------ */

void apmu_fence(void)
{
#ifdef __riscv
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
#else
    /* Not the target, but keeps the library buildable (and unit-testable)
     * on a development host. */
    __sync_synchronize();
#endif
}

int apmu_in_window(uint64_t phys, size_t len)
{
    /* Order matters: the `len >` test below relies on APMU_WIN_END - phys not
     * having wrapped, so phys must be bounded on both sides first. */
    if (phys < APMU_WIN_BASE || phys >= APMU_WIN_END)
        return 0;
    if ((uint64_t)len > (uint64_t)APMU_WIN_END - phys)
        return 0;
    return 1;
}

/*
 * Out-of-window or misaligned accesses abort rather than return an error.
 * Every caller inside this library passes a compile-time-correct address, so
 * reaching here means a programming error, and the alternative -- letting a
 * stray 32-bit store land somewhere else in the peripheral window -- can wedge
 * the SoC. The bus ties off error reporting, so there is no hardware backstop.
 */
static volatile uint32_t *reg(const apmu_t *a, uint64_t phys)
{
    if (!a || !a->map) {
        fprintf(stderr, "libapmu: access through unopened handle\n");
        abort();
    }
    if (phys & 0x3u) {
        fprintf(stderr, "libapmu: unaligned MMIO address 0x%llx "
                        "(AXI4-Lite port is 32-bit only)\n",
                (unsigned long long)phys);
        abort();
    }
    if (!apmu_in_window(phys, 4)) {
        fprintf(stderr, "libapmu: address 0x%llx outside APMU window "
                        "0x%lx..0x%lx\n",
                (unsigned long long)phys,
                (unsigned long)APMU_WIN_BASE, (unsigned long)APMU_WIN_END);
        abort();
    }
    return (volatile uint32_t *)(a->map + (phys - APMU_WIN_BASE));
}

uint32_t apmu_rd32(const apmu_t *a, uint64_t phys)
{
    return *reg(a, phys);
}

void apmu_wr32(apmu_t *a, uint64_t phys, uint32_t val)
{
    *reg(a, phys) = val;
}

uint64_t apmu_rd64(const apmu_t *a, uint64_t phys)
{
    uint32_t hi, lo, hi2;
    int i;

    /* The timer runs while we read. Retry until the high word is stable. */
    for (i = 0; i < 8; i++) {
        hi  = apmu_rd32(a, phys + 4);
        lo  = apmu_rd32(a, phys);
        hi2 = apmu_rd32(a, phys + 4);
        if (hi == hi2)
            return ((uint64_t)hi << 32) | lo;
    }
    return ((uint64_t)hi2 << 32) | lo;
}

void apmu_wr64(apmu_t *a, uint64_t phys, uint64_t val)
{
    apmu_wr32(a, phys,     (uint32_t)(val & 0xffffffffu));
    apmu_wr32(a, phys + 4, (uint32_t)(val >> 32));
}

/* ------------------------------------------------------------------ */
/* PE control                                                          */
/* ------------------------------------------------------------------ */

void apmu_halt(apmu_t *a)
{
    apmu_wr32(a, APMU_STATUS, APMU_STATUS_STALL);
    apmu_fence();
}

void apmu_run(apmu_t *a)
{
    apmu_fence();
    apmu_wr32(a, APMU_STATUS, 0);
    apmu_fence();
}

int apmu_is_halted(const apmu_t *a)
{
    return (apmu_rd32(a, APMU_STATUS) & APMU_STATUS_STALL) != 0;
}

void apmu_set_boot_addr(apmu_t *a, uint32_t addr)
{
    apmu_wr32(a, APMU_BOOT_ADDR, addr);
    apmu_fence();
}

uint32_t apmu_get_boot_addr(const apmu_t *a)
{
    return apmu_rd32(a, APMU_BOOT_ADDR);
}

uint64_t apmu_get_timer(const apmu_t *a)
{
    return apmu_rd64(a, APMU_TIMER);
}

void apmu_set_period(apmu_t *a, uint64_t period)
{
    apmu_wr64(a, APMU_PERIOD, period);
    apmu_fence();
}

uint64_t apmu_get_period(const apmu_t *a)
{
    return apmu_rd64(a, APMU_PERIOD);
}

void apmu_set_timer_free_running(apmu_t *a)
{
    apmu_set_period(a, ~(uint64_t)0);
}

/* ------------------------------------------------------------------ */
/* counters                                                            */
/* ------------------------------------------------------------------ */

static int ck_idx(const apmu_t *a, unsigned i)
{
    if (i >= a->ncounters) {
        fprintf(stderr, "libapmu: counter index %u >= ncounters %u\n",
                i, a->ncounters);
        abort();
    }
    return 0;
}

#define CTR_ACCESSOR(name, addr_macro)                                     \
    uint32_t apmu_##name##_get(const apmu_t *a, unsigned i)                \
    { ck_idx(a, i); return apmu_rd32(a, addr_macro(i)); }                  \
    void apmu_##name##_set(apmu_t *a, unsigned i, uint32_t v)              \
    { ck_idx(a, i); apmu_wr32(a, addr_macro(i), v); }

CTR_ACCESSOR(counter,     APMU_COUNTER)
CTR_ACCESSOR(event_sel,   APMU_EVENT_SEL)
CTR_ACCESSOR(event_info,  APMU_EVENT_INFO)
CTR_ACCESSOR(init_budget, APMU_INIT_BUDGET)

void apmu_counters_reset(apmu_t *a)
{
    unsigned i;
    for (i = 0; i < a->ncounters; i++) {
        apmu_wr32(a, APMU_EVENT_SEL(i),   0);
        apmu_wr32(a, APMU_EVENT_INFO(i),  0);
        apmu_wr32(a, APMU_INIT_BUDGET(i), 0);
        apmu_wr32(a, APMU_COUNTER(i),     0);
    }
    apmu_fence();
}

int apmu_probe_ncounters(apmu_t *a)
{
    const uint32_t pat = 0x00a5005au;   /* fits the event-select field width */
    unsigned i;
    int found = 0;

    if (!apmu_is_halted(a))
        return fail("apmu_probe_ncounters: halt the PE first");

    for (i = 0; i < APMU_NCOUNTERS_MAX; i++) {
        uint64_t addr = APMU_EVENT_SEL(i);
        uint32_t save, back;

        save = apmu_rd32(a, addr);
        apmu_wr32(a, addr, pat);
        apmu_fence();
        back = apmu_rd32(a, addr);
        apmu_wr32(a, addr, save);
        apmu_fence();

        if (back != pat)
            break;
        found = (int)i + 1;
    }

    apmu_err[0] = '\0';
    return found;
}

/* ------------------------------------------------------------------ */
/* scratchpad access                                                   */
/* ------------------------------------------------------------------ */

int apmu_write_mem(apmu_t *a, uint64_t phys, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    size_t off;

    if (phys & 0x3u)
        return fail("apmu_write_mem: destination 0x%llx not 4-byte aligned",
                    (unsigned long long)phys);
    if (!apmu_in_window(phys, (len + 3) & ~(size_t)3))
        return fail("apmu_write_mem: [0x%llx, +0x%zx) outside APMU window",
                    (unsigned long long)phys, len);

    for (off = 0; off < len; off += 4) {
        size_t n = len - off;
        uint32_t w = 0;
        /* Little-endian assembly of the (possibly partial) word. Padding the
         * tail with zeros is safe: scratchpad, no side effects. */
        if (n > 4)
            n = 4;
        memcpy(&w, src + off, n);
        apmu_wr32(a, phys + off, w);
    }
    apmu_fence();
    apmu_err[0] = '\0';
    return 0;
}

int apmu_read_mem(const apmu_t *a, uint64_t phys, void *buf, size_t len)
{
    uint8_t *dst = buf;
    size_t off;

    if (phys & 0x3u)
        return fail("apmu_read_mem: source 0x%llx not 4-byte aligned",
                    (unsigned long long)phys);
    if (!apmu_in_window(phys, (len + 3) & ~(size_t)3))
        return fail("apmu_read_mem: [0x%llx, +0x%zx) outside APMU window",
                    (unsigned long long)phys, len);

    for (off = 0; off < len; off += 4) {
        size_t n = len - off;
        uint32_t w = apmu_rd32(a, phys + off);
        if (n > 4)
            n = 4;
        memcpy(dst + off, &w, n);
    }
    apmu_err[0] = '\0';
    return 0;
}

int apmu_zero_mem(apmu_t *a, uint64_t phys, size_t len)
{
    size_t off;

    if (phys & 0x3u)
        return fail("apmu_zero_mem: 0x%llx not 4-byte aligned",
                    (unsigned long long)phys);
    if (!apmu_in_window(phys, (len + 3) & ~(size_t)3))
        return fail("apmu_zero_mem: [0x%llx, +0x%zx) outside APMU window",
                    (unsigned long long)phys, len);

    for (off = 0; off < len; off += 4)
        apmu_wr32(a, phys + off, 0);
    apmu_fence();
    apmu_err[0] = '\0';
    return 0;
}

long apmu_write_mem_verify(apmu_t *a, uint64_t phys, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    size_t off;
    long bad = 0;

    if (apmu_write_mem(a, phys, buf, len) < 0)
        return -1;

    for (off = 0; off < len; off += 4) {
        size_t n = len - off;
        uint32_t want = 0, got;
        if (n > 4)
            n = 4;
        memcpy(&want, src + off, n);
        got = apmu_rd32(a, phys + off);
        if (got != want)
            bad++;
    }
    return bad;
}

/* ------------------------------------------------------------------ */
/* firmware loading                                                    */
/* ------------------------------------------------------------------ */

int apmu_load_raw(apmu_t *a,
                  const void *text, size_t text_len,
                  const void *data, size_t data_len,
                  uint64_t data_addr,
                  apmu_image_t *out)
{
    apmu_image_t img;
    long bad;

    memset(&img, 0, sizeof(img));

    /* apmu-os links its DSPM output section at ORIGIN = APMU_DSPM_BASE, so a
     * data .bin belongs at the base of DSPM unless the caller says otherwise. */
    if (data_addr == 0)
        data_addr = APMU_DSPM_BASE;

    if (text_len > APMU_ISPM_SIZE)
        return fail("text image is %zu B, ISPM is %lu B",
                    text_len, (unsigned long)APMU_ISPM_SIZE);
    if (data_len && (data_addr < APMU_DSPM_BASE ||
                     data_addr + data_len > APMU_DSPM_BASE + APMU_DSPM_SIZE))
        return fail("data image [0x%llx, +0x%zx) does not fit DSPM "
                    "[0x%lx, +0x%lx)",
                    (unsigned long long)data_addr, data_len,
                    (unsigned long)APMU_DSPM_BASE, (unsigned long)APMU_DSPM_SIZE);

    /* The PE must be parked before ISPM is touched -- it fetches from there. */
    apmu_halt(a);

    bad = apmu_write_mem_verify(a, APMU_ISPM_BASE, text, text_len);
    if (bad < 0)
        return -1;
    img.verify_errors = bad;
    img.text_addr = APMU_ISPM_BASE;
    img.text_size = text_len;

    if (data_len) {
        bad = apmu_write_mem_verify(a, data_addr, data, data_len);
        if (bad < 0)
            return -1;
        img.verify_errors += bad;
        img.data_addr = data_addr;
        img.data_size = data_len;
    }

    /*
     * Deliberately NOT written: PMC_BOOT_ADDR.
     *
     * This whole function is a copy of the sequence in
     * tests/pmu_test/pmu_test_func.c that is known to run on the hardware --
     * halt, write ISPM, write DSPM, release -- and that sequence never touches
     * the boot address. It relies on the reset value, which pmu_top.sv fixes at
     * ISPM_BASE_ADDR. `apmuctl bubble` passes on the board doing exactly this;
     * the earlier version of this loader also wrote the boot address and the PE
     * did not execute. Use `apmuctl boot ADDR` if you ever need to move it.
     */
    img.entry = (uint32_t)APMU_ISPM_BASE;

    if (out)
        *out = img;

    if (img.verify_errors)
        return fail("firmware readback mismatched in %ld word(s)",
                    img.verify_errors);

    apmu_err[0] = '\0';
    return 0;
}

/* Read a whole file into a malloc'd buffer. Returns NULL and sets the library
 * error string on failure; the caller frees. */
void *apmu_slurp(const char *path, size_t *lenp)
{
    FILE  *f;
    long   n;
    void  *buf;
    size_t got;

    f = fopen(path, "rb");
    if (!f) {
        fail("open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fail("stat %s: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fail("out of memory reading %s", path);
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        fail("short read on %s", path);
        free(buf);
        return NULL;
    }
    if (lenp)
        *lenp = got;
    return buf;
}

/*
 * Convenience wrapper: read a text image (and optionally a data image) from
 * disk and hand them to apmu_load_raw(). data_addr == 0 means the DSPM base.
 *
 * The images are raw objcopy output -- little-endian 32-bit words, no headers:
 *
 *   llvm-objcopy -O binary --only-section=.text  fw.elf text.bin
 *   llvm-objcopy -O binary --only-section=.data --only-section=.rodata \
 *                                                fw.elf data.bin
 */
int apmu_load_file(apmu_t *a, const char *text_path, const char *data_path,
                   uint64_t data_addr, apmu_image_t *out)
{
    void  *text = NULL, *data = NULL;
    size_t tlen = 0, dlen = 0;
    int    rc = -1;

    text = apmu_slurp(text_path, &tlen);
    if (!text)
        return -1;
    if (data_path) {
        data = apmu_slurp(data_path, &dlen);
        if (!data)
            goto out;
    }
    rc = apmu_load_raw(a, text, tlen, data, dlen, data_addr, out);
out:
    free(text);
    free(data);
    return rc;
}
