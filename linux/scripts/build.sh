#!/usr/bin/env bash
# Build Linux + OpenSBI for AlSaqr from the bundled cva6-sdk sources.
# Reproduces the recipe documented in alsaqr-fpga/LINUX_BOOT.md, using:
#   - bundled rv64 elf toolchain     (for OpenSBI / bare-metal pieces)
#   - bundled host gcc wrapper       (relaxes GCC 15 strictness for host pkgs)
#   - buildroot's self-built cross   (riscv64-buildroot-linux-gnu-gcc, fetched
#                                     from upstream on first run)
#
# Outputs land in $LINUX_DIR/cva6-sdk/install64/.
# First build downloads ~8 GB and takes 30-60 min. Needs internet.
set -euo pipefail

# Resolve paths relative to this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ALSAQR_ROOT="$(cd "$LINUX_DIR/.." && pwd)"

# Pull in the bundled rv64 elf toolchain + openocd, exports $RISCV
if [ -z "${ALSAQR_ROOT_SOURCED:-}" ]; then
    # shellcheck disable=SC1091
    source "$ALSAQR_ROOT/source.sh" >/dev/null
fi

: "${TARGET_FREQ:=50000000}"
: "${NUM_HARTS:=4}"
export TARGET_FREQ NUM_HARTS

# Host gcc wrapper FIRST on PATH so buildroot host packages compile under GCC 15
export PATH="$LINUX_DIR/host-gcc-wrap/bin:$PATH"

# Sanity checks
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing host tool: $1" >&2; exit 1; }; }
for t in make dtc python3 cpio rsync bison flex; do need "$t"; done
[ -x /usr/bin/gcc ] || { echo "host /usr/bin/gcc not found" >&2; exit 1; }

cd "$LINUX_DIR/cva6-sdk"
echo "==> Building Linux + OpenSBI fw_payload (TARGET_FREQ=$TARGET_FREQ NUM_HARTS=$NUM_HARTS)"
echo "    bundled RISCV (elf): $RISCV"
echo "    host gcc wrapper:    $LINUX_DIR/host-gcc-wrap/bin/gcc -> /usr/bin/gcc"
echo "    output dir:          $LINUX_DIR/cva6-sdk/install64/"
# Target fw_payload.bin instead of images: images also wants uImage, which
# needs u-boot/tools/mkimage. u-boot is not in this bundle (not used for the
# JTAG boot path). fw_payload.bin is what gets loaded over GDB at 0x80000000.
exec make -j"$(nproc)" fw_payload.bin
