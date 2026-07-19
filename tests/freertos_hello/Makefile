RISCV_XLEN ?= 64
RISCV_LIB  ?= elf

TARGET=riscv${RISCV_XLEN}-unknown-${RISCV_LIB}
#-----------------------------------------------------------
GCC		= $(TARGET)-gcc
OBJCOPY	= $(TARGET)-objcopy
OBJDUMP	= $(TARGET)-objdump
AR		= $(TARGET)-ar

GCCVER 	= $(shell $(GCC) --version | grep gcc | cut -d" " -f9)

#64-bit
#RISCV ?= ~/riscv_fpga  
#32-bit
#RISCV ?= ~/riscv
RISCV_CCPATH = ${CCPATH}

BUILD_DIR       = build
FREERTOS_SOURCE_DIR = $(abspath ./FreeRTOS/Source)
DEMO_SOURCE_DIR = $(abspath ./FreeRTOS/Demo/Common/Minimal)

INCLUDES = \
	-I. \
	-I$(FREERTOS_SOURCE_DIR)/include \
	-I$(FREERTOS_SOURCE_DIR)/../Demo/Common/include \
	-I$(FREERTOS_SOURCE_DIR)/portable/GCC/RISC-V \
	-I./uart \
	$(addprefix -I, $(INC_DIRS))
#-----------------------------------------------------------


# cva6
CPPFLAGS = 

CFLAGS = \
	-Wall \
	$(INCLUDES) \
	-fomit-frame-pointer \
	-fno-strict-aliasing \
	-fno-builtin-printf \
	-D__gracefulExit \
	-mcmodel=medany \
	-static \
	-DRISCV \
	-I $(FREERTOS_SOURCE_DIR)/portable/GCC/RISC-V/chip_specific_extensions/RISCV_MTIME_CLINT_no_extensions


LDFLAGS	 = -T link.ld \
			-nostartfiles \
			-static \
			-fno-builtin-printf \
			-Xlinker --defsym=__stack_size=300 
			#-Xlinker -Map=RTOSDemo.map # output file

LIBS	 = -L$(RISCV_CCPATH)/lib/gcc/$(TARGET)/$(GCCVER) \
		   -L$(RISCV_CCPATH)/$(TARGET)/lib \
		   -lc \
		   -lgcc


DEBUG = 1
ifeq ($(DEBUG), 1)
    CFLAGS += -O0 -ggdb3
else
    CFLAGS += -O2
endif

## FPGA build flag — mirrors software/common.mk. `make fpga=1` defines
## FPGA_EMULATION for the FPGA UART/clock paths. (he-soc FPGA: mtime @ 10 MHz,
## already hardcoded in hw_platform.h.)
ifdef fpga
    CFLAGS += -DFPGA_EMULATION
endif

#-----------------------------------------------------------

FREERTOS_SRC = \
	$(FREERTOS_SOURCE_DIR)/croutine.c \
	$(FREERTOS_SOURCE_DIR)/event_groups.c \
	$(FREERTOS_SOURCE_DIR)/list.c \
	$(FREERTOS_SOURCE_DIR)/queue.c \
	$(FREERTOS_SOURCE_DIR)/stream_buffer.c \
	$(FREERTOS_SOURCE_DIR)/tasks.c \
	$(FREERTOS_SOURCE_DIR)/timers.c \
	$(FREERTOS_SOURCE_DIR)/portable/MemMang/heap_4.c \
	$(FREERTOS_SOURCE_DIR)/portable/GCC/RISC-V/port.c


DEMO_SRC =

ROOT_DIR:=$(realpath .)

# ---- SDVB disparity (QCIF) workload + bare-metal FAT16 ----
SDVB_DIR    = sdvb
DISP_DIR    = $(SDVB_DIR)/benchmarks/disparity/src/c
COMMON_DIR  = $(SDVB_DIR)/common/c

# sdvb_bench MUST precede common/c so its bare sdvbs_common.h overrides the original.
INC_DIRS += sdvb_bench $(COMMON_DIR) $(DISP_DIR)

# SDVB sources require these defines (sim => WIN_SZ=4/SHIFT=8 in disparity).
CFLAGS += -Dsim -Driscv -DBARE_METAL

SDVB_SRC = \
	sdvb_bench/fat16.c \
	sdvb_bench/fat_file.c \
	sdvb_bench/bare_malloc.c \
	sdvb_bench/bare_stubs.c \
	$(DISP_DIR)/script_disparity.c \
	$(DISP_DIR)/computeSAD.c \
	$(DISP_DIR)/correlateSAD_2D.c \
	$(DISP_DIR)/finalSAD.c \
	$(DISP_DIR)/findDisparity.c \
	$(DISP_DIR)/getDisparity.c \
	$(DISP_DIR)/integralImage2D2D.c \
	$(DISP_DIR)/padarray2.c \
	$(DISP_DIR)/padarray4.c \
	$(COMMON_DIR)/readImage.c \
	$(COMMON_DIR)/iMallocHandle.c \
	$(COMMON_DIR)/fMallocHandle.c \
	$(COMMON_DIR)/uiMallocHandle.c \
	$(COMMON_DIR)/iFreeHandle.c \
	$(COMMON_DIR)/fFreeHandle.c \
	$(COMMON_DIR)/uiFreeHandle.c \
	$(COMMON_DIR)/iSetArray.c \
	$(COMMON_DIR)/fSetArray.c \
	$(COMMON_DIR)/photonStartTiming.c \
	$(COMMON_DIR)/photonEndTiming.c \
	$(COMMON_DIR)/photonReportTiming.c \
	$(COMMON_DIR)/photonPrintTiming.c

ASM_SRC = start.S \
		  vector.S \
		  sdvb_bench/fat_image_wrapper.S \
	      $(FREERTOS_SOURCE_DIR)/portable/GCC/RISC-V/portASM.S

APP_SRC = \
	main.c \
	riscv-virt.c \
	uart/uart.c \
	$(SDVB_SRC)
	

OBJS = $(FREERTOS_SRC:%.c=$(BUILD_DIR)/%.o) \
	   $(DEMO_SRC:%.c=$(BUILD_DIR)/%.o) \
	   $(ASM_SRC:%.S=$(BUILD_DIR)/%.o) \
	   $(APP_SRC:%.c=$(BUILD_DIR)/%.o)

DEPS = $(FREERTOS_SRC:%.c=$(BUILD_DIR)/%.d) \
		$(DEMO_SRC:%.c=$(BUILD_DIR)/%.d) \
		$(ASM_SRC:%.S=$(BUILD_DIR)/%.d) \
		$(APP_SRC:%.c=$(BUILD_DIR)/%.d)

$(BUILD_DIR)/RTOSDemo.elf: $(OBJS) link.ld Makefile
	$(GCC) $(LDFLAGS) $(LIBS) $(OBJS) -o $@ -lc -lm

$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p $(@D)
	$(GCC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S Makefile
	@mkdir -p $(@D)
	$(GCC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

dis:
	$(TARGET)-objdump -d $(BUILD_DIR)/RTOSDemo.elf > RTOSDemo.asm

-include $(DEPS)

