/*
 * Linker for apmu-os components: places a relocatable RV32 object (built by
 * apmu-os `make components`) at given ISPM/DSPM addresses and resolves calls
 * into the base image from its export table. Builds in the kernel and in
 * userspace (for tests); it touches no hardware.
 */
#ifndef APMU_LINK_H
#define APMU_LINK_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include "../apmu_abi.h"

#define APMU_LINK_MAX_SECTIONS  64
#define APMU_LINK_MAX_EXPORTS   32

struct apmu_link {
	/* in */
	const uint8_t *obj;
	size_t len;
	abi_export_t exp[APMU_LINK_MAX_EXPORTS];
	unsigned nexp;
	/* out of apmu_link_parse() */
	uint32_t text_len, data_len;
	/* out of apmu_link_place(): component_t fields, as link addresses */
	uint32_t bitmask, event, request, init, exit;
	char err[96];
	/* private */
	const void *sh, *sym;
	const char *str;
	unsigned nsh, nsym;
	uint32_t strsize;
	uint32_t off[APMU_LINK_MAX_SECTIONS], addr[APMU_LINK_MAX_SECTIONS];
	uint8_t *img[APMU_LINK_MAX_SECTIONS];
};

/* Check the object and size its code (text_len) and data (data_len). */
int apmu_link_parse(struct apmu_link *l);

/*
 * Link for code loaded at ISPM physical address text_at and data at DSPM
 * physical address data_at, into zeroed buffers of text_len and data_len
 * bytes, with component_id = id. Returns 0, or -1 with l->err set.
 */
int apmu_link_place(struct apmu_link *l, uint32_t text_at, uint32_t data_at,
		    uint8_t *text, uint8_t *data, uint32_t id);

#endif
