/* insmodp MODULE.ko [param=value ...]: finit_module with parameters (busybox's small insmod drops them) */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
int main(int argc, char **argv) {
  char params[1024] = ""; int i, fd;
  if (argc < 2) { fprintf(stderr, "usage: insmodp MODULE.ko [param=value ...]\n"); return 2; }
  for (i = 2; i < argc; i++) { if (i > 2) strcat(params, " "); strncat(params, argv[i], sizeof(params) - strlen(params) - 2); }
  fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd < 0) { perror(argv[1]); return 1; }
  if (syscall(SYS_finit_module, fd, params, 0) != 0) { perror("finit_module"); return 1; }
  return 0;
}
