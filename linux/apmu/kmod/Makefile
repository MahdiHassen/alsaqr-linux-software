# apmu.ko. Build against the configured kernel tree:
#   make KDIR=<linux build dir> CROSS_COMPILE=<prefix>
obj-m := apmu.o
apmu-y := apmu_main.o apmu_link.o

ifeq ($(KERNELRELEASE),)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=riscv modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=riscv clean
endif
