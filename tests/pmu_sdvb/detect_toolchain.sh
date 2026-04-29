#!/bin/bash
# Toolchain Detection Script for PMU Counter Verification Test
# Run this to automatically detect and configure your RISC-V toolchains

echo "======================================"
echo "RISC-V Toolchain Detection"
echo "======================================"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check for RV32 toolchains
echo "Checking for RV32 (PMU) toolchains..."
echo ""

RV32_CC=""
RV32_OBJDUMP=""
RV32_OBJCOPY=""

# Option 1: Check for LLVM/Clang
if command -v clang &> /dev/null; then
    CLANG_VERSION=$(clang --version 2>&1 | head -n1)
    if echo "$CLANG_VERSION" | grep -q "riscv\|RISC-V"; then
        echo -e "${GREEN}✓${NC} Found Clang with RISC-V support:"
        echo "  $CLANG_VERSION"
        RV32_CC="clang"
        RV32_OBJDUMP="llvm-objdump"
        RV32_OBJCOPY="llvm-objcopy"
    else
        echo -e "${YELLOW}⚠${NC} Found Clang but may not have RISC-V support:"
        echo "  $CLANG_VERSION"
        echo "  You may need to install LLVM with RISC-V backend"
    fi
fi

# Option 2: Check for riscv32-unknown-elf-gcc
if command -v riscv32-unknown-elf-gcc &> /dev/null; then
    GCC32_VERSION=$(riscv32-unknown-elf-gcc --version 2>&1 | head -n1)
    echo -e "${GREEN}✓${NC} Found riscv32-unknown-elf-gcc:"
    echo "  $GCC32_VERSION"
    if [ -z "$RV32_CC" ]; then
        RV32_CC="riscv32-unknown-elf-gcc"
        RV32_OBJDUMP="riscv32-unknown-elf-objdump"
        RV32_OBJCOPY="riscv32-unknown-elf-objcopy"
    fi
fi

# Option 3: Check for riscv-none-elf-gcc
if command -v riscv-none-elf-gcc &> /dev/null; then
    GCC_NONE_VERSION=$(riscv-none-elf-gcc --version 2>&1 | head -n1)
    echo -e "${GREEN}✓${NC} Found riscv-none-elf-gcc:"
    echo "  $GCC_NONE_VERSION"
    if [ -z "$RV32_CC" ]; then
        RV32_CC="riscv-none-elf-gcc"
        RV32_OBJDUMP="riscv-none-elf-objdump"
        RV32_OBJCOPY="riscv-none-elf-objcopy"
    fi
fi

echo ""

# Check for RV64 toolchains (for CVA6)
echo "Checking for RV64 (CVA6) toolchains..."
echo ""

RV64_CC=""

# Check for riscv64-unknown-elf-gcc
if command -v riscv64-unknown-elf-gcc &> /dev/null; then
    GCC64_VERSION=$(riscv64-unknown-elf-gcc --version 2>&1 | head -n1)
    echo -e "${GREEN}✓${NC} Found riscv64-unknown-elf-gcc:"
    echo "  $GCC64_VERSION"
    RV64_CC="riscv64-unknown-elf-gcc"
fi

echo ""
echo "======================================"
echo "Summary"
echo "======================================"

if [ -n "$RV32_CC" ]; then
    echo -e "${GREEN}✓ RV32 toolchain found:${NC} $RV32_CC"
else
    echo -e "${RED}✗ No RV32 toolchain found${NC}"
    echo "  You need one of:"
    echo "    - Clang/LLVM with RISC-V support"
    echo "    - riscv32-unknown-elf-gcc"
fi

if [ -n "$RV64_CC" ]; then
    echo -e "${GREEN}✓ RV64 toolchain found:${NC} $RV64_CC"
else
    echo -e "${YELLOW}⚠ No RV64 toolchain found${NC}"
    echo "  (May be provided by common.mk)"
fi

echo ""

# Offer to update Makefile
if [ -n "$RV32_CC" ]; then
    echo "======================================"
    echo "Recommended Configuration"
    echo "======================================"
    echo ""
    echo "Add these lines to pmu_firmware/Makefile:"
    echo ""
    echo "CC = $RV32_CC"
    echo "OBJDUMP = $RV32_OBJDUMP"
    echo "OBJCOPY = $RV32_OBJCOPY"
    echo ""

    read -p "Would you like to update pmu_firmware/Makefile automatically? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        if [ -f "pmu_firmware/Makefile" ]; then
            # Backup original
            cp pmu_firmware/Makefile pmu_firmware/Makefile.backup

            # Update toolchain lines
            sed -i.tmp "s|^CC = .*|CC = $RV32_CC|" pmu_firmware/Makefile
            sed -i.tmp "s|^OBJDUMP = .*|OBJDUMP = $RV32_OBJDUMP|" pmu_firmware/Makefile
            sed -i.tmp "s|^OBJCOPY = .*|OBJCOPY = $RV32_OBJCOPY|" pmu_firmware/Makefile
            rm -f pmu_firmware/Makefile.tmp

            echo -e "${GREEN}✓ Updated pmu_firmware/Makefile${NC}"
            echo "  Backup saved as pmu_firmware/Makefile.backup"
        else
            echo -e "${RED}✗ pmu_firmware/Makefile not found${NC}"
        fi
    fi
else
    echo "======================================"
    echo "Action Required"
    echo "======================================"
    echo ""
    echo -e "${RED}Please install a RISC-V RV32 toolchain:${NC}"
    echo ""
    echo "Option 1: Install LLVM with RISC-V support"
    echo "  See: https://github.com/llvm/llvm-project"
    echo ""
    echo "Option 2: Install riscv32-unknown-elf GCC"
    echo "  See: https://github.com/riscv-collab/riscv-gnu-toolchain"
    echo "  Configure with: --with-arch=rv32im --with-abi=ilp32"
    echo ""
fi

echo ""
echo "======================================"
echo "Next Steps"
echo "======================================"
echo ""
if [ -n "$RV32_CC" ]; then
    echo "1. Verify configuration: cat pmu_firmware/Makefile"
    echo "2. Build PMU firmware: make pmu_firmware"
    echo "3. Build complete test: make"
    echo "4. Run on hardware"
else
    echo "1. Install RV32 toolchain (see above)"
    echo "2. Run this script again: ./detect_toolchain.sh"
    echo "3. Or manually edit pmu_firmware/Makefile"
fi
echo ""
