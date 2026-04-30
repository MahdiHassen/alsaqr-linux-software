# source ./source.sh   (must be sourced, not executed)
#
# Sets up env to build & run AlSaqr SoC test programs from this bundle.
# All paths are resolved relative to this script's location, so the bundle
# is fully relocatable — move it anywhere and re-source.
#
# Shipped binaries (toolchain, OpenOCD) are x86_64 Linux. On other platforms,
# build the toolchain and OpenOCD from source and symlink into toolchain/ and
# openocd/bin/.

# Resolve bundle root (handles both bash and zsh)
if [ -n "${BASH_SOURCE:-}" ]; then
    _SOURCE="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
    _SOURCE="${(%):-%x}"
else
    echo "source.sh: please source from bash or zsh" >&2
    return 1 2>/dev/null || exit 1
fi
export ALSAQR_ROOT="$(cd "$(dirname "$_SOURCE")" && pwd)"

# RV64 toolchain (CVA6 target)
export RISCV="$ALSAQR_ROOT/toolchain/rv64"
export PATH="$RISCV/bin:$PATH"

# RV32 toolchain (Ibex PMU firmware)
export PATH="$ALSAQR_ROOT/toolchain/rv32/bin:$PATH"

# riscv-none-elf-* shim (PMU firmware Makefiles expect this naming)
export PATH="$ALSAQR_ROOT/toolchain/shim:$PATH"

# OpenOCD binary + tcl scripts + vendored shared libs
export PATH="$ALSAQR_ROOT/openocd/bin:$PATH"
export OPENOCD_SCRIPTS="$ALSAQR_ROOT/openocd/tcl"
export LD_LIBRARY_PATH="$ALSAQR_ROOT/openocd/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Vendored Python 3.12 runtime (GDB was built against libpython3.12; ship it
# so hosts without python3.12 can still run gdb without installing anything).
# PYTHONHOME is *not* exported here — it would break the system python3 if the
# host has a different version. Instead, the gdb wrapper sets it locally.
export LD_LIBRARY_PATH="$ALSAQR_ROOT/toolchain/python-runtime/lib:$LD_LIBRARY_PATH"

# GDB (used by load.sh)
export GDB_BIN="$RISCV/bin/riscv64-unknown-elf-gdb"

# Bundle paths exposed to test Makefiles
export ALSAQR_TESTS="$ALSAQR_ROOT/tests"
export ALSAQR_INC="$ALSAQR_ROOT/inc"

# Convenience: also expose SW_HOME so the existing common.mk works unchanged
export SW_HOME="$ALSAQR_ROOT/tests"

# Sanity check
if [ ! -x "$RISCV/bin/riscv64-unknown-elf-gcc" ]; then
    echo "WARN: $RISCV/bin/riscv64-unknown-elf-gcc not found." >&2
    echo "      The bundle is missing the RV64 toolchain." >&2
fi
if [ ! -x "$ALSAQR_ROOT/toolchain/rv32/bin/riscv32-unknown-elf-gcc" ]; then
    echo "WARN: RV32 toolchain not found at $ALSAQR_ROOT/toolchain/rv32/" >&2
fi
if [ ! -x "$ALSAQR_ROOT/openocd/bin/openocd" ]; then
    echo "WARN: openocd not found at $ALSAQR_ROOT/openocd/bin/openocd" >&2
fi

# Vendored pyelftools (used by tests/inc/scripts/stim_utils.py and similar).
# Append rather than prepend so a system install still wins if present.
export PYTHONPATH="${PYTHONPATH:+$PYTHONPATH:}$ALSAQR_ROOT/scripts/vendor"

echo "AlSaqr environment ready."
echo "  ALSAQR_ROOT=$ALSAQR_ROOT"
echo "  RISCV=$RISCV"
echo "  Tests:   $ALSAQR_TESTS"
echo "  OpenOCD: $ALSAQR_ROOT/openocd/bin/openocd"
unset _SOURCE
