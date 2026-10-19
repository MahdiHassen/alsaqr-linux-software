# llcpart.ko: LLC way partitioning test (see llcpart.c). Builds against the bundle's kernel tree.
#   make                        build llcpart.ko
#   make install                scp it to the board (BOARD=root@10.42.0.2)
#   make run                    insmod it on the board and print the kernel log lines
#   make sweep                 latency vs working set for 16/8/4/2/1 ways -> llcpart_sweep.txt + llcpart_sweep.png
#   make clean
SDK   := $(abspath ..)/cva6-sdk
KDIR  ?= $(SDK)/buildroot/output/build/linux-v6.1.183
HOSTBIN ?= $(SDK)/buildroot/output/host/bin
BOARD ?= root@10.42.0.2
SSH   := ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
PARAMS ?=

obj-m := llcpart.o

all:
	PATH=$(HOSTBIN):$$PATH $(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=riscv CROSS_COMPILE=riscv64-buildroot-linux-gnu- modules

clean:
	PATH=$(HOSTBIN):$$PATH $(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=riscv clean
	rm -f insmodp

install: all insmodp
	scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR llcpart.ko insmodp $(BOARD):/root/

# busybox's small insmod drops "param=value" arguments: a 20-line static insmod that passes them (finit_module)
insmodp: insmodp.c
	PATH=$(HOSTBIN):$$PATH riscv64-buildroot-linux-gnu-gcc -O2 -static -o $@ $<

run: install
	$(SSH) $(BOARD) 'rmmod llcpart 2>/dev/null; dmesg -c >/dev/null; /root/insmodp /root/llcpart.ko $(PARAMS) && dmesg | grep llcpart'

sweep: install
	$(SSH) $(BOARD) 'rmmod llcpart 2>/dev/null; dmesg -c >/dev/null; /root/insmodp /root/llcpart.ko sweep=1 $(PARAMS) && dmesg | grep "llcpart: sweep"' | tee llcpart_sweep.txt
	python3 plot_sweep.py llcpart_sweep.txt llcpart_sweep.png

.PHONY: all clean install run sweep
