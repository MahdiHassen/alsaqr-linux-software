APP := pmu_bench
XLEN:=64

export SW_HOME := $(shell cd .. && pwd)
include $(shell pwd)/../common.mk

# PMU firmware paths (RV32 Ibex core firmware embedded into the CVA6 binary)
PMU_FW_DIR   := pmu_firmware
PMU_BUILD_DIR := $(PMU_FW_DIR)/build
PMU_TEXT_BIN  := $(PMU_BUILD_DIR)/text_section.bin
PMU_DATA_BIN  := $(PMU_BUILD_DIR)/data_rodata_bss.bin

BENCH_INCLUDES_BASE := \
	-I$(shell pwd)

# ==============================================================================
# PMU Firmware 
# ==============================================================================

.PHONY: pmu_firmware
pmu_firmware:
	$(MAKE) -C $(PMU_FW_DIR) all generate-bin-files

pmu_text.o: pmu_firmware
	cp $(PMU_TEXT_BIN) text_section.bin
	$(RISCV_GCC) -c pmu_text_wrapper.S -o $@

pmu_data.o: pmu_firmware
	cp $(PMU_DATA_BIN) data_rodata_bss.bin
	$(RISCV_GCC) -c pmu_data_wrapper.S -o $@

# ==============================================================================
# CVA6 Firmware
# ==============================================================================
pmu_bench: pmu_text.o pmu_data.o
	$(RISCV_GCC) $(RISCV_FLAGS) $(BENCH_INCLUDES_BASE) \
		-Driscv -Dsim -DBARE_METAL \
		-T test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c bare_malloc.c bare_math.c \
		pmu_text.o pmu_data.o \
		$(SRC) \
		-o pmu_bench.riscv

clean:
	rm -f pmu_bench.riscv $(APP).dump *.slm
	$(MAKE) -C $(PMU_FW_DIR) clean
	rm -f pmu_text.o pmu_data.o text_section.bin data_rodata_bss.bin
