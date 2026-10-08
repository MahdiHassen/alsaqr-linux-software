/*
 * libapmu -- the application interface to the APMU.
 *
 * Talks to the apmu kernel module through /dev/apmu (apmu_ioctl.h); the
 * module owns the hardware, links and places components, and routes their
 * responses. A component is a relocatable object built with apmu-os
 * (`make components`, see apmu-os/README.md).
 *
 *   int fd = apmu_open();
 *   int id = apmu_install_file(fd, "hello.o", 0, err, sizeof err);
 *   uint32_t req[] = { 5, 6, 7 }, resp[APMU_MSG_WORDS];
 *   int n = apmu_call(fd, id, req, 3, resp, APMU_MSG_WORDS, 1000);
 *   apmu_close(fd);                 // also removes the component
 *
 * Functions return -1 and set errno on failure.
 */
#ifndef LIBAPMU_H
#define LIBAPMU_H

#include <stddef.h>
#include <stdint.h>

#include "apmu_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

int  apmu_open(void);
/* Components installed through fd (without APMU_INSTALL_PERSIST) are removed. */
void apmu_close(int fd);

/* Load and start apmu-os (root). Removes every component. */
int  apmu_boot(int fd, const void *text, size_t text_len, const void *data, size_t data_len);
int  apmu_boot_files(int fd, const char *text_path, const char *data_path);

/* Returns the component id. On failure, err (if not NULL) says why. */
int  apmu_install(int fd, const void *obj, size_t len, unsigned flags, char *err, size_t errlen);
int  apmu_install_file(int fd, const char *path, unsigned flags, char *err, size_t errlen);
int  apmu_uninstall(int fd, unsigned id);

/* Requests and responses are whole 32-bit words, at most APMU_MSG_WORDS. */
int  apmu_send(int fd, unsigned id, const uint32_t *w, unsigned n);
/* Next response from id, waiting up to timeout_ms (<0: forever). Returns its
 * length in words; at most max are stored. */
int  apmu_recv(int fd, unsigned id, uint32_t *w, unsigned max, int timeout_ms);
/* apmu_send, then apmu_recv. */
int  apmu_call(int fd, unsigned id, const uint32_t *req, unsigned n,
               uint32_t *resp, unsigned max, int timeout_ms);

int  apmu_info(int fd, struct apmu_info *info);

#ifdef __cplusplus
}
#endif

#endif
