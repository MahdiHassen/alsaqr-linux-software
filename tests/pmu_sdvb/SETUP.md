# Quick Setup Guide

## Your System Configuration

**Location**: `/home/mhassen/he-soc/software/pmu_counter_verify_test/`

## Before Building

### 1. Update Toolchain Paths

Edit `pmu_firmware/Makefile` and set your toolchain paths:

```bash
cd /home/mhassen/he-soc/software/pmu_counter_verify_test/pmu_firmware
nano Makefile  # or vim/emacs
```

Update these lines based on your toolchain installation:

```makefile
# Option 1: If you have LLVM/Clang for RV32
CC = /path/to/your/clang
OBJDUMP = /path/to/your/llvm-objdump
OBJCOPY = /path/to/your/llvm-objcopy

# Option 2: If you have GCC for RV32
CC = /path/to/riscv32-unknown-elf-gcc
OBJDUMP = /path/to/riscv32-unknown-elf-objdump
OBJCOPY = /path/to/riscv32-unknown-elf-objcopy
```

**Common locations:**
- LLVM: `~/apmu-toolchains/llvm-project/build/bin/clang`
- GCC: `~/riscv32-gcc/bin/riscv32-unknown-elf-gcc`
- System: `/usr/bin/clang` (if installed system-wide)

### 2. Check Your Current Toolchains

```bash
# Check what you have available
which clang
which riscv32-unknown-elf-gcc
which riscv64-unknown-elf-gcc

# Check versions
clang --version
riscv32-unknown-elf-gcc --version
riscv64-unknown-elf-gcc --version
```

### 3. Verify Common Build System

Check that the common.mk file exists:

```bash
ls -la /home/mhassen/he-soc/software/common.mk
```

If it doesn't exist, you may need to adjust the Makefile:

```makefile
# In the main Makefile, change:
include $(shell pwd)/../common.mk

# To your actual path, for example:
include /home/mhassen/he-soc/software/common/common.mk
```

## Quick Build Test

### Test 1: Build PMU Firmware Only

```bash
cd /home/mhassen/he-soc/software/pmu_counter_verify_test
make pmu_firmware
```

**Expected output:**
```
======================================
Building PMU Firmware...
======================================
mkdir -p build
clang ... -c crt0.s -o build/crt0.o
clang ... -c main.c -o build/main.o
clang ... -c common/debug_printf.c -o build/debug_printf.o
clang ... -o build/pmu_firmware.elf
llvm-objcopy -O binary --only-section=.text ...
llvm-objcopy -O binary --only-section=.data ...

=== PMU Firmware Binary Sizes ===
-rw-r--r-- 1 mhassen users 1234 ... text_section.bin
-rw-r--r-- 1 mhassen users   56 ... data_rodata_bss.bin
```

**If this fails:** Check toolchain paths in `pmu_firmware/Makefile`

### Test 2: Verify Binary Generation

```bash
ls -lh pmu_firmware/build/
```

You should see:
- `text_section.bin` (PMU code)
- `data_rodata_bss.bin` (PMU data)
- `pmu_firmware.elf` (ELF file)
- `pmu_firmware.asm` (disassembly)

### Test 3: Build Complete System

```bash
cd /home/mhassen/he-soc/software/pmu_counter_verify_test
make clean
make
```

**Expected output:**
```
======================================
Building PMU Firmware...
======================================
[PMU firmware builds...]

======================================
Converting PMU text section to object file...
Converting PMU data section to object file...
======================================

[CVA6 compilation...]

======================================
Linking CVA6 app with embedded PMU firmware...
======================================
```

### Test 4: Verify Embedded Symbols

```bash
make verify-pmu-symbols
```

Should show:
```
====================================
Verifying PMU firmware symbols...
====================================
Text section symbols:
0000000000000000 D _binary_text_section_bin_start
00000000000004d2 D _binary_text_section_bin_end
00000000000004d2 A _binary_text_section_bin_size

Data section symbols:
0000000000000000 D _binary_data_rodata_bss_bin_start
0000000000000038 D _binary_data_rodata_bss_bin_end
0000000000000038 A _binary_data_rodata_bss_bin_size
```

## Common Issues

### Issue 1: "Command not found" errors

```
make: clang: Command not found
```

**Solution:** Update toolchain path in `pmu_firmware/Makefile` to absolute path

### Issue 2: "No rule to make target '../common.mk'"

```
make: *** No rule to make target '../common.mk'.
```

**Solution:** Find where common.mk is located and update the include path:

```bash
find /home/mhassen/he-soc -name "common.mk"
# Then update Makefile with the correct path
```

### Issue 3: Wrong architecture errors

```
error: unknown target triple 'riscv32-unknown-elf'
```

**Solution:** Your toolchain doesn't support RV32. You need:
- LLVM with RISC-V support, OR
- riscv32-unknown-elf-gcc

### Issue 4: Linker errors about missing symbols

```
undefined reference to `_binary_text_section_bin_start'
```

**Solution:** PMU firmware didn't build. Run `make pmu_firmware` first.

## Minimal Working Configuration

If you're having trouble, try this minimal setup:

1. **Use system clang** (if available):
   ```makefile
   CC = clang
   OBJDUMP = llvm-objdump
   OBJCOPY = llvm-objcopy
   ```

2. **Or use riscv32-gcc** (simpler):
   ```makefile
   CC = riscv32-unknown-elf-gcc
   OBJDUMP = riscv32-unknown-elf-objdump
   OBJCOPY = riscv32-unknown-elf-objcopy
   ```

3. **Build step by step:**
   ```bash
   cd pmu_firmware
   make clean
   make all
   make generate-bin-files
   cd ..
   make
   ```

## Next Steps

Once the build succeeds:

1. **Check output file**:
   ```bash
   ls -lh pmu_test.riscv
   file pmu_test.riscv
   ```

2. **View disassembly** (optional):
   ```bash
   riscv64-unknown-elf-objdump -D pmu_test.riscv | less
   ```

3. **Load to hardware** using your usual method

4. **Expected test output** - see README.md

## Getting Help

If you encounter issues:

1. Run `make help` to see available targets
2. Run `make show-pmu-size` to check firmware sizes
3. Check `pmu_firmware/build/pmu_firmware.asm` to verify PMU code
4. Verify toolchain with `which clang` and `clang --version`

## File Locations Summary

```
/home/mhassen/he-soc/software/pmu_counter_verify_test/
├── Makefile                   # Main build (RV64)
├── pmu_test.c                 # CVA6 test code
├── pmu_defines.h              # Register addresses
├── pmu_test_func.c            # Helper functions
├── pmu_test_func.h
├── README.md                  # Full documentation
├── SETUP.md                   # This file
└── pmu_firmware/
    ├── Makefile               # PMU build (RV32) - UPDATE THIS
    ├── main.c                 # PMU firmware code
    ├── crt0.s                 # PMU startup
    ├── linker.ld              # PMU memory map
    ├── common/
    │   └── debug_printf.c
    ├── include/
    │   └── *.h
    └── build/                 # Generated files
        ├── text_section.bin
        └── data_rodata_bss.bin
```

Good luck! 🚀
