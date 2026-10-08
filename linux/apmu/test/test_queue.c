// Interop test: libapmu's host queue against apmu-os's PE queue_lib.c, both on
// the same backing file. Build: make test-queue (needs the apmu-os checkout).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "apmu_mmio.h"

static uintptr_t pe_dspm;
#define QUEUE_DSPM_BASE pe_dspm
#include "queue_lib.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    apmu_t a;
    uint32_t w[64], got[64], rid, size;
    unsigned n, i, k, sent = 0;
    if (argc != 2)
        return printf("usage: %s BACKING_FILE\n", argv[0]), 2;
    setenv("APMU_DEVMEM", argv[1], 1);
    if (apmu_mmio_open(&a, 0) < 0)
        return printf("open: %s\n", apmu_strerror()), 1;
    pe_dspm = (uintptr_t)a.map + (ABI_DSPM_PHYS - APMU_WIN_BASE);

    queue_init(ABI_QUEUE_REQ);
    queue_init(ABI_QUEUE_RSP);
    CHECK(apmu_queue_pop(&a, ABI_QUEUE_RSP, &rid, got, 64, &n) == 0);

    for (k = 0; k < 5000; k++) {
        unsigned len = 1 + (k * 7) % 40;
        queue_obj_t *o;

        for (i = 0; i < len; i++)
            w[i] = k * 1000 + i;
        if (apmu_queue_push(&a, ABI_QUEUE_REQ, k, w, len) < 0) {
            printf("push %u: %s\n", k, apmu_strerror());
            return 1;
        }
        sent++;
        if (k % 3)          /* let requests pile up so the queue fills and wraps */
            continue;
        while ((o = queue_pop_get_buffer(ABI_QUEUE_REQ, &size))) {
            queue_obj_t *r = queue_push_get_buffer(ABI_QUEUE_RSP, size);
            CHECK(r != 0);
            r->req_id = o->req_id;
            for (i = 0; i < size / 4; i++)
                r->payload[i] = o->payload[i] + 1;
            queue_push_buffer(ABI_QUEUE_RSP, r);
            queue_pop(ABI_QUEUE_REQ);
            CHECK(apmu_queue_pop(&a, ABI_QUEUE_RSP, &rid, got, 64, &n) == 1);
            for (i = 0; i < n; i++)
                CHECK(got[i] == rid * 1000 + i + 1);
        }
    }

    /* fill until push refuses, then check nothing was lost */
    queue_init(ABI_QUEUE_REQ);
    for (k = 0; apmu_queue_push(&a, ABI_QUEUE_REQ, k, w, 10) == 0; k++)
        ;
    CHECK(k == (ABI_Q_DATA_SIZE - 1) / 48);   /* 48-byte objects, one gap kept */
    for (i = 0; queue_pop_get_buffer(ABI_QUEUE_REQ, &size); i++)
        queue_pop(ABI_QUEUE_REQ);
    CHECK(i == k);

    printf("%u requests, %d failures, queue full at %u objects\n", sent, fails, k);
    return fails != 0;
}
