APP := fat_test
XLEN:=64

export SW_HOME := $(shell cd .. && pwd)
include $(shell pwd)/../common.mk

# PMU firmware paths (RV32 Ibex core firmware embedded into the CVA6 binary)
PMU_FW_DIR   := pmu_firmware
PMU_BUILD_DIR := $(PMU_FW_DIR)/build
PMU_TEXT_BIN  := $(PMU_BUILD_DIR)/text_section.bin
PMU_DATA_BIN  := $(PMU_BUILD_DIR)/data_rodata_bss.bin

OBJCOPY ?= riscv64-unknown-elf-objcopy

SDVB_DIR    := sdvb
DISP_DIR    := $(SDVB_DIR)/benchmarks/disparity/src/c
COMMON_DIR  := $(SDVB_DIR)/common/c

# Disparity benchmark sources
DISP_BENCH_SRC := \
	$(DISP_DIR)/script_disparity.c \
	$(DISP_DIR)/computeSAD.c \
	$(DISP_DIR)/correlateSAD_2D.c \
	$(DISP_DIR)/finalSAD.c \
	$(DISP_DIR)/findDisparity.c \
	$(DISP_DIR)/getDisparity.c \
	$(DISP_DIR)/integralImage2D2D.c \
	$(DISP_DIR)/padarray2.c \
	$(DISP_DIR)/padarray4.c

# SDVB common sources
DISP_COMMON_SRC := \
	$(COMMON_DIR)/readImage.c \
	$(COMMON_DIR)/iMallocHandle.c \
	$(COMMON_DIR)/fMallocHandle.c \
	$(COMMON_DIR)/uiMallocHandle.c \
	$(COMMON_DIR)/iFreeHandle.c \
	$(COMMON_DIR)/fFreeHandle.c \
	$(COMMON_DIR)/uiFreeHandle.c \
	$(COMMON_DIR)/iSetArray.c \
	$(COMMON_DIR)/fSetArray.c \
	$(COMMON_DIR)/selfCheck.c \
	$(COMMON_DIR)/photonStartTiming.c \
	$(COMMON_DIR)/photonEndTiming.c \
	$(COMMON_DIR)/photonReportTiming.c \
	$(COMMON_DIR)/photonPrintTiming.c

# Our bare-metal support
BARE_SRC := fat16.c fat_file.c bare_malloc.c

# Common include flags
BENCH_INCLUDES_BASE := \
	-I$(shell pwd) \
	-I$(shell pwd)/$(COMMON_DIR)

DISP_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(DISP_DIR)

# ==============================================================================
# PMU Firmware build + embedding
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
# computeSAD profiler: disparity on core 0, no interference
# ==============================================================================
fat_image.o:
	$(RISCV_GCC) -c fat_image_wrapper.S -o $@

pmu_bench: pmu_text.o pmu_data.o fat_image.o
	$(RISCV_GCC) $(RISCV_FLAGS) $(DISP_INCLUDES) \
		-Driscv -Dsim -DBARE_METAL \
		-T test_sim.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c $(BARE_SRC) \
		pmu_text.o pmu_data.o fat_image.o \
		$(DISP_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o pmu_bench.riscv

clean:
	rm -f $(APP).riscv disp_test.riscv pmu_bench.riscv $(APP).dump *.slm
	$(MAKE) -C $(PMU_FW_DIR) clean
	rm -f pmu_text.o pmu_data.o fat_image.o text_section.bin data_rodata_bss.bin
