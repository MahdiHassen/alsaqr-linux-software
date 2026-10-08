// SPDX-License-Identifier: GPL-2.0
/*
 * apmu: owns the AlSaqr APMU and the apmu-os running on its PE.
 *
 * Userspace (through /dev/apmu, see ../apmu_ioctl.h) boots apmu-os, installs
 * component objects, and exchanges messages with them. This module does the
 * rest: it picks component ids and free ISPM/DSPM, links the object
 * (apmu_link.c), writes it while the PE continues running, and routes every
 * response from apmu-os's single response queue to a mailbox per component.
 *
 * All hardware access and state are under one mutex. There is no interrupt
 * from the PE, so waits poll.
 */
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../apmu_abi.h"
#include "../apmu_ioctl.h"
#include "apmu_link.h"

#define APMU_PHYS       0x10405000u
#define APMU_SIZE       0x44000u
#define R_STATUS        0x1000u                 /* bit 0: stall */
#define R_COUNTER(i)    (0x2000u + (i) * 0x1000u)
#define ISPM            (ABI_ISPM_PHYS - APMU_PHYS)
#define DSPM            (ABI_DSPM_PHYS - APMU_PHYS)
#define DSPM_SIZE       0x20000u
#define QBASE(q)        (DSPM + ((q) == ABI_QUEUE_REQ ? ABI_REQ_Q_OFF : ABI_RSP_Q_OFF))
#define OBJ_LEN(size)   (8 + (((size) + 3) & ~3u))

#define MBOX_LEN        8
#define BASE_TIMEOUT_MS 2000
#define MAX_OBJ         0x10000

struct msg {
	u32 n;
	u32 w[APMU_MSG_WORDS];
};

struct comp {
	u32 id;                 /* 0: free (slot 0 is only the base mailbox) */
	u32 generation;
	u32 flags;
	struct file *owner;     /* NULL when persistent */
	u32 ispm_off, ispm_size, dspm_off, dspm_size;
	struct msg mbox[MBOX_LEN];
	unsigned int head, count;
};

static struct {
	void __iomem *io;
	struct mutex lock;
	struct comp comp[ABI_MAX_COMPONENTS];
	u32 session;            /* our stamp in DSPM; apmu-os clears it on a cold boot */
	u32 restarts;           /* cold restarts that took components with them */
	u32 next_generation;
	bool hung;              /* apmu-os stopped answering; requires a cold boot */
} apmu;

/* ---------------------------------------------------------------- hardware */

static u32 rd(u32 off) { return readl(apmu.io + off); }
static void wr(u32 off, u32 v) { writel(v, apmu.io + off); }

static bool os_ready(void) { return rd(DSPM + ABI_STATUS_OFF) == ABI_STATUS_READY; }
static bool halted(void) { return rd(R_STATUS) & 1; }
static void doorbell(void) { wr(R_COUNTER(ABI_DOORBELL_COUNTER), 0x80000000u); }

static void halt(void)
{
	int i;

	wr(R_STATUS, 1);
	for (i = 0; i < 64; i++)
		rd(R_STATUS);
}

static int write_verify(u32 off, const void *buf, size_t len)
{
	const u8 *p = buf;
	size_t i;
	u32 v;

	for (i = 0; i < len; i += 4) {
		v = 0;
		memcpy(&v, p + i, min_t(size_t, 4, len - i));
		wr(off + i, v);
	}
	for (i = 0; i < len; i += 4) {
		v = 0;
		memcpy(&v, p + i, min_t(size_t, 4, len - i));
		if (rd(off + i) != v)
			return -EIO;
	}
	return 0;
}

static int queue_ptrs(unsigned int q, u32 *head, u32 *tail)
{
	*head = rd(QBASE(q));
	*tail = rd(QBASE(q) + 4);
	if (*head >= ABI_Q_DATA_SIZE || *tail >= ABI_Q_DATA_SIZE || ((*head | *tail) & 3))
		return -EIO;
	return 0;
}

/* Two-phase push, as on the PE: write the object, then publish the head. */
static int queue_push(u32 id, const u32 *w, unsigned int n)
{
	u32 data = QBASE(ABI_QUEUE_REQ) + 8, head, tail, len = OBJ_LEN(4 * n), pos;
	unsigned int i;

	if (queue_ptrs(ABI_QUEUE_REQ, &head, &tail))
		return -EIO;
	pos = head;
	if (head >= tail) {
		u32 end = ABI_Q_DATA_SIZE - head;

		if (!(len < end || (len == end && tail != 0))) {
			if (len >= tail)
				return -EAGAIN;
			wr(data + head, ABI_Q_WRAP);
			pos = 0;
		}
	} else if (len >= tail - head) {
		return -EAGAIN;
	}
	wr(data + pos, 4 * n);
	wr(data + pos + 4, id);
	for (i = 0; i < n; i++)
		wr(data + pos + 8 + 4 * i, w[i]);
	head = pos + len;
	wr(QBASE(ABI_QUEUE_REQ), head == ABI_Q_DATA_SIZE ? 0 : head);
	return 0;
}

/* Pop one response into its component's mailbox (the oldest is dropped when full). */
static int queue_pop(void)
{
	u32 data = QBASE(ABI_QUEUE_RSP) + 8, head, tail, size, id;
	struct comp *c;
	struct msg *m;
	unsigned int i, n;

	if (queue_ptrs(ABI_QUEUE_RSP, &head, &tail))
		return -EIO;
	if (tail == head)
		return 0;
	size = rd(data + tail);
	if (size == ABI_Q_WRAP) {
		tail = 0;
		wr(QBASE(ABI_QUEUE_RSP) + 4, 0);
		if (tail == head)
			return 0;
		size = rd(data);
	}
	if (OBJ_LEN(size) > ABI_Q_DATA_SIZE - tail)
		return -EIO;
	id = rd(data + tail + 4);
	if (id < ABI_MAX_COMPONENTS && (id == ABI_BASE_ID || apmu.comp[id].id)) {
		c = &apmu.comp[id];
		if (c->count == MBOX_LEN) {
			c->head = (c->head + 1) % MBOX_LEN;
			c->count--;
		}
		m = &c->mbox[(c->head + c->count++) % MBOX_LEN];
		n = (size + 3) / 4;
		m->n = min_t(u32, n, APMU_MSG_WORDS);
		for (i = 0; i < m->n; i++)
			m->w[i] = rd(data + tail + 8 + 4 * i);
	}
	tail += OBJ_LEN(size);
	wr(QBASE(ABI_QUEUE_RSP) + 4, tail == ABI_Q_DATA_SIZE ? 0 : tail);
	return 1;
}

static int drain(void)
{
	int r;

	while ((r = queue_pop()) > 0)
		;
	return r;
}

static bool mbox_take(struct comp *c, struct msg *out)
{
	if (!c->count)
		return false;
	*out = c->mbox[c->head];
	c->head = (c->head + 1) % MBOX_LEN;
	c->count--;
	return true;
}

/* Wait for the base component's {op, status, id} reply to op. */
static int base_wait(u32 op, u32 *status)
{
	unsigned long end = jiffies + msecs_to_jiffies(BASE_TIMEOUT_MS);
	struct msg m;

	do {
		if (drain() < 0)
			return -EIO;
		while (mbox_take(&apmu.comp[ABI_BASE_ID], &m))
			if (m.n >= 3 && m.w[0] == op) {
				*status = m.w[1];
				return 0;
			}
		usleep_range(100, 200);
	} while (time_before(jiffies, end));
	/*
	 * Stop accepting work until userspace explicitly cold-boots apmu-os.
	 */
	apmu.hung = true;
	pr_err("apmu: apmu-os is not responding; cold boot it before retrying\n");
	return -EIO;
}

static int base_op(const u32 *req, unsigned int n, u32 *status)
{
	int r;

	if (!os_ready() || halted())
		return -ENODEV;
	if (apmu.hung)
		return -EIO;
	r = queue_push(ABI_BASE_ID, req, n);
	if (r)
		return r;
	doorbell();
	return base_wait(req[0], status);
}

static void zero_region(u32 off, u32 len)
{
	u32 i;

	for (i = 0; i < len; i += 4)
		wr(off + i, 0);
}

/* ---------------------------------------------------------------- components */

static void comp_free(struct comp *c)
{
	memset(c, 0, sizeof(*c));
}

/* A trap restarts apmu-os cold; crt0.s leaves mcause, mepc, mtval behind. */
static void report_trap(void)
{
	u32 cause = rd(DSPM + ABI_TRAP_OFF), epc = rd(DSPM + ABI_TRAP_OFF + 4);
	u32 tval = rd(DSPM + ABI_TRAP_OFF + 8), id = 0, i;

	if (!cause)
		return;
	for (i = 1; i < ABI_MAX_COMPONENTS; i++)
		if (apmu.comp[i].id &&
		    epc - (ABI_ISPM_PHYS + apmu.comp[i].ispm_off) < apmu.comp[i].ispm_size)
			id = i;
	if (id)
		pr_warn("apmu: component %u trapped: mcause %#x mepc %#x mtval %#x\n",
			id, cause, epc, tval);
	else
		pr_warn("apmu: apmu-os trapped: mcause %#x mepc %#x mtval %#x\n", cause, epc, tval);
	wr(DSPM + ABI_TRAP_OFF, 0);
}

/*
 * If the session word no longer holds our stamp, apmu-os restarted cold behind
 * our back (the PE can do that on its own) and its components are gone:
 * forget them, so their users get ENOENT, and stamp the new session.
 */
static void check_session(void)
{
	bool lost = false;
	int i;

	if (!os_ready() || (apmu.session && rd(DSPM + ABI_SESSION_OFF) == apmu.session))
		return;
	report_trap();
	apmu.hung = false;
	for (i = 0; i < ABI_MAX_COMPONENTS; i++) {
		lost |= i && apmu.comp[i].id;
		comp_free(&apmu.comp[i]);
	}
	if (lost) {
		apmu.restarts++;
		pr_warn("apmu: apmu-os restarted; its components are gone\n");
	}
	apmu.session = get_random_u32() | 1;
	wr(DSPM + ABI_SESSION_OFF, apmu.session);
}

static int comp_uninstall(struct comp *c)
{
	u32 req[3] = { ABI_OP_UNINSTALL, c->id, c->generation }, st = ABI_OK;
	int r = base_op(req, ARRAY_SIZE(req), &st);

	if (r == -ENODEV) {     /* apmu-os is gone: nothing can execute it */
		comp_free(c);
		return 0;
	}
	if (r)
		return r;
	if (st != ABI_OK && st != ABI_ERR_NOENT)
		return -EIO;
	zero_region(ISPM + c->ispm_off, c->ispm_size);
	zero_region(DSPM + c->dspm_off, c->dspm_size);
	comp_free(c);
	return 0;
}

/* First fit in [lo, hi) around the ranges in use. */
static int fit(bool ispm, u32 lo, u32 hi, u32 size, u32 align, u32 *off)
{
	u32 best = hi, c, o, n;
	int i, j;

	for (i = -1; i < ABI_MAX_COMPONENTS; i++) {
		struct comp *ci = &apmu.comp[i < 0 ? 0 : i];

		if (i >= 0 && !ci->id)
			continue;
		c = i < 0 ? lo : ispm ? ci->ispm_off + ci->ispm_size : ci->dspm_off + ci->dspm_size;
		c = ALIGN(c, align);
		if (c < lo || c + size > hi || c >= best)
			continue;
		for (j = 1; j < ABI_MAX_COMPONENTS; j++) {
			struct comp *cj = &apmu.comp[j];

			o = ispm ? cj->ispm_off : cj->dspm_off;
			n = ispm ? cj->ispm_size : cj->dspm_size;
			if (cj->id && n && c < o + n && o < c + size)
				break;
		}
		if (j == ABI_MAX_COMPONENTS)
			best = c;
	}
	if (best == hi)
		return -ENOMEM;
	*off = best;
	return 0;
}

static int read_exports(struct apmu_link *l)
{
	u32 at = rd(DSPM + ABI_EXPORTS_OFF) - ABI_DSPM_PHYS, i;
	u32 *p = (u32 *)l->exp;

	l->nexp = rd(DSPM + ABI_NEXPORTS_OFF);
	if (l->nexp > APMU_LINK_MAX_EXPORTS ||
	    at + l->nexp * sizeof(abi_export_t) > DSPM_SIZE || (at & 3))
		return -EIO;
	for (i = 0; i < l->nexp * sizeof(abi_export_t) / 4; i++)
		p[i] = rd(DSPM + at + 4 * i);
	for (i = 0; i < l->nexp; i++)
		l->exp[i].name[sizeof(l->exp[i].name) - 1] = 0;
	return 0;
}

static void set_err(struct apmu_install *in, const char *msg)
{
	strscpy(in->err, msg, sizeof(in->err));
}

static int install(struct file *f, struct apmu_install *in)
{
	struct apmu_link *l;
	u8 *text = NULL, *data = NULL;
	u32 id, ioff, doff, dyn, generation, req[8], st;
	struct comp *c;
	int r;

	if (in->len > MAX_OBJ)
		return -EFBIG;
	if ((in->flags & ~APMU_INSTALL_PERSIST) ||
	    ((in->flags & APMU_INSTALL_PERSIST) && !capable(CAP_SYS_ADMIN)))
		return -EPERM;
	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;
	l->obj = memdup_user(u64_to_user_ptr(in->obj), in->len);
	if (IS_ERR(l->obj)) {
		r = PTR_ERR(l->obj);
		l->obj = NULL;
		goto out_free;
	}
	l->len = in->len;
	if (apmu_link_parse(l)) {
		set_err(in, l->err);
		r = -ENOEXEC;
		goto out_free;
	}
	text = kzalloc(l->text_len, GFP_KERNEL);
	data = kzalloc(l->data_len + 4, GFP_KERNEL);
	if (!text || !data) {
		r = -ENOMEM;
		goto out_free;
	}

	mutex_lock(&apmu.lock);
	check_session();
	r = -ENODEV;
	if (!os_ready() || halted() || read_exports(l))
		goto out;
	for (id = 1; id < ABI_MAX_COMPONENTS && apmu.comp[id].id; id++)
		;
	r = -EBUSY;
	if (id == ABI_MAX_COMPONENTS)
		goto out;
	dyn = rd(DSPM + ABI_DYN_ISPM_OFF);
	r = -ENOSPC;
	if (dyn > ABI_ISPM_SIZE ||
	    fit(true, dyn, ABI_ISPM_SIZE, l->text_len, 4, &ioff) ||
	    fit(false, ABI_DYN_DSPM_OFF, ABI_DYN_DSPM_OFF + ABI_DYN_DSPM_SIZE,
		l->data_len, 16, &doff)) {
		set_err(in, "no room in ISPM/DSPM");
		goto out;
	}
	if (apmu_link_place(l, ABI_ISPM_PHYS + ioff, ABI_DSPM_PHYS + doff, text, data, id)) {
		set_err(in, l->err);
		r = -ENOEXEC;
		goto out;
	}

	generation = ++apmu.next_generation;
	if (!generation)
		generation = ++apmu.next_generation;
	req[0] = ABI_OP_INSTALL;
	req[1] = id;
	req[2] = generation;
	req[3] = l->bitmask;
	req[4] = l->event;
	req[5] = l->request;
	req[6] = l->init;
	req[7] = l->exit;
	r = write_verify(ISPM + ioff, text, l->text_len);
	if (!r)
		r = write_verify(DSPM + doff, data, l->data_len);
	if (r)
		goto scrub;
	c = &apmu.comp[id];
	comp_free(c);
	c->id = id;
	c->generation = generation;
	c->flags = in->flags;
	c->owner = in->flags & APMU_INSTALL_PERSIST ? NULL : f;
	c->ispm_off = ioff;
	c->ispm_size = l->text_len;
	c->dspm_off = doff;
	c->dspm_size = l->data_len;
	r = queue_push(ABI_BASE_ID, req, ARRAY_SIZE(req));
	if (r) {
		comp_free(c);
		goto scrub;
	}
	doorbell();
	r = base_wait(ABI_OP_INSTALL, &st);
	if (r) {
		set_err(in, "install acknowledgement lost; component retained");
		goto out;
	}
	if (st != ABI_OK) {
		set_err(in, "rejected by apmu-os");
		r = -EIO;
		comp_free(c);
		goto scrub;
	}
	in->id = id;
	goto out;
scrub:
	zero_region(ISPM + ioff, l->text_len);
	zero_region(DSPM + doff, l->data_len);
out:
	mutex_unlock(&apmu.lock);
out_free:
	kfree(text);
	kfree(data);
	kfree(l->obj);
	kfree(l);
	return r;
}

/* The component id, if f may use it. */
static struct comp *comp_get(struct file *f, u32 id)
{
	struct comp *c;

	if (id == ABI_BASE_ID || id >= ABI_MAX_COMPONENTS)
		return NULL;
	c = &apmu.comp[id];
	if (!c->id || (c->owner && c->owner != f))
		return NULL;
	return c;
}

static int boot(struct apmu_boot *b)
{
	void *text, *data;
	unsigned long end;
	int i, r;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!b->text_len || b->text_len > ABI_ISPM_SIZE || b->data_len > ABI_STATUS_OFF)
		return -EINVAL;
	text = memdup_user(u64_to_user_ptr(b->text), b->text_len);
	if (IS_ERR(text))
		return PTR_ERR(text);
	data = memdup_user(u64_to_user_ptr(b->data), b->data_len);
	if (IS_ERR(data)) {
		kfree(text);
		return PTR_ERR(data);
	}

	mutex_lock(&apmu.lock);
	/* A deliberate host halt is destructive: the next release is a cold boot. */
	halt();
	wr(DSPM + ABI_STATUS_OFF, 0);
	wr(DSPM + ABI_TRAP_OFF, 0);
	for (i = 0; i < ABI_MAX_COMPONENTS; i++)
		comp_free(&apmu.comp[i]);
	r = write_verify(ISPM, text, b->text_len);
	if (!r)
		r = write_verify(DSPM, data, b->data_len);
	if (!r) {
		wr(R_STATUS, 0);
		end = jiffies + msecs_to_jiffies(BASE_TIMEOUT_MS);
		while (!os_ready() && time_before(jiffies, end))
			usleep_range(100, 200);
		r = os_ready() ? 0 : -ETIMEDOUT;
	}
	if (!r)
		apmu.hung = false;
	check_session();
	mutex_unlock(&apmu.lock);
	kfree(text);
	kfree(data);
	return r;
}

static int recv(struct file *f, struct apmu_msg *m)
{
	unsigned long end = jiffies + msecs_to_jiffies(m->timeout_ms);
	struct comp *c;
	struct msg got;
	int r;

	for (;;) {
		mutex_lock(&apmu.lock);
		check_session();
		c = comp_get(f, m->id);
		r = -ENOENT;
		if (c) {
			r = drain();
			if (!r)
				r = mbox_take(c, &got) ? 1 : 0;
		}
		mutex_unlock(&apmu.lock);
		if (r == 1) {
			m->nwords = got.n;
			memcpy(m->w, got.w, got.n * 4);
			return 0;
		}
		if (r)
			return r;
		if (m->timeout_ms >= 0 && !time_before(jiffies, end))
			return -ETIMEDOUT;
		if (signal_pending(current))
			return -EINTR;
		usleep_range(200, 500);
	}
}

/* ---------------------------------------------------------------- file ops */

static long apmu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *u = (void __user *)arg;
	union {
		struct apmu_boot b;
		struct apmu_install in;
		struct apmu_msg m;
		struct apmu_info info;
		u32 id;
	} *a;
	struct comp *c;
	long r;
	int i;

	if (_IOC_TYPE(cmd) != 'A' || _IOC_SIZE(cmd) > sizeof(*a))
		return -ENOTTY;
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && copy_from_user(a, u, _IOC_SIZE(cmd))) {
		r = -EFAULT;
		goto out;
	}

	switch (cmd) {
	case APMU_IOC_BOOT:
		r = boot(&a->b);
		break;
	case APMU_IOC_INSTALL:
		a->in.err[sizeof(a->in.err) - 1] = 0;
		a->in.err[0] = 0;
		r = install(f, &a->in);
		break;
	case APMU_IOC_UNINSTALL:
		mutex_lock(&apmu.lock);
		check_session();
		c = comp_get(f, a->id);
		r = !c ? -ENOENT : (!c->owner && !capable(CAP_SYS_ADMIN)) ? -EPERM : comp_uninstall(c);
		mutex_unlock(&apmu.lock);
		break;
	case APMU_IOC_SEND:
		if (a->m.nwords > APMU_MSG_WORDS) {
			r = -EINVAL;
			break;
		}
		mutex_lock(&apmu.lock);
		check_session();
		r = !comp_get(f, a->m.id) ? -ENOENT : !os_ready() ? -ENODEV :
		    queue_push(a->m.id, a->m.w, a->m.nwords);
		if (!r)
			doorbell();
		mutex_unlock(&apmu.lock);
		break;
	case APMU_IOC_RECV:
		r = recv(f, &a->m);
		break;
	case APMU_IOC_INFO:
		mutex_lock(&apmu.lock);
		check_session();
		a->info.os_ready = os_ready();
		a->info.hung = apmu.hung;
		a->info.restarts = apmu.restarts;
		for (i = 1; i < ABI_MAX_COMPONENTS; i++) {
			c = &apmu.comp[i];
			if (!c->id)
				continue;
			a->info.comp[i] = (struct apmu_comp_info){
				c->id, c->flags, c->ispm_off, c->ispm_size,
				c->dspm_off, c->dspm_size, c->owner == f, 0 };
		}
		mutex_unlock(&apmu.lock);
		r = 0;
		break;
	default:
		r = -ENOTTY;
	}
	/* INSTALL also returns its error text on failure. */
	if ((_IOC_DIR(cmd) & _IOC_READ) && (!r || cmd == APMU_IOC_INSTALL) &&
	    copy_to_user(u, a, _IOC_SIZE(cmd)))
		r = -EFAULT;
out:
	kfree(a);
	return r;
}

/* Components die with the fd that installed them. */
static int apmu_release(struct inode *inode, struct file *f)
{
	int i;

	mutex_lock(&apmu.lock);
	check_session();
	for (i = 1; i < ABI_MAX_COMPONENTS; i++)
		if (apmu.comp[i].id && apmu.comp[i].owner == f &&
		    comp_uninstall(&apmu.comp[i]))
			apmu.comp[i].owner = NULL; /* retain an unconfirmed live range */
	mutex_unlock(&apmu.lock);
	return 0;
}

static const struct file_operations apmu_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = apmu_ioctl,
	.release        = apmu_release,
};

static struct miscdevice apmu_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "apmu",
	.fops  = &apmu_fops,
	.mode  = 0666,
};

static int __init apmu_init(void)
{
	int r;

	mutex_init(&apmu.lock);
	apmu.io = ioremap(APMU_PHYS, APMU_SIZE);
	if (!apmu.io)
		return -ENOMEM;
	r = misc_register(&apmu_misc);
	if (r) {
		iounmap(apmu.io);
		return r;
	}
	mutex_lock(&apmu.lock);
	check_session();
	mutex_unlock(&apmu.lock);
	pr_info("apmu: ready, apmu-os %s\n", os_ready() ? "running" : "not loaded");
	return 0;
}

/* apmu-os keeps running; persistent components are removed with the module. */
static void __exit apmu_exit(void)
{
	int i;

	misc_deregister(&apmu_misc);
	mutex_lock(&apmu.lock);
	for (i = 1; i < ABI_MAX_COMPONENTS; i++)
		if (apmu.comp[i].id)
			comp_uninstall(&apmu.comp[i]);
	mutex_unlock(&apmu.lock);
	iounmap(apmu.io);
}

module_init(apmu_init);
module_exit(apmu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AlSaqr APMU and apmu-os components");
