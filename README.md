# AlSaqr Software Bundle

Fully self-contained software bundle for the AlSaqr SoC FPGA emulation
platform (CVA6 + Ibex PMU). Includes the RISC-V toolchain, OpenOCD with its
shared library dependencies, helper scripts, and a curated set of tests.
Clone, source, build, run — no system-wide installs needed.

> **Platform:** all shipped binaries (RISC-V GCC/GDB, OpenOCD, vendored .so
> deps) are **x86_64 Linux**. On other platforms (ARM, macOS, Windows) you'll
> need to rebuild the toolchain and OpenOCD from source and drop the binaries
> into `toolchain/` and `openocd/bin/`.

> **Host requirements:** standard Linux dev environment — `bash`, `make`,
> `python3`, and a working glibc. Everything else (compiler, linker, GDB,
> OpenOCD, libpython3.12, pyelftools, .so deps) is in this bundle. No
> system-level packages need to be installed.

---

## Layout

```
alsaqr-software/
├── README.md                       # this file
├── source.sh                       # env exports — `source ./source.sh`
├── toolchain/
│   ├── rv64/                       # RV64 GCC + GDB (CVA6 target)
│   ├── rv32/                       # RV32 GCC (Ibex PMU firmware)
│   └── shim/                       # riscv-none-elf-* symlinks
│                                   # (PMU firmware Makefiles expect this name)
├── openocd/
│   ├── bin/openocd                 # prebuilt OpenOCD binary
│   ├── lib/                        # vendored .so deps (libftdi1, libusb-1.0,
│   │                               #  libudev, libhidapi-hidraw, libcap)
│   ├── openocd.cfg
│   └── tcl/                        # OpenOCD scripts
├── toolchain/python-runtime/       # bundled libpython3.12 + stdlib (used by
│                                   #  the GDB wrapper so hosts without
│                                   #  python3.12 still work)
├── scripts/
│   ├── build.sh                    # `make clean <target> fpga=1` wrapper
│   ├── load.sh                     # one-shot load + run via OpenOCD + GDB
│   └── vendor/                     # vendored Python deps (pyelftools)
└── tests/
    ├── common.mk                   # shared Makefile rules (SW_HOME points here)
    ├── inc/                        # shared headers
    ├── common/                     # crt.S, syscalls.c, test.ld, etc.
    ├── pmu_mempol_synth/           # memory policing test (LLC/MEM stages)
    ├── pmu_mempol_synth_sim/       # same test, shortened, for the xsim RTL simulation
    └── pmu_sdvb/                   # SDVB benchmarks under PMU regulation
                                    # (embedded FAT image at 0x82100000)
```

---

## Quick start

```bash
# 1. Source the env (sets PATH, RISCV, GDB_BIN, OPENOCD_SCRIPTS, SW_HOME)
source ./source.sh

# 2. Build a test
./scripts/build.sh tests/pmu_mempol_synth pmu_bench
# → tests/pmu_mempol_synth/pmu_bench.riscv

./scripts/build.sh tests/pmu_sdvb pmu_bench
# → tests/pmu_sdvb/pmu_bench.riscv

# 3. Load and run on FPGA (default 5s run, dump at 0x85000000)
./scripts/load.sh tests/pmu_mempol_synth/pmu_bench.riscv
```

---

## Tests

### `pmu_mempol_synth`
Synthetic memory-policing benchmark. Core 0 cycles through three workload
stages (LLC writes → DRAM writes → LLC writes) while the Ibex PMU runs the
token-bucket bandwidth policer. The PMU pauses/resumes core 0 via interrupts
when the bandwidth budget is exceeded. Useful for validating policing
behavior end-to-end.

Build:
```bash
./scripts/build.sh tests/pmu_mempol_synth pmu_bench
```

Run:
```bash
./scripts/load.sh tests/pmu_mempol_synth/pmu_bench.riscv --time 30000
```

### `pmu_mempol_synth_sim`
The same benchmark configured for the he-soc xsim RTL simulation (driven by the
alsaqr-mcp tools). It compiles the `pmu_mempol_synth` sources with `-DXSIM` and
smaller workload knobs (see the Makefile): the simulator resumes only core 0, so
the `XSIM` build wakes cores 2 and 3 itself, and the wall-clock delays / table
sizes of the FPGA version would take hours of simulated time. Nothing in the
FPGA build changes.

Build:
```bash
FPGA=0 ./scripts/build.sh tests/pmu_mempol_synth_sim pmu_bench
# or: alsaqr-mcp sw_build test=tests/pmu_mempol_synth_sim target=pmu_bench
```

Run: `alsaqr-mcp sim_run binary=tests/pmu_mempol_synth_sim/pmu_bench.riscv`,
then `sim_uart` for the console. Cores print concurrently without a lock, so
lines printed by several cores at once come out interleaved.

### `pmu_sdvb`
SDVB benchmark suite (disparity, mser, sift, stitch) running on core 0 under
PMU bandwidth regulation, with cores 1–3 generating interference. The FAT16
image containing benchmark inputs is embedded into the ELF at `0x82100000`
(no separate image load needed).

Build:
```bash
./scripts/build.sh tests/pmu_sdvb pmu_bench
```

Run (60s for full SDVB):
```bash
./scripts/load.sh tests/pmu_sdvb/pmu_bench.riscv --time 60000
```

---

## How the build works

Each test inherits from `tests/common.mk`. `source.sh` sets:
- `RISCV=$ALSAQR_ROOT/toolchain/rv64`
- `SW_HOME=$ALSAQR_ROOT/tests` (so `$(SW_HOME)/inc/`, `$(SW_HOME)/common/`,
  `$(SW_HOME)/common.mk` resolve correctly)
- `PATH` includes both rv64 and rv32 toolchain bins, plus the `riscv-none-elf-*`
  shim, plus the bundled OpenOCD

Pass `fpga=1` (default in `build.sh`) to set `-DFPGA_EMULATION` for 9600 baud
@ 100 MHz UART config.

---

## How `load.sh` works

1. Starts OpenOCD in the background using the bundled binary, listening on GDB
   port 6699 (default).
2. Launches GDB with a script that:
   - `target extended-remote :6699`
   - `monitor reset halt`
   - `load` the ELF (or `restore` for raw binaries with `--raw --addr`)
   - `monitor resume` → `monitor sleep <time>` → `monitor halt`
   - `monitor dump_image exp.dump 0x85000000 0x40000` (256 KB result region)
   - `x/128wx 0x85000000` (on-screen peek)
3. Drops you into an interactive GDB session for further inspection.

Useful flags:

| Flag             | Effect                                                |
|------------------|-------------------------------------------------------|
| `--time MS`      | ms to run before halt + dump (default 5000)          |
| `--counters`     | After halt, dump PMU counter MMIO regions 0–11       |
| `--gdb-port N`   | Override OpenOCD GDB port (default 6699)             |
| `--raw`          | Treat input as raw binary (use with `--addr`)        |
| `--addr 0xADDR`  | Base address for raw binary (default 0x80000000)     |

---

## Manual OpenOCD + GDB session

If you want full interactive control:

```bash
# Terminal 1 — start OpenOCD
$ALSAQR_ROOT/openocd/bin/openocd \
    -s $ALSAQR_ROOT/openocd \
    -c "adapter speed 2000" \
    -c "set _INTERFACE ftdi" \
    -c "set _DEVICE olimex-arm-usb-ocd-h" \
    -c "gdb_port 6699" \
    -c "tcl_port disabled" \
    -c "telnet_port disabled" \
    -c "set _NUM_CORES 4" \
    -c "set _TARGET smp" \
    -f openocd.cfg \
    -c "init" -c "reset halt"

# Terminal 2 — connect via GDB
$GDB_BIN tests/pmu_mempol_synth/pmu_bench.riscv
(gdb) target extended-remote :6699
(gdb) monitor reset halt
(gdb) load
(gdb) continue
# Ctrl-C to break, monitor halt, x/Nwx <addr> to inspect, etc.
```

---

## Troubleshooting

- **`riscv64-unknown-elf-gcc: command not found`** — you didn't `source ./source.sh`
- **`libpython3.12.so.1.0: cannot open shared object file`** — you're invoking
  the real GDB binary directly. Use `$GDB_BIN` (or `riscv64-unknown-elf-gdb`
  on the PATH) — the wrapper script sets `LD_LIBRARY_PATH` and `PYTHONHOME`
  to point at the bundled python runtime under `toolchain/python-runtime/`.
- **`riscv-none-elf-gcc: No such file or directory`** in PMU firmware build —
  the shim symlinks broke. Recreate them under `toolchain/shim/` pointing at
  the rv32 toolchain.
- **OpenOCD doesn't start** — make sure your USB JTAG adapter is plugged in
  and the user has permission. The default `INTERFACE=ftdi`,
  `DEVICE=olimex-arm-usb-ocd-h` matches the AlSaqr board; override via env if
  using a different probe.
- **`pmu_sdvb` reports "no FAT image at 0x82100000"** — the FAT image didn't
  get linked in. Verify `my_fat_image_reduced.img` is present in
  `tests/pmu_sdvb/` and that the build used `test_sim.ld`.
- **Toolchain size** — bundled toolchains are unstripped debug builds
  (~2.5 GB each, ~5 GB total). Suitable for self-hosted GitLab; not
  GitHub-friendly. To shrink, strip the host x86_64 binaries under `bin/` and
  `libexec/` (do **not** strip RISC-V target libs in `riscv64-unknown-elf/`).

---

## What's NOT in the bundle

- **Bitstreams** — handled separately, not in this repo.
- **Vivado / FPGA build tools** — assumed already on the host.
- **CVA6/Ibex RTL** — this is a software-only bundle.
