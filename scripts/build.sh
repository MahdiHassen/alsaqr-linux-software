#!/usr/bin/env bash
# build.sh — wrapper to build a test in tests/<name>/
#
# Usage:
#   ./scripts/build.sh tests/pmu_mempol_synth        # default target
#   ./scripts/build.sh tests/pmu_mempol_synth pmu_bench
#   ./scripts/build.sh tests/pmu_sdvb pmu_bench
#
# Always passes fpga=1 (FPGA build). Set FPGA=0 to skip.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALSAQR_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Verify env is sourced
if ! command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    echo "[*] Sourcing $ALSAQR_ROOT/source.sh…"
    # shellcheck disable=SC1091
    source "$ALSAQR_ROOT/source.sh"
fi

TEST_DIR="${1:-}"
TARGET="${2:-pmu_bench}"
FPGA="${FPGA:-1}"

if [[ -z "$TEST_DIR" ]]; then
    echo "Usage: $0 <test_dir> [target]" >&2
    echo "Example: $0 tests/pmu_mempol_synth pmu_bench" >&2
    exit 1
fi

# Resolve relative to bundle root if not absolute
if [[ "$TEST_DIR" != /* ]]; then
    TEST_DIR="$ALSAQR_ROOT/$TEST_DIR"
fi
if [[ ! -d "$TEST_DIR" ]]; then
    echo "Error: test dir not found: $TEST_DIR" >&2
    exit 1
fi

echo "[*] Building $TARGET in $TEST_DIR (fpga=$FPGA)"
make -C "$TEST_DIR" clean "$TARGET" fpga="$FPGA"

# Print location of resulting .riscv
echo
echo "[*] Build outputs:"
find "$TEST_DIR" -maxdepth 1 -name "*.riscv" -newer "$TEST_DIR/Makefile" 2>/dev/null | while read -r f; do
    ls -lh "$f"
done
