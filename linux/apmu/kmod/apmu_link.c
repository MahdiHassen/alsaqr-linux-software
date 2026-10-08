// SPDX-License-Identifier: GPL-2.0
/*
 * Component linker (see apmu_link.h). Supports the relocation set the thesis
 * lists in Appendix A. Code and data are linked where they are loaded.
 */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif
#include "apmu_link.h"

struct ehdr {
	uint8_t  ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct shdr { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct sym  { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };
struct rela { uint32_t offset, info; int32_t addend; };

#define SHT_SYMTAB      2
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHF_ALLOC       2
#define SHF_EXECINSTR   4
#define SHN_ABS         0xfff1
#define STB_LOCAL       0

#define SH(l)   ((const struct shdr *)(l)->sh)
#define SYM(l)  ((const struct sym *)(l)->sym)

static int lerr(struct apmu_link *l, const char *msg, const char *arg)
{
	snprintf(l->err, sizeof(l->err), "%s%s", msg, arg ? arg : "");
	return -1;
}

static uint32_t rd(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t hi20(uint32_t v) { return (v + 0x800) & 0xfffff000u; }
static uint32_t lo12(uint32_t v) { return v - hi20(v); }
static uint32_t u_imm(uint32_t in, uint32_t v) { return (in & 0xfff) | hi20(v); }
static uint32_t i_imm(uint32_t in, uint32_t v) { return (in & 0x000fffff) | (v << 20); }
static uint32_t s_imm(uint32_t in, uint32_t v)
{
	return (in & 0x01fff07f) | ((v & 0xfe0) << 20) | ((v & 0x1f) << 7);
}
static uint32_t b_imm(uint32_t in, uint32_t v)
{
	return (in & 0x01fff07f) | ((v & 0x1000) << 19) | ((v & 0x7e0) << 20) |
	       ((v & 0x1e) << 7) | ((v & 0x800) >> 4);
}
static uint32_t j_imm(uint32_t in, uint32_t v)
{
	return (in & 0xfff) | ((v & 0x100000) << 11) | ((v & 0x7fe) << 20) |
	       ((v & 0x800) << 9) | (v & 0xff000);
}

static const char *sym_name(const struct apmu_link *l, const struct sym *s)
{
	return s->name < l->strsize ? l->str + s->name : "";
}

int apmu_link_parse(struct apmu_link *l)
{
	const struct ehdr *e = (const void *)l->obj;
	unsigned i;

	if (l->len < sizeof(*e) || memcmp(e->ident, "\177ELF", 4) || e->ident[4] != 1 ||
	    e->ident[5] != 1 || e->type != 1 || e->machine != 243)
		return lerr(l, "not a little-endian RV32 relocatable object", NULL);
	if (e->shentsize != sizeof(struct shdr) || e->shoff > l->len || (e->shoff & 3) ||
	    (size_t)e->shnum * sizeof(struct shdr) > l->len - e->shoff)
		return lerr(l, "bad section header table", NULL);
	if (e->shnum > APMU_LINK_MAX_SECTIONS)
		return lerr(l, "too many sections", NULL);
	l->sh = l->obj + e->shoff;
	l->nsh = e->shnum;
	l->text_len = l->data_len = 0;

	for (i = 0; i < l->nsh; i++) {
		const struct shdr *s = &SH(l)[i];
		uint32_t *len = (s->flags & SHF_EXECINSTR) ? &l->text_len : &l->data_len;
		uint32_t al = s->addralign > 4 ? s->addralign : 4;

		if (s->type != SHT_NOBITS && (s->offset > l->len || s->size > l->len - s->offset))
			return lerr(l, "section outside the file", NULL);
		if ((s->type == SHT_SYMTAB || s->type == SHT_RELA) && (s->offset & 3))
			return lerr(l, "misaligned table", NULL);
		if (s->type == SHT_SYMTAB) {
			if (s->link >= l->nsh || SH(l)[s->link].type == SHT_NOBITS)
				return lerr(l, "bad string table", NULL);
			l->sym = l->obj + s->offset;
			l->nsym = s->size / sizeof(struct sym);
			l->str = (const char *)l->obj + SH(l)[s->link].offset;
			l->strsize = SH(l)[s->link].size;
		}
		/* Lay out every allocated section: offsets in the text or data image. */
		l->off[i] = 0;
		if (!(s->flags & SHF_ALLOC) || !s->size)
			continue;
		if (al > 0x1000 || s->size > 0x20000)
			return lerr(l, "section too large", NULL);
		*len = (*len + al - 1) & ~(al - 1);
		l->off[i] = *len;
		*len += (s->size + 3) & ~3u;
	}
	if (!l->sym)
		return lerr(l, "no symbol table", NULL);
	/* The string table was bounds-checked in the loop; it must end in a NUL. */
	if (!l->strsize || l->str[l->strsize - 1])
		return lerr(l, "bad string table", NULL);
	if (!l->text_len)
		return lerr(l, "component has no code", NULL);
	return 0;
}

static int sym_addr(struct apmu_link *l, uint32_t idx, uint32_t *v)
{
	const struct sym *s;
	unsigned i;

	if (idx >= l->nsym)
		return lerr(l, "bad symbol index", NULL);
	s = &SYM(l)[idx];
	if (s->shndx == 0) {
		for (i = 0; i < l->nexp; i++)
			if (!strcmp(l->exp[i].name, sym_name(l, s))) {
				*v = l->exp[i].addr;
				return 0;
			}
		return lerr(l, "undefined symbol (not exported by apmu-os): ", sym_name(l, s));
	}
	if (s->shndx == SHN_ABS) {
		*v = s->value;
		return 0;
	}
	if (s->shndx >= l->nsh || !l->addr[s->shndx])
		return lerr(l, "symbol in a section that is not loaded: ", sym_name(l, s));
	*v = l->addr[s->shndx] + s->value;
	return 0;
}

/* PCREL_LO12 points at the auipc; its value comes from that auipc's PCREL_HI20. */
static int pcrel_hi(struct apmu_link *l, const struct shdr *rs, unsigned t, uint32_t at,
		    uint32_t *v)
{
	const struct rela *r = (const void *)(l->obj + rs->offset);
	unsigned i, n = rs->size / sizeof(*r);
	uint32_t s;

	for (i = 0; i < n; i++)
		if ((r[i].info & 0xff) == 23 && l->addr[t] + r[i].offset == at) {
			if (sym_addr(l, r[i].info >> 8, &s) < 0)
				return -1;
			*v = s + r[i].addend - at;
			return 0;
		}
	return lerr(l, "PCREL_LO12 without a matching PCREL_HI20", NULL);
}

static int relocate(struct apmu_link *l)
{
	unsigned i, k;

	for (i = 0; i < l->nsh; i++) {
		const struct shdr *rs = &SH(l)[i];
		const struct rela *r = (const void *)(l->obj + rs->offset);
		unsigned t = rs->info, n = rs->size / sizeof(*r);

		if (rs->type != SHT_RELA || t >= l->nsh || !l->addr[t])
			continue;
		for (k = 0; k < n; k++) {
			uint32_t type = r[k].info & 0xff, S = 0, P = l->addr[t] + r[k].offset, v;
			uint32_t need = type == 18 || type == 19 ? 8 : 4;
			uint8_t *loc = l->img[t] + r[k].offset;

			if (type == 0 || type == 51)                    /* NONE, RELAX */
				continue;
			if (SH(l)[t].size < need || r[k].offset > SH(l)[t].size - need)
				return lerr(l, "relocation outside its section", NULL);
			if (sym_addr(l, r[k].info >> 8, &S) < 0)
				return -1;
			v = S + r[k].addend;
			switch (type) {
			case 1:  wr(loc, v);                                break;  /* 32 */
			case 16: wr(loc, b_imm(rd(loc), v - P));            break;  /* BRANCH */
			case 17: wr(loc, j_imm(rd(loc), v - P));            break;  /* JAL */
			case 18:                                                    /* CALL */
			case 19: wr(loc, u_imm(rd(loc), v - P));                    /* CALL_PLT */
				 wr(loc + 4, i_imm(rd(loc + 4), lo12(v - P)));
				 break;
			case 23: wr(loc, u_imm(rd(loc), v - P));            break;  /* PCREL_HI20 */
			case 24:
			case 25: if (pcrel_hi(l, rs, t, S, &v) < 0)                 /* PCREL_LO12_I/S */
					 return -1;
				 wr(loc, type == 24 ? i_imm(rd(loc), lo12(v)) : s_imm(rd(loc), lo12(v)));
				 break;
			case 26: wr(loc, u_imm(rd(loc), v));                break;  /* HI20 */
			case 27: wr(loc, i_imm(rd(loc), lo12(v)));          break;  /* LO12_I */
			case 28: wr(loc, s_imm(rd(loc), lo12(v)));          break;  /* LO12_S */
			default:
				snprintf(l->err, sizeof(l->err), "unsupported relocation type %u", type);
				return -1;
			}
		}
	}
	return 0;
}

/* A defined global by name: its link address, and where it sits in the image. */
static uint32_t find(const struct apmu_link *l, const char *name, uint8_t **where)
{
	unsigned i;

	for (i = 0; i < l->nsym; i++) {
		const struct sym *s = &SYM(l)[i];

		if ((s->info >> 4) != STB_LOCAL && s->shndx && s->shndx < l->nsh &&
		    l->addr[s->shndx] && !strcmp(sym_name(l, s), name)) {
			if (where) {
				if (SH(l)[s->shndx].size < 4 || s->value > SH(l)[s->shndx].size - 4)
					return 0;
				*where = l->img[s->shndx] + s->value;
			}
			return l->addr[s->shndx] + s->value;
		}
	}
	return 0;
}

int apmu_link_place(struct apmu_link *l, uint32_t text_at, uint32_t data_at,
		    uint8_t *text, uint8_t *data, uint32_t id)
{
	uint8_t *p;
	unsigned i;

	for (i = 0; i < l->nsh; i++) {
		const struct shdr *s = &SH(l)[i];

		l->addr[i] = 0;
		l->img[i] = NULL;
		if (!(s->flags & SHF_ALLOC) || !s->size)
			continue;
		if (s->flags & SHF_EXECINSTR) {
			l->addr[i] = text_at + l->off[i];
			l->img[i] = text + l->off[i];
		} else {
			l->addr[i] = data_at + l->off[i];
			l->img[i] = data + l->off[i];
		}
		if (s->type != SHT_NOBITS)
			memcpy(l->img[i], l->obj + s->offset, s->size);
	}
	if (relocate(l) < 0)
		return -1;

	if (find(l, "component_id", &p))
		wr(p, id);
	l->bitmask = 0;
	if (find(l, "component_bitmask", &p))
		l->bitmask = rd(p);
	l->event = find(l, "component_event_handler", NULL);
	l->request = find(l, "component_request_handler", NULL);
	l->init = find(l, "init_hook", NULL);
	l->exit = find(l, "exit_hook", NULL);
	if (l->bitmask && !l->event)
		return lerr(l, "component_bitmask is set but component_event_handler is missing", NULL);
	return 0;
}
