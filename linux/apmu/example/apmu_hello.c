/*
 * apmu_hello -- the smallest complete "drive the PMU from Linux" program.
 *
 * Does what pmu_sad_profile/pmu_bench.c:load_and_start_pmu() does, but as a
 * normal Linux process:
 *
 *   1. map the APMU
 *   2. halt the PE
 *   3. free-running timer
 *   4. load firmware into ISPM/DSPM
 *   5. start the PE
 *   6. poll the DSPM mailbox for the firmware's "I'm alive" word
 *
 * Build:  make example        (in linux/apmu)
 * Run:    ./apmu_hello /path/to/pmu_firmware.elf
 *
 * The mailbox contract below is the one pmu_sad_profile's firmware uses. Your
 * firmware defines its own -- change STATUS_OFF/STATUS_STARTED to match, or
 * delete step 6 if it has no handshake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "apmu_mmio.h"

#define STATUS_OFF      0x00            /* DSPM byte offset of the status word */
#define STATUS_STARTED  0xBB000001u     /* what the firmware writes when up    */

int main(int argc, char **argv)
{
    apmu_t a;
    apmu_image_t img;
    uint64_t t0, t1;
    int tries;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <text.bin> [data.bin]\n", argv[0]);
        return 2;
    }

    if (apmu_mmio_open(&a, 0) < 0) {
        fprintf(stderr, "apmu_mmio_open: %s\n", apmu_strerror());
        return 1;
    }

    /* apmu_load_file() halts the PE, copies the text image to ISPM and the
     * data image to the DSPM base, and verifies both readbacks. It does not
     * start anything, and it deliberately leaves the boot address alone -- the
     * PE boots from its reset value, as in the known-good baremetal sequence. */
    if (apmu_load_file(&a, argv[1], argc == 3 ? argv[2] : NULL, 0, &img) < 0) {
        fprintf(stderr, "load: %s\n", apmu_strerror());
        apmu_mmio_close(&a);
        return 1;
    }
    printf("loaded: .text %zu B @ 0x%08llx, .data %zu B @ 0x%08llx, entry 0x%08x\n",
           img.text_size, (unsigned long long)img.text_addr,
           img.data_size, (unsigned long long)img.data_addr, img.entry);

    /* Every firmware in the tree assumes the timer is free-running. Do this
     * before releasing the stall, not after. */
    apmu_set_timer_free_running(&a);

    t0 = apmu_get_timer(&a);
    apmu_run(&a);

    /*
     * Releasing the stall restarts the PE at BOOT_ADDR -- it is a boot, not a
     * resume. Poll the mailbox rather than guessing at a delay.
     *
     * Busy-poll, no usleep(): the target kernel is CONFIG_HZ_100 with
     * CONFIG_HIGH_RES_TIMERS unset, so a usleep(100) really costs ~10 ms and a
     * 10000-iteration timeout would take 100 seconds. If your firmware takes
     * long enough that spinning is wasteful, sleep in units of the 10 ms tick
     * and size the count accordingly.
     */
    for (tries = 0; tries < 10000; tries++) {
        if (apmu_rd32(&a, APMU_DSPM_BASE + STATUS_OFF) == STATUS_STARTED)
            break;
    }
    t1 = apmu_get_timer(&a);

    if (tries == 10000) {
        printf("PE did not report STARTED; mailbox[0] = 0x%08x\n",
               apmu_rd32(&a, APMU_DSPM_BASE + STATUS_OFF));
        printf("(if your firmware has no handshake this is expected -- "
               "check `apmuctl dspm 0 16` for whatever it does write)\n");
    } else {
        printf("PE up after %llu APMU timer ticks\n",
               (unsigned long long)(t1 - t0));
    }

    /* Leave it running. `apmuctl halt` stops it; the next run is a reboot. */
    apmu_mmio_close(&a);
    return 0;
}
