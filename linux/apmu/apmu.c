/*
 * apmu -- command-line front end to libapmu (the apmu kernel module).
 *
 *   apmu boot text_section.bin data_rodata_bss.bin
 *   apmu install hello.o          # -> 1
 *   apmu req 1 5 6 7              # -> 00000006 00000007 00000008
 *   apmu list
 *   apmu uninstall 1
 *   apmu run hello.o 5 6 7        # install for this command only
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libapmu.h"

#define TIMEOUT_MS 1000

static void usage(void)
{
    fprintf(stderr,
"usage: apmu <command> [args]\n"
"  boot TEXT.bin DATA.bin   load and start apmu-os (removes all components)\n"
"  install OBJ              install a component until uninstalled; prints its id\n"
"  uninstall ID\n"
"  list                     apmu-os state and installed components\n"
"  req ID [WORD...]         send a request, print the response\n"
"  recv ID                  print the next response\n"
"  run OBJ [WORD...]        install OBJ, send one request, print the response,\n"
"                           remove it\n");
    exit(2);
}

static void die(const char *what)
{
    fprintf(stderr, "apmu: %s: %s\n", what, strerror(errno));
    exit(1);
}

static unsigned num(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);

    if (end == s || *end) {
        fprintf(stderr, "apmu: bad number '%s'\n", s);
        exit(2);
    }
    return v;
}

static void print_words(const uint32_t *w, int n)
{
    int i;

    for (i = 0; i < n && i < APMU_MSG_WORDS; i++)
        printf("%08x%c", w[i], i + 1 < n ? ' ' : '\n');
    if (!n)
        printf("(empty)\n");
}

/* Send argv as a request to id and print the response. */
static void request(int fd, unsigned id, int argc, char **argv)
{
    uint32_t w[APMU_MSG_WORDS];
    int i, n;

    if (argc > APMU_MSG_WORDS)
        usage();
    for (i = 0; i < argc; i++)
        w[i] = num(argv[i]);
    if (apmu_send(fd, id, w, argc) < 0)
        die("send");
    n = apmu_recv(fd, id, w, APMU_MSG_WORDS, TIMEOUT_MS);
    if (n < 0 && errno == ETIMEDOUT)
        printf("no response\n");
    else if (n < 0)
        die("recv");
    else
        print_words(w, n);
}

int main(int argc, char **argv)
{
    char err[96];
    int fd, id, i;

    if (argc < 2)
        usage();
    fd = apmu_open();
    if (fd < 0)
        die(APMU_DEV);

    if (!strcmp(argv[1], "boot") && argc == 4) {
        if (apmu_boot_files(fd, argv[2], argv[3]) < 0)
            die("boot");
        printf("apmu-os running\n");
    } else if ((!strcmp(argv[1], "install") && argc == 3) ||
               (!strcmp(argv[1], "run") && argc >= 3)) {
        int run = argv[1][0] == 'r';

        id = apmu_install_file(fd, argv[2], run ? 0 : APMU_INSTALL_PERSIST, err, sizeof err);
        if (id < 0) {
            fprintf(stderr, "apmu: install %s: %s\n", argv[2], err);
            return 1;
        }
        if (run)
            request(fd, id, argc - 3, argv + 3);
        else
            printf("%d\n", id);
    } else if (!strcmp(argv[1], "uninstall") && argc == 3) {
        if (apmu_uninstall(fd, num(argv[2])) < 0)
            die("uninstall");
    } else if (!strcmp(argv[1], "req") && argc >= 3) {
        request(fd, num(argv[2]), argc - 3, argv + 3);
    } else if (!strcmp(argv[1], "recv") && argc == 3) {
        uint32_t w[APMU_MSG_WORDS];
        int n = apmu_recv(fd, num(argv[2]), w, APMU_MSG_WORDS, TIMEOUT_MS);

        if (n < 0)
            die("recv");
        print_words(w, n);
    } else if (!strcmp(argv[1], "list") && argc == 2) {
        struct apmu_info info;

        if (apmu_info(fd, &info) < 0)
            die("info");
        printf("apmu-os %s", info.hung ? "not responding (reflash the FPGA)" :
               info.os_ready ? "running" : "not running");
        if (info.restarts)
            printf(", restarted on its own %u time(s)", info.restarts);
        printf("\n");
        for (i = 1; i < APMU_MAX_COMPONENTS; i++) {
            struct apmu_comp_info *c = &info.comp[i];

            if (c->id)
                printf("%u  ispm 0x%03x+%-4u dspm 0x%05x+%-5u%s\n", c->id,
                       c->ispm_off, c->ispm_size, c->dspm_off, c->dspm_size,
                       c->flags & APMU_INSTALL_PERSIST ? "" : "  (owned by a process)");
        }
    } else {
        usage();
    }
    apmu_close(fd);
    return 0;
}
