// Link a component with kmod/apmu_link.c and write the code and data images,
// for test_link.sh to compare against ld.lld.
//   test_link OBJ TEXT_AT DATA_AT TEXT_OUT DATA_OUT [NAME ADDR]...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kmod/apmu_link.h"

static void put(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n)
        exit(1);
    fclose(f);
}

int main(int argc, char **argv)
{
    static struct apmu_link l;
    static uint8_t obj[0x10000];
    uint8_t *text, *data;
    FILE *f;
    int i;

    if (argc < 6 || !(f = fopen(argv[1], "rb")))
        return 2;
    l.obj = obj;
    l.len = fread(obj, 1, sizeof obj, f);
    for (i = 6; i + 1 < argc && l.nexp < APMU_LINK_MAX_EXPORTS; i += 2, l.nexp++) {
        strncpy(l.exp[l.nexp].name, argv[i], sizeof l.exp[0].name - 1);
        l.exp[l.nexp].addr = strtoul(argv[i + 1], 0, 0);
    }
    if (apmu_link_parse(&l) < 0)
        return printf("parse: %s\n", l.err), 1;
    text = calloc(1, l.text_len);
    data = calloc(1, l.data_len + 4);
    if (apmu_link_place(&l, strtoul(argv[2], 0, 0), strtoul(argv[3], 0, 0), text, data, 7) < 0)
        return printf("place: %s\n", l.err), 1;
    put(argv[4], text, l.text_len);
    put(argv[5], data, l.data_len);
    printf("text %u B, data %u B, bitmask %x, event %x, request %x, init %x, exit %x\n",
           l.text_len, l.data_len, l.bitmask, l.event, l.request, l.init, l.exit);
    return 0;
}
