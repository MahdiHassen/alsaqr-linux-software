#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (C) 2019 FORTH-ICS/CARV
#		Panagiotis Peristerakis <perister@ics.forth.gr>
#

HALF_FREQ=$(shell expr $(TARGET_FREQ) / 2)

ifndef TARGET_FREQ
  $(error TARGET_FREQ not set - please export TARGET FREQ in Hz)
endif

ifndef NUM_HARTS
  $(error NUM_HARTS not set - please export number of harts)
endif

ifeq ($(shell expr $(TARGET_FREQ) \>= 50000000), 1)
  TARGET_BAUDRATE := 115200
else ifeq ($(shell expr $(TARGET_FREQ) \>= 40000000), 1)
	TARGET_BAUDRATE := 38400
else
	TARGET_BAUDRATE := 9600
endif

ifndef MEM_SIZE
	ifdef USE_HYPER
		MEM_SIZE := 0x2000000
	else
		MEM_SIZE := 0x20000000
	endif
endif

platform-genflags-y += -DTARGET_FREQ=$(TARGET_FREQ) -DTARGET_BAUDRATE=$(TARGET_BAUDRATE) -DNUM_HARTS=$(NUM_HARTS)

# Compiler flags
platform-cppflags-y =
platform-cflags-y = -std=gnu11
platform-asflags-y =
platform-ldflags-y =

# Object to build
platform-objs-y += platform.o

PLATFORM_RISCV_XLEN = 64

# Blobs to build
FW_TEXT_START=0x80000000
FW_JUMP=n

ifeq ($(PLATFORM_RISCV_XLEN), 32)
 # This needs to be 4MB aligned for 32-bit support
 FW_JUMP_ADDR=0x80400000
 else
 # This needs to be 2MB aligned for 64-bit support
 FW_JUMP_ADDR=0x80200000
 endif
FW_JUMP_FDT_ADDR=0x82200000

# Firmware with payload configuration.
FW_PAYLOAD=y

ifeq ($(PLATFORM_RISCV_XLEN), 32)
# This needs to be 4MB aligned for 32-bit support
  FW_PAYLOAD_OFFSET=0x400000
else
# This needs to be 2MB aligned for 64-bit support
  FW_PAYLOAD_OFFSET=0x200000
endif
FW_PAYLOAD_FDT_ADDR=0x81800000
FW_PAYLOAD_ALIGN=0x1000

clean.dts:
	rm -f $(platform_src_dir)/fdt_gen/alsaqr.dts

alsaqr.dts: clean.dts
	cp $(platform_src_dir)/fdt_gen/alsaqr-template.dts $(platform_src_dir)/fdt_gen/alsaqr.dts
	python3 $(platform_src_dir)/fdt_gen/dts_gen.py $(platform_src_dir)/fdt_gen/alsaqr.dts $(NUM_HARTS) $(TARGET_FREQ) $(HALF_FREQ) $(TARGET_BAUDRATE) $(MEM_SIZE)

PHONY: clean.dts alsaqr.dts
