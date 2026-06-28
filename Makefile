# AlSaqr software bundle — top-level Makefile
#
# Ported from alsaqr-fpga/Makefile and adapted to this self-contained bundle:
#   - OpenOCD, GDB and the RISC-V toolchain are the binaries shipped under
#     openocd/ and toolchain/ (no system installs assumed).
#   - The CVA6 SDK lives in linux/cva6-sdk and prebuilt Linux artifacts in
#     linux/prebuilt, so `make gdb-load-payload` works out of the box.
#   - All paths are resolved relative to this Makefile, so the bundle stays
#     relocatable — move it anywhere and `make` still works.
#
# Typical flow (mirrors alsaqr-fpga):
#   make load_bitstream BITSTREAM=bitstreams/alsaqr_xilinx_48.bit
#   make openocd                     # term 1: GDB server on :6699
#   make gdb-load-payload            # term 2: upload DTB + fw_payload.elf, run

# Bundle root = directory containing this Makefile (relocatable)
ROOT := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))

# Define default variables (bundled binaries)
OPENOCD         ?= $(ROOT)/openocd/bin/openocd
OPENOCD_SCRIPTS ?= $(ROOT)/openocd/tcl
GDB             ?= $(ROOT)/toolchain/rv64/bin/riscv64-unknown-elf-gdb
ADAPTER_SPEED   ?= 2000
INTERFACE       ?= ftdi
DEVICE          ?= olimex-arm-usb-ocd-h
ID              ?=
TARGET          ?= smp

# OpenOCD ports
GDB_PORT    ?= 6699
TCL_PORT    ?= disabled
TELNET_PORT ?= disabled

export OPENOCD_SCRIPTS

SCRIPTS_DIR   ?= $(ROOT)/scripts

# Vivado variables (bitstream programming delegates to scripts/load_bitstream.sh)
VIVADO        ?=
BITSTREAM     ?=
HW_TARGET     ?=

# CVA6 SDK variables. Default payload/DTB point at the shipped prebuilt
# artifacts so the boot path works without a 30-60 min Linux build. Build them
# yourself with `make images` (outputs land in $(IMAGES_DIR)).
CVA6_SDK_DIR  ?= $(ROOT)/linux/cva6-sdk
IMAGES_DIR    ?= $(CVA6_SDK_DIR)/install64
PREBUILT_DIR  ?= $(ROOT)/linux/prebuilt
PAYLOAD       ?= $(PREBUILT_DIR)/fw_payload.elf
DTB_FILE      ?= $(PREBUILT_DIR)/alsaqr.dtb
DTB_ADDR      ?= 0x81800000

MEM_BASE_ADDR ?= 0x80000000

# Set NUM_HARTS and TARGET_FREQ
NUM_HARTS     ?= 4
TARGET_FREQ   ?= 50000000 # Hz

export NUM_HARTS TARGET_FREQ

# OpenOCD directory (contains openocd.cfg + target/) and vendored .so deps
OPENOCD_DIR   ?= $(ROOT)/openocd
OPENOCD_LIB   ?= $(ROOT)/openocd/lib

OPENOCD_DEPS :=
GDB_DEPS     := $(PAYLOAD)

# Initialize OpenOCD command arguments
OPENOCD_ARGS  = -s "$(OPENOCD_DIR)"

# Initialize OpenOCD commands
OPENOCD_CMDS   = -c "adapter speed $(ADAPTER_SPEED)" \
                 -c "set _INTERFACE $(INTERFACE)" \
                 -c "set _DEVICE $(DEVICE)" \
                 -c "gdb_port $(GDB_PORT)" \
                 -c "tcl_port $(TCL_PORT)" \
                 -c "telnet_port $(TELNET_PORT)"
ifneq ($(ID), )
	OPENOCD_CMDS +=  -c "adapter serial $(ID)"
endif

OPENOCD_CMDS  += -c "set _NUM_CORES $(NUM_HARTS)" \
                 -c "set _TARGET $(TARGET)" \
                 -f "openocd.cfg" \
                 -c "init" \
                 -c "reset halt"

# Use LLC in SPM mode (experimental)
ifeq ($(LLC_SPM), 1)
	OPENOCD_CMDS += -c "mww 0x10401000 0xffffffff"
	OPENOCD_CMDS += -c "mww 0x10401004 0xffffffff"
	OPENOCD_CMDS += -c "mww 0x10401010 0x1"
endif

# Load the DTB only when booting the fw_payload (matches alsaqr-fpga behaviour)
LOAD_DTB ?= $(if $(filter fw_payload.elf,$(notdir $(PAYLOAD))),1,)
ifeq ($(LOAD_DTB),1)
	GDB_DEPS += $(DTB_FILE)
endif

# Append OpenOCD commands to arguments
OPENOCD_ARGS += $(OPENOCD_CMDS)

.DEFAULT_GOAL := help

.PHONY: help
help:
	@echo "AlSaqr software bundle — make targets:"
	@echo "  make openocd                          start OpenOCD GDB server on :$(GDB_PORT)"
	@echo "  make gdb                              attach GDB to a running server"
	@echo "  make gdb-load-payload                upload DTB + payload and boot Linux"
	@echo "  make load_bitstream BITSTREAM=<.bit>  program a bitstream onto the FPGA"
	@echo "  make images                          build Linux + OpenSBI fw_payload"
	@echo "  make dtb                             build the device tree blob"
	@echo "  make clean | deep-clean              remove build artifacts"
	@echo "  make init                            verify the bundle is complete"
	@echo ""
	@echo "Key knobs: PAYLOAD=$(PAYLOAD)"
	@echo "           DTB_FILE=$(DTB_FILE)"
	@echo "           NUM_HARTS=$(NUM_HARTS) TARGET=$(TARGET) GDB_PORT=$(GDB_PORT)"

# Build Linux + OpenSBI fw_payload via the bundle's build script. Outputs land
# in $(IMAGES_DIR) (= linux/cva6-sdk/install64). Point PAYLOAD there to boot it.
.PHONY: images
images: $(IMAGES_DIR)/fw_payload.elf

$(IMAGES_DIR)/fw_payload.elf:
	$(ROOT)/linux/scripts/build.sh

# Build the device tree blob in the CVA6 SDK (from alsaqr.dts via dtc)
.PHONY: dtb
dtb:
	$(MAKE) -C $(CVA6_SDK_DIR) alsaqr.dtb

# Program a bitstream onto the connected FPGA. Delegates to the bundle's
# load_bitstream.sh, which sources Vivado and auto-detects the JTAG target.
.PHONY: load_bitstream load_bistream
load_bitstream:
	$(if $(VIVADO),VIVADO=$(VIVADO)) $(if $(HW_TARGET),HW_TARGET=$(HW_TARGET)) \
		$(SCRIPTS_DIR)/load_bitstream.sh $(BITSTREAM)
# Alias kept for parity with alsaqr-fpga's (misspelled) target name
load_bistream: load_bitstream

# Run OpenOCD to set up a GDB server
.PHONY: openocd
openocd: $(OPENOCD_DEPS)
	@echo "Interface      : $(INTERFACE)"
	@echo "Device         : $(DEVICE)"
	@echo "ID             : $(ID)"
	@echo "Number of cores: $(NUM_HARTS)"
	@echo "Target         : $(TARGET)"
	LD_LIBRARY_PATH="$(OPENOCD_LIB):$$LD_LIBRARY_PATH" \
		$(OPENOCD) $(OPENOCD_ARGS) -c "echo \"Ready for Remote Connections\""

# Run GDB to load the payload and execute it
.PHONY: gdb gdb-load-payload
gdb:
	$(GDB) \
	-ex "target extended-remote :$(GDB_PORT)"
gdb-load-payload: $(GDB_DEPS)
	$(GDB) $(PAYLOAD) \
	-ex "target extended-remote :$(GDB_PORT)" \
	$(if $(filter 1,$(LOAD_DTB)), \
		-ex "monitor load_image $(DTB_FILE) $(DTB_ADDR)",) \
	$(foreach i, $(shell seq 1 $(NUM_HARTS)), -ex "thread $(i)" -ex "set \$$pc=_start" -ex "info registers pc") \
	-ex "load" \
	-ex "continue"

# Verify the bundle is complete (replaces alsaqr-fpga's submodule init —
# this bundle vendors everything, so nothing is fetched).
.PHONY: init
init:
	@ok=1; \
	for f in "$(OPENOCD)" "$(GDB)" "$(PAYLOAD)" "$(DTB_FILE)"; do \
		if [ -e "$$f" ]; then echo "ok   $$f"; else echo "MISSING $$f"; ok=0; fi; \
	done; \
	[ $$ok -eq 1 ] && echo "Bundle looks complete." || { echo "Bundle is incomplete."; exit 1; }

.PHONY: clean deep-clean

# Only remove rebuildable build outputs. Shipped prebuilt artifacts under
# linux/prebuilt and the vendored DTB are left untouched.
clean:
	-$(MAKE) -C $(CVA6_SDK_DIR) clean
	-$(MAKE) -C $(CVA6_SDK_DIR)/opensbi clean
	rm -rf $(IMAGES_DIR)

deep-clean: clean
	-$(MAKE) -C $(CVA6_SDK_DIR)/buildroot clean
