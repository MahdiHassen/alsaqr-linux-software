/*
 * Tests for the raw-.bin load path -- the one apmuctl actually uses.
 *
 * Runs against a file-backed "physical memory" via $APMU_DEVMEM, so it needs
 * no hardware. It replaces an earlier suite that exercised the ELF loader,
 * which is no longer built (see libapmu_elf.c.unused).
 *
 * The load sequence mirrors tests/pmu_test/pmu_test_func.c, which is proven on
 * the board. The most important assertion here is the negative one: loading
 * must NOT write PMC_BOOT_ADDR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libapmu.h"

static int fails;

static void ck(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static void wr(const char *p, const uint32_t *w, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(1); }
    fwrite(w, 4, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static const uint32_t text[] = { 0x33u, 0x33u, 0x104280b7u, 0x8093u, 0x2408113u };
    static const uint32_t data[] = { 0xdeadbeefu, 0x1234u, 0u, 0xffffffffu };
    const char *tp = "/tmp/apmu_t.bin", *dp = "/tmp/apmu_d.bin";
    apmu_image_t im;
    apmu_t a;
    unsigned i;

    if (argc > 1) setenv("APMU_DEVMEM", argv[1], 1);
    wr(tp, text, 5);
    wr(dp, data, 4);

    if (apmu_open(&a, 0) < 0) { fprintf(stderr, "open: %s\n", apmu_strerror()); return 1; }

    /* A boot address we can detect being clobbered. */
    apmu_set_boot_addr(&a, 0xcafe0000u);

    ck(apmu_load_file(&a, tp, NULL, 0, &im) == 0, "load text only succeeds");
    ck(im.text_addr == APMU_ISPM_BASE,            "text lands at ISPM base");
    ck(im.text_size == 20,                        "text size is 20 B");
    ck(im.verify_errors == 0,                     "text readback verifies");
    for (i = 0; i < 5; i++)
        ck(apmu_rd32(&a, APMU_ISPM_BASE + 4 * i) == text[i],
           "text word lands at ISPM_BASE + 4*N");

    ck(apmu_get_boot_addr(&a) == 0xcafe0000u,
       "load does NOT write PMC_BOOT_ADDR");

    ck(apmu_is_halted(&a) == 1,                   "load leaves the PE halted");

    ck(apmu_load_file(&a, tp, dp, 0, &im) == 0,   "load text + data succeeds");
    ck(im.data_addr == APMU_DSPM_BASE,            "data defaults to DSPM base");
    ck(im.data_size == 16,                        "data size is 16 B");
    for (i = 0; i < 4; i++)
        ck(apmu_rd32(&a, APMU_DSPM_BASE + 4 * i) == data[i],
           "data word lands at DSPM_BASE + 4*N");

    ck(apmu_load_file(&a, tp, dp, APMU_DSPM_BASE + 0x40, &im) == 0,
       "explicit data address succeeds");
    ck(im.data_addr == APMU_DSPM_BASE + 0x40,     "explicit data address honoured");
    ck(apmu_rd32(&a, APMU_DSPM_BASE + 0x40) == data[0], "data word at explicit address");

    ck(apmu_get_boot_addr(&a) == 0xcafe0000u,
       "boot address still untouched after data load");

    ck(apmu_load_file(&a, "/tmp/apmu_missing.bin", NULL, 0, &im) < 0,
       "missing text file is an error");
    ck(apmu_load_file(&a, tp, dp, APMU_DSPM_BASE + APMU_DSPM_SIZE, &im) < 0,
       "data past the end of DSPM is rejected");

    apmu_run(&a);
    ck(apmu_is_halted(&a) == 0,                   "run clears the stall bit");
    apmu_halt(&a);
    ck(apmu_is_halted(&a) == 1,                   "halt sets the stall bit");

    apmu_close(&a);
    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
