/*
 * libapmu -- userspace control of the AlSaqr APMU from Linux.
 *
 * Everything the baremetal tests do to the PMU over MMIO, done from a normal
 * Linux process through /dev/mem. See README.md for the why; the short version:
 *
 *   - The APMU sits at 0x10405000..0x10448000, inside the SoC's AXI4-Lite
 *     peripheral window. That is not RAM, so Linux never maps it and there is
 *     no driver to go through.
 *   - /dev/mem + mmap(O_SYNC) hands the process an uncached mapping of it.
 *     CVA6's PMAs already make the whole sub-0x80000000 range non-cacheable and
 *     non-idempotent in hardware, so the mapping attributes are belt-and-braces.
 *   - The config port is AXI4-Lite, 32 bits wide. EVERY access must be a single
 *     naturally aligned 32-bit load or store. Do not memcpy() into ISPM/DSPM
 *     from here; use apmu_write_mem(), which decomposes into word stores.
 *
 * All addresses in this API are PHYSICAL addresses (use the APMU_* constants
 * from apmu_regs.h). The library translates into its mapping and bounds-checks.
 *
 * Not thread-safe and not multi-process-safe: there is exactly one APMU and no
 * arbitration. Run one controller at a time.
 */

#ifndef LIBAPMU_H
#define LIBAPMU_H

#include <stddef.h>
#include <stdint.h>

#include "apmu_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int                fd;         /* /dev/mem                                 */
    volatile uint8_t  *map;        /* APMU_WIN_BASE maps here                  */
    unsigned           ncounters;  /* counters this build will talk to         */
    /* mmap() needs a page-aligned offset. On the target the page size is 4 KiB
     * and APMU_WIN_BASE already qualifies, but a development host may use
     * 16 KiB pages, so the raw mapping can start below APMU_WIN_BASE. Keep it
     * for munmap(); use ->map for everything else. */
    void              *map_raw;
    size_t             map_raw_size;
    /* Physical offset applied to the whole map; 0 for the current revision.
     * See apmu_open_at(). */
    int64_t            base_delta;
} apmu_t;

/* Human-readable reason for the last failure. Valid until the next call. */
const char *apmu_strerror(void);

/*
 * Map the APMU. ncounters == 0 selects APMU_NCOUNTERS_DEFAULT, or the value of
 * the APMU_NCOUNTERS environment variable if set. Returns 0, or -1 with
 * apmu_strerror() set (needs root and CONFIG_DEVMEM=y).
 */
int  apmu_open(apmu_t *a, unsigned ncounters);
void apmu_close(apmu_t *a);

/*
 * Open against a PMU whose register block sits somewhere other than
 * APMU_BASE. The internal offsets are structural -- the whole map moves as a
 * unit -- so this just slides the mmap by (base - APMU_BASE) and leaves every
 * address in this API at its canonical value.
 *
 * You need it because the map HAS moved: an older APMU revision put the
 * register block at 0x10404000 with ISPM/DSPM at 0x10426000/0x10427000, a
 * uniform 0x1000 below today's. Bitstreams of both vintages exist. `apmuctl
 * whichmap` tells you which one you have.
 *
 * base == 0 means APMU_BASE, or $APMU_BASE_ADDR if that is set.
 *
 * Caveat for the old revision: its DSPM was 32 KiB, not 128 KiB, so the top of
 * the mapped window has nothing behind it and reads back 0xBA5E1E55.
 */
int  apmu_open_at(apmu_t *a, unsigned ncounters, uint64_t base);

/* ---- raw MMIO ---------------------------------------------------- */

/* Full ordering against device memory. RISC-V `fence iorw,iorw`. */
void     apmu_fence(void);

/* Single 32-bit accesses. `phys` must be 4-byte aligned and inside the window;
 * a violation aborts the process rather than silently corrupting the PMU, so
 * validate untrusted addresses with apmu_in_window() first. */
uint32_t apmu_rd32(const apmu_t *a, uint64_t phys);
void     apmu_wr32(apmu_t *a, uint64_t phys, uint32_t val);

int      apmu_in_window(uint64_t phys, size_t len);

/* 64-bit registers (timer, period) are two 32-bit accesses, low word first.
 * The timer is free-running, so a lo/hi pair can tear; apmu_rd64() retries
 * until the high word is stable. */
uint64_t apmu_rd64(const apmu_t *a, uint64_t phys);
void     apmu_wr64(apmu_t *a, uint64_t phys, uint64_t val);

/* ---- PE control -------------------------------------------------- */

/*
 * IMPORTANT -- halt is not "pause".
 *
 * Writing STATUS.stall=1 parks the Ibex in its PMU_STALL state; clearing it
 * sends the controller to BOOT_SET, i.e. the PE restarts from BOOT_ADDR
 * (ibex_controller.sv:650-656). There is no resume. Register file and DSPM
 * contents survive, so firmware must not assume a cold machine beyond what its
 * own crt0 re-initialises.
 *
 * Counters keep counting while the PE is halted -- the PE is a consumer of
 * events, not a gate on them.
 */
void apmu_halt(apmu_t *a);
void apmu_run(apmu_t *a);
int  apmu_is_halted(const apmu_t *a);

void     apmu_set_boot_addr(apmu_t *a, uint32_t addr);
uint32_t apmu_get_boot_addr(const apmu_t *a);

uint64_t apmu_get_timer(const apmu_t *a);
void     apmu_set_period(apmu_t *a, uint64_t period);
uint64_t apmu_get_period(const apmu_t *a);
/* Period = all ones: the timer free-runs instead of wrapping on a MemGuard
 * window. Every working firmware in the tree does this first. */
void     apmu_set_timer_free_running(apmu_t *a);

/* ---- counters ---------------------------------------------------- */

uint32_t apmu_counter_get(const apmu_t *a, unsigned i);
void     apmu_counter_set(apmu_t *a, unsigned i, uint32_t v);
uint32_t apmu_event_sel_get(const apmu_t *a, unsigned i);
void     apmu_event_sel_set(apmu_t *a, unsigned i, uint32_t v);
uint32_t apmu_event_info_get(const apmu_t *a, unsigned i);
void     apmu_event_info_set(apmu_t *a, unsigned i, uint32_t v);
uint32_t apmu_init_budget_get(const apmu_t *a, unsigned i);
void     apmu_init_budget_set(apmu_t *a, unsigned i, uint32_t v);

/* Zero counter, event select, event info and initial budget on every counter
 * this handle knows about. */
void     apmu_counters_reset(apmu_t *a);

/*
 * Probe how many counter bundles the loaded bitstream actually implements, by
 * walking the pages and write/read/restore-testing each Event Select register.
 * Requires the PE to be halted (it clobbers no counter values, but a running
 * firmware may be using the registers). Returns a count in [0, APMU_NCOUNTERS_MAX],
 * or -1 on error. Advisory only -- an unimplemented page decodes back into the
 * packed register struct rather than reading as zero, so treat a surprising
 * result as "check the bitstream", not as ground truth.
 */
int      apmu_probe_ncounters(apmu_t *a);

/* ---- scratchpad access ------------------------------------------- */

/*
 * Word-at-a-time copies to/from ISPM/DSPM. `len` need not be a multiple of 4:
 * writes zero-pad the final partial word (scratchpads have no side effects, so
 * this is safe), reads truncate it.
 *
 * apmu_write_mem_verify() writes then reads back and compares, returning the
 * number of mismatching words. Cheap and worth doing on ISPM -- a silently
 * half-loaded program is otherwise indistinguishable from a firmware bug.
 */
int apmu_write_mem(apmu_t *a, uint64_t phys, const void *buf, size_t len);
int apmu_read_mem(const apmu_t *a, uint64_t phys, void *buf, size_t len);
int apmu_zero_mem(apmu_t *a, uint64_t phys, size_t len);
long apmu_write_mem_verify(apmu_t *a, uint64_t phys, const void *buf, size_t len);

/* ---- firmware loading -------------------------------------------- */

typedef struct {
    /* Where the images ended up and how big they were. */
    uint64_t text_addr;
    size_t   text_size;
    uint64_t data_addr;
    size_t   data_size;
    uint64_t bss_addr;      /* 0 if none */
    size_t   bss_size;
    uint32_t entry;         /* boot address that was programmed */
    long     verify_errors; /* mismatching words on readback, 0 == good */
} apmu_image_t;

/*
 * Load raw .bin images -- the output of
 *     objcopy -O binary --only-section=.text                 fw.elf text.bin
 *     objcopy -O binary --only-section=.data --only-section=.rodata \
 *                       --only-section=.bss                  fw.elf data.bin
 * which is exactly what the `generate-bin-files` rule in each
 * tests/<name>/pmu_firmware/Makefile produces.
 *
 * text goes to APMU_ISPM_BASE unconditionally. data goes to `data_addr`; pass
 * 0 for APMU_DSPM_BASE, which is where apmu-os links its DSPM section.
 *
 * Halts the PE, loads and verifies both sections, and leaves the boot
 * address alone -- mirroring the known-good baremetal sequence exactly. Loads,
 * verifies, and programs BOOT_ADDR = APMU_ISPM_BASE. Does NOT start the PE.
 * `data` may be NULL/0. Returns 0 on success.
 */
int apmu_load_raw(apmu_t *a,
                  const void *text, size_t text_len,
                  const void *data, size_t data_len,
                  uint64_t data_addr,
                  apmu_image_t *out);

/*
 * Load an RV32 ELF directly, so the board does not need binutils. Reproduces
 * the objcopy split by section flags: SHF_ALLOC|SHF_EXECINSTR PROGBITS become
 * the text image, other SHF_ALLOC PROGBITS become the data image (laid out by
 * address with gaps zero-filled, exactly as `objcopy -O binary` would), and
 * SHT_NOBITS (.bss) is zeroed in place.
 *
 * The .text section's link address is IGNORED; text is always loaded at
 * APMU_ISPM_BASE. That matches the proven baremetal loader, and it is what
 * makes the firmware linker scripts' "-4 quirk" ORIGIN harmless -- intra-.text
 * control flow is PC-relative. Data sections ARE placed at their link
 * addresses, because -mcmodel=medlow makes global references absolute.
 *
 * Returns 0 on success.
 */
/* Read a whole file into a malloc'd buffer; caller frees. NULL on failure. */
void *apmu_slurp(const char *path, size_t *lenp);

/* Load raw text/data .bin images from disk. data_path may be NULL; data_addr 0
 * means APMU_DSPM_BASE. Same contract as apmu_load_raw() otherwise. */
int apmu_load_file(apmu_t *a, const char *text_path, const char *data_path,
                   uint64_t data_addr, apmu_image_t *out);

/* The ELF loader is not built; see libapmu_elf.c.unused for why. */

#ifdef __cplusplus
}
#endif

#endif /* LIBAPMU_H */
