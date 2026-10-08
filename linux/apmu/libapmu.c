/* libapmu -- see libapmu.h. A thin layer over the /dev/apmu ioctls. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "libapmu.h"

int apmu_open(void)
{
    return open(APMU_DEV, O_RDWR | O_CLOEXEC);
}

void apmu_close(int fd)
{
    close(fd);
}

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    void *buf = NULL;
    long n;

    if (!f)
        return NULL;
    errno = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc(n ? n : 1)) && fread(buf, 1, n, f) == (size_t)n) {
        *len = n;
    } else {
        free(buf);
        buf = NULL;
        if (!errno)
            errno = EIO;
    }
    fclose(f);
    return buf;
}

int apmu_boot(int fd, const void *text, size_t text_len, const void *data, size_t data_len)
{
    struct apmu_boot b = {
        .text = (uintptr_t)text, .data = (uintptr_t)data,
        .text_len = text_len, .data_len = data_len,
    };

    return ioctl(fd, APMU_IOC_BOOT, &b);
}

int apmu_boot_files(int fd, const char *text_path, const char *data_path)
{
    size_t tl = 0, dl = 0;
    void *t = slurp(text_path, &tl), *d = t ? slurp(data_path, &dl) : NULL;
    int r = d ? apmu_boot(fd, t, tl, d, dl) : -1;
    int e = errno;

    free(t);
    free(d);
    errno = e;
    return r;
}

int apmu_install(int fd, const void *obj, size_t len, unsigned flags, char *err, size_t errlen)
{
    struct apmu_install in = { .obj = (uintptr_t)obj, .len = len, .flags = flags };
    int e;

    if (ioctl(fd, APMU_IOC_INSTALL, &in) < 0) {
        e = errno;
        if (err && errlen)
            snprintf(err, errlen, "%s", in.err[0] ? in.err : strerror(e));
        errno = e;
        return -1;
    }
    return in.id;
}

int apmu_install_file(int fd, const char *path, unsigned flags, char *err, size_t errlen)
{
    size_t len;
    void *obj = slurp(path, &len);
    int r, e;

    if (!obj) {
        if (err && errlen)
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    r = apmu_install(fd, obj, len, flags, err, errlen);
    e = errno;
    free(obj);
    errno = e;
    return r;
}

int apmu_uninstall(int fd, unsigned id)
{
    uint32_t v = id;

    return ioctl(fd, APMU_IOC_UNINSTALL, &v);
}

int apmu_send(int fd, unsigned id, const uint32_t *w, unsigned n)
{
    struct apmu_msg m = { .id = id, .nwords = n };

    if (n > APMU_MSG_WORDS) {
        errno = EINVAL;
        return -1;
    }
    memcpy(m.w, w, 4 * n);
    return ioctl(fd, APMU_IOC_SEND, &m);
}

int apmu_recv(int fd, unsigned id, uint32_t *w, unsigned max, int timeout_ms)
{
    struct apmu_msg m = { .id = id, .timeout_ms = timeout_ms };

    if (ioctl(fd, APMU_IOC_RECV, &m) < 0)
        return -1;
    memcpy(w, m.w, 4 * (m.nwords < max ? m.nwords : max));
    return m.nwords;
}

int apmu_call(int fd, unsigned id, const uint32_t *req, unsigned n,
              uint32_t *resp, unsigned max, int timeout_ms)
{
    if (apmu_send(fd, id, req, n) < 0)
        return -1;
    return apmu_recv(fd, id, resp, max, timeout_ms);
}

int apmu_info(int fd, struct apmu_info *info)
{
    return ioctl(fd, APMU_IOC_INFO, info);
}
