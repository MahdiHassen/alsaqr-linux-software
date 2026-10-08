/*
 * /dev/apmu: the interface between the apmu kernel module and userspace.
 *
 * The module owns the APMU. Userspace hands it component objects and
 * messages; the module links, places, loads and talks to apmu-os. Components
 * belong to the file descriptor that installed them and are removed when it
 * is closed, unless installed with APMU_INSTALL_PERSIST.
 */
#ifndef APMU_IOCTL_H
#define APMU_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <linux/types.h>
#endif

#define APMU_DEV            "/dev/apmu"
#define APMU_MSG_WORDS      64      /* max payload of one request or response */
#define APMU_MAX_COMPONENTS 10      /* ids 1..9; id 0 is apmu-os's base component */

/* Load and start apmu-os (CAP_SYS_ADMIN). Removes every component. */
struct apmu_boot {
	__u64 text;         /* user pointer: ISPM image */
	__u64 data;         /* user pointer: DSPM image */
	__u32 text_len;
	__u32 data_len;
};

/* Install a relocatable RV32 component object; id is returned. */
#define APMU_INSTALL_PERSIST 0x1    /* outlives the fd (CAP_SYS_ADMIN) */
struct apmu_install {
	__u64 obj;          /* user pointer */
	__u32 len;
	__u32 flags;
	__u32 id;           /* out */
	__u32 pad;
	char err[80];       /* out: why it failed, if the module knows */
};

/*
 * A request (SEND) or response (RECV) for component id. RECV waits up to
 * timeout_ms (0: don't wait, <0: forever) and fails with ETIMEDOUT. Responses
 * longer than APMU_MSG_WORDS are truncated.
 */
struct apmu_msg {
	__u32 id;
	__u32 nwords;
	__s32 timeout_ms;
	__u32 pad;
	__u32 w[APMU_MSG_WORDS];
};

struct apmu_comp_info {
	__u32 id;           /* 0: slot unused */
	__u32 flags;        /* APMU_INSTALL_* */
	__u32 ispm_off, ispm_size;
	__u32 dspm_off, dspm_size;
	__u32 mine;         /* installed through this fd */
	__u32 pad;
};

struct apmu_info {
	__u32 os_ready;
	__u32 hung;         /* apmu-os stopped answering: cold boot it */
	__u32 restarts;     /* times apmu-os restarted on its own and lost components */
	__u32 pad;
	struct apmu_comp_info comp[APMU_MAX_COMPONENTS];
};

#define APMU_IOC_BOOT       _IOW('A', 1, struct apmu_boot)
#define APMU_IOC_INSTALL    _IOWR('A', 2, struct apmu_install)
#define APMU_IOC_UNINSTALL  _IOW('A', 3, __u32)
#define APMU_IOC_SEND       _IOW('A', 4, struct apmu_msg)
#define APMU_IOC_RECV       _IOWR('A', 5, struct apmu_msg)
#define APMU_IOC_INFO       _IOR('A', 6, struct apmu_info)

#endif
