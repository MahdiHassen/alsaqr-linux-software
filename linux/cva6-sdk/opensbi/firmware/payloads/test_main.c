/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_string.h>

struct sbiret {
	unsigned long error;
	unsigned long value;
};

struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
			unsigned long arg1, unsigned long arg2,
			unsigned long arg3, unsigned long arg4,
			unsigned long arg5)
{
	struct sbiret ret;

	register unsigned long a0 asm ("a0") = (unsigned long)(arg0);
	register unsigned long a1 asm ("a1") = (unsigned long)(arg1);
	register unsigned long a2 asm ("a2") = (unsigned long)(arg2);
	register unsigned long a3 asm ("a3") = (unsigned long)(arg3);
	register unsigned long a4 asm ("a4") = (unsigned long)(arg4);
	register unsigned long a5 asm ("a5") = (unsigned long)(arg5);
	register unsigned long a6 asm ("a6") = (unsigned long)(fid);
	register unsigned long a7 asm ("a7") = (unsigned long)(ext);
	asm volatile ("ecall"
		      : "+r" (a0), "+r" (a1)
		      : "r" (a2), "r" (a3), "r" (a4), "r" (a5), "r" (a6), "r" (a7)
		      : "memory");
	ret.error = a0;
	ret.value = a1;

	return ret;
}

static inline void sbi_ecall_console_puts(const char *str)
{
	sbi_ecall(SBI_EXT_DBCN, SBI_EXT_DBCN_CONSOLE_WRITE,
		  sbi_strlen(str), (unsigned long)str, 0, 0, 0, 0);
}

#define wfi()                                             \
	do {                                              \
		__asm__ __volatile__("wfi" ::: "memory"); \
	} while (0)


#define read_csr(reg) ({ unsigned long __tmp;    \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })

char buffer[1024 * 1024];

void sweep(int stride)
{
  long icachemiss, dcachemiss;
  int max_j = 4 * 1024;
  icachemiss = read_csr(0xc03);
  dcachemiss = read_csr(0xc04);

  for(int i = 0; i < 10; i++)
  {
    if(i == 1)
    {
      icachemiss = read_csr(0xc03);
      dcachemiss = read_csr(0xc04);
    }

    for(int j = 0; j < max_j; j += 4)
    {
      buffer[(j + 0) * stride] = 0;
      buffer[(j + 1) * stride] = 0;
      buffer[(j + 2) * stride] = 0;
      buffer[(j + 3) * stride] = 0;
    }
  }

  icachemiss = icachemiss - read_csr(0xc03);
  dcachemiss = dcachemiss - read_csr(0xc04);

  if( (icachemiss==0) && (dcachemiss==0) )
    sbi_ecall_console_puts("Both zeros\n");
  else
    sbi_ecall_console_puts("Not zeros\n");

}


void test_main(unsigned long a0, unsigned long a1)
{
	sbi_ecall_console_puts("\nTest payload running\n");

  sweep(0);
  sweep(1);
  sweep(2);
  sweep(4);
  sweep(8);
  sweep(16);

  while (1)
		wfi();
}
