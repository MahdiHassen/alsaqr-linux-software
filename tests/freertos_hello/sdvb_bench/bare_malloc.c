#include <stddef.h>
#include <string.h>
#include <stdarg.h>

/*
 * Bump allocator over a fixed DRAM region at 0x81000000 (32 MiB), well clear of
 * the FreeRTOS image/heap/stack (below ~0x80300000) and the FAT image at
 * 0x83000000. bare_malloc_reset() rewinds it so each benchmark run starts fresh.
 * memmove/errno/assert come from newlib (we link -lc), so they are not redefined
 * here as they were in the bare-metal (-nostdlib) original.
 */
#define HEAP_BASE ((unsigned char *)0x81000000UL)
#define HEAP_SIZE (32u * 1024u * 1024u)

static unsigned char *heap_ptr = (unsigned char *)0x81000000UL;

void bare_malloc_reset(void) { heap_ptr = HEAP_BASE; }

void *bare_malloc(size_t size) {
    if (size == 0) return (void *)heap_ptr;
    size = (size + 7u) & ~7u;
    void *p = (void *)heap_ptr;
    heap_ptr += size;
    return p;
}

void  bare_free(void *ptr)              { (void)ptr; }
void *bare_calloc(size_t n, size_t sz) { void *p = bare_malloc(n*sz); if(p) memset(p,0,n*sz); return p; }
void *bare_realloc(void *ptr, size_t sz){ (void)ptr; return bare_malloc(sz); }

/* ---- minimal sprintf ---- */
int bare_sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *out = buf;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { *out++ = *f; continue; }
        f++;
        if (*f == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s) *out++ = *s++;
        } else if (*f == 'd' || *f == 'i') {
            int v = va_arg(args, int);
            if (v < 0) { *out++ = '-'; v = -v; }
            char tmp[12]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            else { while (v) { tmp[n++] = (char)('0' + v%10); v /= 10; } }
            for (int k = n-1; k >= 0; k--) *out++ = tmp[k];
        } else if (*f == 'u') {
            unsigned v = va_arg(args, unsigned);
            char tmp[12]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            else { while (v) { tmp[n++] = (char)('0' + v%10); v /= 10; } }
            for (int k = n-1; k >= 0; k--) *out++ = tmp[k];
        } else if (*f == 'c') { *out++ = (char)va_arg(args, int);
        } else if (*f == '%') { *out++ = '%';
        } else { *out++ = '%'; *out++ = *f; }
    }
    *out = '\0';
    va_end(args);
    return (int)(out - buf);
}
