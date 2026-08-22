/*
 * recv — pull a file out of the JTAG landing zone into the CVA6 Linux filesystem.
 *
 * Counterpart to alsaqr-software/scripts/send_file.sh. The host writes a payload
 * into the 16 MB window at 0x9f000000 using the debug module's system-bus master
 * (no hart halt required), then publishes it by writing the magic word LAST.
 * This program waits for that magic, copies the payload to a file, and clears
 * the magic to mark the window consumed.
 *
 * Layout at WIN_BASE:
 *     +0x0000  magic   'ALSQ' (0x51534c41)   written last by the host
 *     +0x0004  length  payload bytes
 *     +0x1000  payload
 *
 * Why /dev/mem + mmap, and why a binary at all:
 *   The window is deliberately carved OUT of Linux's memory node (build the DTB
 *   with MEM_SIZE=0x1F000000; see RUNBOOK §8), so the kernel never maps or caches
 *   it -- which is what makes the un-snooped system-bus writes safe. But that
 *   also puts it above high_memory, and read() on /dev/mem is gated by
 *   valid_phys_addr_range() (addr + count <= __pa(high_memory)), so byte-stream
 *   tools would fail -EFAULT there even if present. mmap is gated by
 *   valid_mmap_phys_addr_range(), which is permissive, so mmap works. That is
 *   also why busybox `devmem` (mmap-based) works for smoke tests.
 *   Note the target busybox has CONFIG_DD unset -- there is no dd to fall back
 *   on regardless (busybox64.config), which is why this program exists.
 *
 * O_SYNC on /dev/mem is what requests an uncached mapping. Do not drop it.
 *
 * Build:  make -C linux/recv     (needs a RISC-V *Linux* cross gcc -- the bundled
 *                                toolchain/rv64 is bare-metal and cannot build this).
 *         Normally built for you into the rootfs by cva6-sdk's ROOTFS_EXTRAS.
 * Usage:  recv [-w] [-k] [-o OUTFILE]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#define WIN_BASE   0x9f000000UL
#define WIN_SIZE   (16UL * 1024 * 1024)
#define HDR_SIZE   0x1000UL
#define MAGIC      0x51534c41UL          /* 'ALSQ' */
#define MAX_LEN    (WIN_SIZE - HDR_SIZE)

#define OFF_MAGIC  0
#define OFF_LEN    1                     /* in 32-bit words */

static void die(const char *msg)
{
    fprintf(stderr, "recv: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: recv [-w] [-k] [-o OUTFILE]\n"
        "  -w         wait (poll) until the host publishes a payload\n"
        "  -k         keep the magic word (do not mark the window consumed)\n"
        "  -o FILE    output path (default: received.bin)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *out = "received.bin";
    int wait = 0, keep = 0, c;

    while ((c = getopt(argc, argv, "wko:h")) != -1) {
        switch (c) {
        case 'w': wait = 1; break;
        case 'k': keep = 1; break;
        case 'o': out = optarg; break;
        default:  usage();
        }
    }

    if (optind < argc) {
        fprintf(stderr, "recv: unexpected argument '%s'"
                        " (the output path goes after -o)\n", argv[optind]);
        usage();
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        die("open /dev/mem (need root; kernel needs CONFIG_DEVMEM=y)");

    void *map = mmap(NULL, WIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, (off_t)WIN_BASE);
    if (map == MAP_FAILED)
        die("mmap window");
    close(fd);

    volatile uint32_t *hdr = (volatile uint32_t *)map;
    const unsigned char *payload = (const unsigned char *)map + HDR_SIZE;

    /* Volatile read in the loop: the mapping is uncached, so each read is a real
     * bus access and the compiler must not hoist it out. */
    uint32_t magic;
    for (;;) {
        magic = hdr[OFF_MAGIC];
        if (magic == MAGIC || !wait)
            break;
        usleep(50000);
    }

    if (magic != MAGIC) {
        if (magic == 0)
            fprintf(stderr, "recv: window empty (no payload published)\n");
        else
            fprintf(stderr,
                    "recv: bad magic 0x%08x (expected 0x%08lx)\n"
                    "      Garbage rather than 0 usually means the window is not "
                    "backed by DRAM --\n"
                    "      check the DTB really carved 0x%08lx out of memory@80000000 "
                    "(MEM_SIZE=0x1F000000).\n",
                    magic, MAGIC, WIN_BASE);
        return 1;
    }

    uint32_t len = hdr[OFF_LEN];
    if (len == 0 || len > MAX_LEN) {
        fprintf(stderr, "recv: implausible length %u (max %lu)\n", len, MAX_LEN);
        return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f)
        die(out);
    if (fwrite((const void *)payload, 1, len, f) != len) {
        fclose(f);
        die("write output");
    }
    if (fclose(f) != 0)
        die("close output");

    if (!keep)
        hdr[OFF_MAGIC] = 0;      /* mark consumed so a re-run does not duplicate */

    printf("recv: wrote %u bytes to %s\n", len, out);
    munmap(map, WIN_SIZE);
    return 0;
}
