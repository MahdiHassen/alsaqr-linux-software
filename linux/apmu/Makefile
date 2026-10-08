# apmu userspace: libapmu + `apmu` (applications, through the apmu kernel
# module in kmod/) and apmuctl (debugging over /dev/mem, apmu_mmio).
#
# These are Linux userspace programs, so the bundled bare-metal
# riscv64-unknown-elf toolchain CANNOT build them (newlib has no mmap and no
# /dev/mem). A RISC-V *Linux* cross compiler is required -- same constraint and
# same search order as ../recv/Makefile.
#
# Normally you do not need to run this by hand: cva6-sdk/Makefile builds
# both programs and apmu.ko into the rootfs overlay (see ROOTFS_EXTRAS there).
#
# Use this Makefile to iterate without a 10-20 min image rebuild:
#     make            # build apmu, apmuctl, libapmu.a
#     make b64        # paste-to-console bootstrap for an already-booted target
# or push it over JTAG with ../../scripts/send_file.sh + `recv` (RUNBOOK §8).
#
# Override the compiler explicitly with:  make CC=/path/to/riscv64-linux-gnu-gcc

BR_CC  := $(wildcard ../cva6-sdk/buildroot/output/host/bin/riscv64-buildroot-linux-gnu-gcc)
SYS_CC := $(shell command -v riscv64-linux-gnu-gcc \
                   || command -v riscv64-unknown-linux-gnu-gcc \
                   || command -v riscv64-buildroot-linux-gnu-gcc 2>/dev/null)

# NOTE: plain `CC ?=` would not work -- make has a built-in default for CC
# ("cc"), so ?= never fires and we would silently build a host binary. Only
# auto-detect when CC is still that built-in default. (Same trap as ../recv.)
ifeq ($(origin CC),default)
CC := $(firstword $(BR_CC) $(SYS_CC))
endif

# Dynamic by default: ~15 KB instead of ~700 KB, which is what makes the `b64`
# serial-paste bootstrap practical. Safe with buildroot's own toolchain since
# the libc matches the rootfs by construction. Use STATIC=1 with an unrelated
# system cross compiler whose glibc may not match.
CFLAGS ?= -Wall -Wextra -O2 -std=gnu99 $(if $(STATIC),-static,)

# strip lives next to the compiler; derive it rather than hardcoding a prefix.
STRIP ?= $(patsubst %gcc,%strip,$(CC))

LIB_OBJS := libapmu.o
BINS     := apmu apmuctl
EXAMPLES := example/apmu_hello

.PHONY: all example clean distclean check b64 install test-queue test-link

all: $(BINS)

define need_cc
	@if [ -z "$(strip $(CC))" ]; then \
	  echo "error: no RISC-V Linux cross compiler found." >&2; \
	  echo "" >&2; \
	  echo "  The bundled toolchain/rv64 is bare-metal (riscv64-unknown-elf) and" >&2; \
	  echo "  cannot build Linux userspace. Either:" >&2; \
	  echo "    - run 'make images' in ../cva6-sdk (builds buildroot's toolchain), or" >&2; \
	  echo "    - install one and pass it:  make CC=riscv64-linux-gnu-gcc" >&2; \
	  exit 1; \
	fi
endef

%.o: %.c libapmu.h apmu_ioctl.h apmu_mmio.h apmu_regs.h apmu_abi.h
	$(need_cc)
	$(CC) $(CFLAGS) -c $< -o $@

libapmu.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

apmu: apmu.o libapmu.a
	$(need_cc)
	$(CC) $(CFLAGS) $^ -o $@
	@echo "[ok] built $@"

apmuctl: apmuctl.o apmu_mmio.o
	$(need_cc)
	$(CC) $(CFLAGS) $^ -o $@
	@echo "[ok] built $@"

example: $(EXAMPLES)

example/apmu_hello: example/apmu_hello.c apmu_mmio.o apmu_mmio.h apmu_regs.h
	$(need_cc)
	$(CC) $(CFLAGS) -I. $< apmu_mmio.o -o $@
	@echo "[ok] built $@"

# Build both flavours, stripped, ready to push to the board with
# ../../scripts/send_file.sh. Two of them because the DYNAMIC binary links
# against buildroot's current glibc and only runs on a rootfs built from the
# same tree (linux/built). The prebuilt 5.10.7 image in linux/prebuilt has an
# unrelated glibc, so use the static one there -- 412 KB instead of 26 KB, but
# it runs on any of them.
.PHONY: dist
dist:
	$(MAKE) clean
	$(MAKE) apmuctl
	$(STRIP) apmuctl
	rm -f *.o *.a
	$(MAKE) apmuctl STATIC=1
	$(STRIP) apmuctl
	mv apmuctl apmuctl-static
	rm -f *.o *.a
	$(MAKE) apmuctl
	$(STRIP) apmuctl
	@echo ""
	@ls -l apmuctl apmuctl-static
	@echo ""
	@echo "md5, to check against the target after transfer:"
	@md5sum apmuctl apmuctl-static 2>/dev/null || md5 apmuctl apmuctl-static

# Emit a paste-ready bootstrap block for the Linux serial console, to get
# apmuctl onto an ALREADY-BUILT rootfs without rerunning the image build. The
# target busybox has base64 but no networking applets and no dd. Keep the
# console in terminal mode; see RUNBOOK §5 about UART overrun on large pastes,
# and split the block into smaller `>>` appends if it garbles.
#
# Prefer scripts/send_file.sh + recv (RUNBOOK §8) when it is available -- it is
# far faster and does not depend on the UART surviving a big paste.
b64: apmuctl
	@echo "cat > /tmp/apmuctl.b64 <<'EOF'"
	@base64 < apmuctl
	@echo "EOF"
	@echo "base64 -d /tmp/apmuctl.b64 > /usr/bin/apmuctl && chmod +x /usr/bin/apmuctl && rm /tmp/apmuctl.b64"
	@echo "# then: apmuctl info"

# Syntax/warning check with the host compiler. Produces no usable binary --
# the RISC-V `fence iorw,iorw` in apmu_fence() will not assemble elsewhere --
# but catches ordinary mistakes with no cross toolchain present.
check:
	cc -Wall -Wextra -std=gnu99 -fsyntax-only -I. apmu_mmio.c apmuctl.c libapmu.c apmu.c example/apmu_hello.c \
	  && echo "[ok] sources compile clean"

install: apmu apmuctl
	install -D -m 0755 apmu $(DESTDIR)/usr/bin/apmu
	install -D -m 0755 apmuctl $(DESTDIR)/usr/bin/apmuctl

clean:
	rm -f $(BINS) $(EXAMPLES) *.o *.a
	@# apmuctl-static is a release artifact; `make distclean` removes it

distclean: clean
	rm -f apmuctl-static

# ------------------------------------------------------------------
# Offline loader test. Builds and runs NATIVELY on the development host (not
# the board): APMU_DEVMEM points apmu_mmio_open() at a sparse file instead of
# /dev/mem, so the raw-.bin load path -- placement, stride, verify, and the
# fact that loading leaves PMC_BOOT_ADDR alone -- is checked without hardware.
# Hardware behaviour (32-bit-only bus access, ordering, whether the PE actually
# executes) is not covered; `apmuctl bubble` is the on-board check for that.
.PHONY: test
test:
	rm -f /tmp/apmu_backing.bin
	python3 -c "open('/tmp/apmu_backing.bin','wb').truncate(0x10460000)"
	cc -Wall -Wextra -std=gnu99 -O1 -I. -Itest test/test_load.c apmu_mmio.c -o /tmp/apmu_test_load
	/tmp/apmu_test_load /tmp/apmu_backing.bin

# Host queue vs apmu-os's PE queue code on the same backing file.
APMU_OS ?= ../../../apmu-os
test-queue:
	rm -f /tmp/apmu_backing.bin
	python3 -c "open('/tmp/apmu_backing.bin','wb').truncate(0x10460000)"
	cc -Wall -Wextra -std=gnu99 -O1 -I. -I$(APMU_OS)/common -I$(APMU_OS)/common/include \
	  test/test_queue.c apmu_mmio.c -o /tmp/apmu_test_queue
	/tmp/apmu_test_queue /tmp/apmu_backing.bin

# kmod/apmu_link.c against ld.lld on every apmu-os component (in the build VM,
# after `make components` in apmu-os).
test-link:
	test/test_link.sh $(APMU_OS)
