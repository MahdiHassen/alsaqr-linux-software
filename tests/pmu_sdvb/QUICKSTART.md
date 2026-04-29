# Quick Start - PMU Counter Verification Test

**Location**: `/home/mhassen/he-soc/software/pmu_counter_verify_test/`

## What This Test Does

1. ✅ **PMU firmware** (C code) writes `0x11112222` to all 8 counter registers
2. ✅ **Core 0** polls DSPM until PMU signals completion
3. ✅ **Core 0** reads counter values and verifies they are `0x11112222`
4. ✅ **Core 0** writes `0x55555555` to `0x85000000` on success

## Files Overview

```
pmu_counter_verify_test/
├── pmu_firmware/          # PMU code (RV32IM - runs on Ibex)
│   ├── main.c             # ← PMU firmware writes to counters
│   └── Makefile           # ← UPDATE TOOLCHAIN PATHS HERE
│
├── pmu_test.c             # CVA6 test (RV64 - runs on Core 0)
├── Makefile               # Main build system
├── QUICKSTART.md          # ← This file
├── SETUP.md               # Detailed setup guide
├── README.md              # Full documentation
└── detect_toolchain.sh    # Auto-detect toolchains
```

## Build Instructions (3 Steps)

### Step 1: Detect and Configure Toolchain

```bash
cd /home/mhassen/he-soc/software/pmu_counter_verify_test
./detect_toolchain.sh
```

This will:
- Find your RISC-V toolchains
- Optionally update `pmu_firmware/Makefile` automatically

### Step 2: Build Everything

```bash
make clean
make
```

Expected output:
```
======================================
Building PMU Firmware...
======================================
[Compiling RV32 code...]

Converting PMU text section to object file...
Converting PMU data section to object file...

======================================
Linking CVA6 app with embedded PMU firmware...
======================================
[Linking...]
```

### Step 3: Verify Build

```bash
make show-pmu-size
make verify-pmu-symbols
ls -lh pmu_test.riscv
```

You should see:
- PMU firmware binaries (~1-2 KB)
- Embedded symbols in .o files
- Final `pmu_test.riscv` executable

## If Build Fails

### Problem: "Command not found"

```bash
# Manually set toolchain in pmu_firmware/Makefile
cd pmu_firmware
nano Makefile

# Change these lines:
CC = /path/to/your/clang              # or riscv32-unknown-elf-gcc
OBJDUMP = /path/to/your/llvm-objdump  # or riscv32-unknown-elf-objdump
OBJCOPY = /path/to/your/llvm-objcopy  # or riscv32-unknown-elf-objcopy
```

Common paths:
- `~/apmu-toolchains/llvm-project/build/bin/clang`
- `/usr/bin/clang`
- `~/riscv-tools/bin/riscv32-unknown-elf-gcc`

### Problem: "No rule to make target '../common.mk'"

```bash
# Find where common.mk is located
find /home/mhassen/he-soc -name "common.mk"

# Update Makefile line 58:
include /path/to/actual/common.mk
```

### Problem: Other errors

See `SETUP.md` for detailed troubleshooting.

## Running the Test

Load `pmu_test.riscv` to your hardware using your normal method, then check the output:

### Expected Output

```
==========================================
PMU Counter Verification Test
==========================================

Step 1: Halting PMU core...
Step 2: Clearing completion signal...

Step 3: Loading PMU firmware...
PMU Firmware Sizes:
  Text: 1234 bytes
  Data: 56 bytes
Loading firmware to ISPM (0x10427000)...
Loading firmware to DSPM (0x10428000)...
PMU firmware loaded successfully!

Step 4: Starting PMU core...
Starting PMU core...
PMU core started!

Step 5: Polling for PMU completion...
PMU signaled completion after 12345 polls!

Step 6: Reading PMU counter values...

Counter Values:
  Counter 0: 0x11112222
  Counter 1: 0x11112222
  Counter 2: 0x11112222
  Counter 3: 0x11112222
  Counter 4: 0x11112222
  Counter 5: 0x11112222
  Counter 6: 0x11112222
  Counter 7: 0x11112222

Step 7: Verifying counters...
SUCCESS: All counters contain 0x11112222!

Step 8: Writing 0x55555555 to 0x85000000...
Verification read: 0x55555555

==========================================
TEST PASSED
==========================================

Halting PMU core...
Test complete.
```

## Architecture

```
┌─────────────────────────┐         ┌───────────────────────┐
│  CVA6 Core 0 (RV64)     │         │  PMU Ibex Core (RV32) │
│  pmu_test.c             │         │  main.c               │
├─────────────────────────┤         ├───────────────────────┤
│                         │         │                       │
│ 1. Load firmware        │────────→│  ISPM (0x10427000)    │
│    to ISPM/DSPM         │  memcpy │  DSPM (0x10428000)    │
│                         │         │                       │
│ 2. Start PMU            │─ start─→│  for (i=0; i<8; i++)  │
│                         │         │    write_counter(i,   │
│ 3. Poll DSPM            │         │      0x11112222);     │
│    for completion       │←─done──│                       │
│                         │   101   │  write_mem(DSPM, 101);│
│ 4. Read counters        │         │                       │
│    from 0x10407000      │←─read──→│  Counter Registers    │
│                         │         │  (0x10407000 + i*0x1000)
│ 5. Verify = 0x11112222  │         │                       │
│                         │         │                       │
│ 6. Write 0x55555555     │         │                       │
│    to 0x85000000        │         │                       │
└─────────────────────────┘         └───────────────────────┘
```

## Key Differences from Other Tests

| Feature | This Test | pmu_L3_test_v2 |
|---------|-----------|----------------|
| PMU code | **C firmware** | Hand-written assembly |
| PMU role | **Writes to counters** | Monitors traffic |
| Build | **Dual-stage** (RV32+RV64) | Single-stage (RV64) |
| Toolchain | **Requires RV32** | RV64 only |

## Help

```bash
make help              # Show build targets
./detect_toolchain.sh  # Detect toolchains
cat SETUP.md           # Detailed setup
cat README.md          # Full documentation
```

## Summary

✅ **Built**: PMU firmware (C) + CVA6 app
✅ **Test**: PMU writes counters, Core 0 verifies
✅ **Result**: `0x55555555` at `0x85000000` = SUCCESS

Good luck! 🚀
