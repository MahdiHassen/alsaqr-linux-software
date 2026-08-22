# Standalone build for recv (host->target file receiver).
#
# Normally you do not need this: cva6-sdk/Makefile builds recv into the rootfs
# overlay automatically as part of `make images` (see ROOTFS_EXTRAS there), so
# a from-source Linux build ships it at /usr/bin/recv.
#
# Use this Makefile when you want the binary on its own -- e.g. to copy onto an
# already-built rootfs, or to check it compiles without a full buildroot run.
#
# recv is Linux userspace, so the bundled bare-metal riscv64-unknown-elf
# toolchain CANNOT build it (newlib has no mmap/dev/mem). A Linux cross
# compiler is required. Search order:
#   1. buildroot's, if `make images` has been run
#   2. a system riscv64 Linux cross gcc
# Override explicitly with:  make CC=/path/to/riscv64-linux-gnu-gcc

BR_CC := $(wildcard ../cva6-sdk/buildroot/output/host/bin/riscv64-buildroot-linux-gnu-gcc)
SYS_CC := $(shell command -v riscv64-linux-gnu-gcc \
                   || command -v riscv64-unknown-linux-gnu-gcc \
                   || command -v riscv64-buildroot-linux-gnu-gcc 2>/dev/null)

# NOTE: plain `CC ?=` would not work here -- make has a built-in default for CC
# ("cc"), so ?= never fires and we would silently build a host binary. Only
# auto-detect when CC is still that built-in default.
ifeq ($(origin CC),default)
CC := $(firstword $(BR_CC) $(SYS_CC))
endif

# Default to a DYNAMIC binary: it comes out ~10 KB instead of ~700 KB, which is
# what makes the `make b64` serial-paste bootstrap practical. Safe when built
# with buildroot's own toolchain, since the libc matches by construction.
# Use STATIC=1 if you are building with an unrelated system cross compiler whose
# glibc may not match the target's.
CFLAGS ?= -Wall -Wextra -O2 $(if $(STATIC),-static,)

.PHONY: all clean check b64

all: recv

recv: recv.c
ifeq ($(strip $(CC)),)
	@echo "error: no RISC-V Linux cross compiler found." >&2
	@echo "" >&2
	@echo "  The bundled toolchain/rv64 is bare-metal (riscv64-unknown-elf) and" >&2
	@echo "  cannot build Linux userspace. Either:" >&2
	@echo "    - run 'make images' in ../cva6-sdk (builds buildroot's toolchain), or" >&2
	@echo "    - install one and pass it:  make CC=riscv64-linux-gnu-gcc" >&2
	@false
endif
	$(CC) $(CFLAGS) $< -o $@
	@echo "[ok] built $@ -- copy it into the target rootfs, or see ../cva6-sdk"

# Emit a paste-ready bootstrap block for the Linux serial console. This gets
# recv onto an ALREADY-BUILT rootfs without a 30-60 min `make images` rerun --
# the target busybox has base64 (CONFIG_BASE64=y) but no networking applets, so
# pasting is the only way in before recv itself exists.
#
# Paste the output into the console. Keep the console in VSCode's terminal mode
# or use screen; see RUNBOOK §5 about UART overrun on large pastes -- if it
# garbles, split the base64 block into a few smaller `>>` appends.
b64: recv
	@echo "cat > /tmp/recv.b64 <<'EOF'"
	@base64 < recv
	@echo "EOF"
	@echo "base64 -d /tmp/recv.b64 > /usr/bin/recv && chmod +x /usr/bin/recv && rm /tmp/recv.b64"
	@echo "# then: recv -w -o /tmp/out.bin"

# Syntax/warning check using the host compiler. Does not produce a usable
# binary, but catches mistakes without any cross toolchain present.
check:
	cc -Wall -Wextra -O2 -fsyntax-only recv.c && echo "[ok] recv.c compiles clean"

clean:
	rm -f recv
