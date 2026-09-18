# pmu_mempol_synth_sim: the pmu_mempol_synth benchmark configured for the he-soc xsim RTL simulation.
#
# Same sources as ../pmu_mempol_synth (nothing is copied); the differences are compile-time:
#   -DXSIM             wake cores 2 and 3 (the simulator resumes only core 0 over JTAG, the boot ROM parks
#                      cores 2/3 in wfi until a PLIC interrupt reaches them) and drop the 15 s wall-clock delay
#                      before the mempol log dump (hours of simulated time)
#   WORK_READ_LINES    cache lines primed per core: 1024 (64 KB, covers the 768 lines each stage touches) instead of 4096
#   STAGE_ITERATIONS   4 per stage instead of 10 (the LLC / DRAM / LLC sequence is unchanged)
#   MAX_IRQ_LOG        256 interrupt log entries per core instead of 5000 (zeroing the table dominates startup)
#   XSIM_LOG_CAP       at most 100 mempol log lines are printed
# Simulated time is in the tens of milliseconds, i.e. tens of minutes of wall clock with xsim.
#
# Build:  make pmu_bench            (or from alsaqr-mcp: sw_build test=<this dir> target=pmu_bench)
# Run:    alsaqr-mcp sim_run binary=pmu_bench.riscv
APP := pmu_bench
XLEN := 64

export SW_HOME := $(shell cd .. && pwd)
include $(shell pwd)/../common.mk

ORIG          := ../pmu_mempol_synth
PMU_FW_DIR    := $(ORIG)/pmu_firmware
PMU_BUILD_DIR := $(PMU_FW_DIR)/build
PMU_TEXT_BIN  := $(PMU_BUILD_DIR)/text_section.bin
PMU_DATA_BIN  := $(PMU_BUILD_DIR)/data_rodata_bss.bin

SIM_DEFINES := -DXSIM -DWORK_READ_LINES=1024 -DSTAGE_ITERATIONS=4 -DMAX_IRQ_LOG=256 -DXSIM_LOG_CAP=100

.PHONY: pmu_firmware
pmu_firmware:
	$(MAKE) -C $(PMU_FW_DIR) all generate-bin-files

# the wrappers .incbin the firmware images from the current directory
pmu_text.o: pmu_firmware
	cp $(PMU_TEXT_BIN) text_section.bin
	$(RISCV_GCC) -c $(ORIG)/pmu_text_wrapper.S -o $@

pmu_data.o: pmu_firmware
	cp $(PMU_DATA_BIN) data_rodata_bss.bin
	$(RISCV_GCC) -c $(ORIG)/pmu_data_wrapper.S -o $@

pmu_bench: pmu_text.o pmu_data.o
	$(RISCV_GCC) $(RISCV_FLAGS) -I$(ORIG) \
		-Driscv -Dsim -DBARE_METAL $(SIM_DEFINES) \
		-T $(ORIG)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		$(ORIG)/pmu_bench.c $(ORIG)/bare_malloc.c $(ORIG)/bare_math.c \
		pmu_text.o pmu_data.o \
		$(SRC) \
		-o $(APP).riscv

build: pmu_bench

clean:
	rm -f $(APP).riscv $(APP).dump *.slm pmu_text.o pmu_data.o text_section.bin data_rodata_bss.bin
	$(MAKE) -C $(PMU_FW_DIR) clean
