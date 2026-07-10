# AlSaqr on VCU118 — Command Runbook

Copy-paste cheat sheet. Everything runs from `/HDrive8/emon/alsaqr_stuff/alsaqr-software`.

---

## 0. Environment — DO THIS FIRST IN EVERY NEW TERMINAL

```bash
cd /HDrive8/emon/alsaqr_stuff/alsaqr-software
source source.sh
```

> If you skip this, OpenOCD dies with `error while loading shared libraries: libhidapi-hidraw.so.0`.
> That error ALWAYS means "you forgot to source." Just source and retry.

---

## 1. Program a bitstream

```bash
make load_bitstream \
  BITSTREAM=bitstreams/alsaqr_xilinx_48.bit \
  HW_TARGET=xilinx_tcf/Digilent/210308B3AC77
```

Available bitstreams (swap into `BITSTREAM=`):

| File | Notes | Serial baud |
|---|---|---|
| `bitstreams/alsaqr_opendram2.bit` | **PREFERRED** — OpenDRAM v5, 20 MHz, boots Linux, cleanest console (validated 07/08) | **38400** |
| `bitstreams/alsaqr_xilinx_48.bit` | known-good baseline, boots Linux | **38400** |
| `bitstreams/alsaqr_opendram.bit`  | OpenDRAM (old, 04/29) — booted Linux 07/07 | **115200** |

> HW_TARGET is fixed to this board — do NOT change it.
> After programming, **wait ~30 s** for DDR calibration to finish before connecting.

---

## 2. Build an application

```bash
./scripts/build.sh <test_directory>/ build
```

The ELF lands in the test dir, named after `APP :=` in its Makefile (NOT always the dir name):

```bash
./scripts/build.sh hello_culsans/ build              # -> hello_culsans/hello_culsans.riscv
./scripts/build.sh tests/pmu_test/ build             # -> tests/pmu_test/pmu_test.riscv
./scripts/build.sh tests/pmu_mempol_synth/ build     # -> tests/pmu_mempol_synth/fat_test.riscv
./scripts/build.sh tests/pmu_sdvb/ build             # -> tests/pmu_sdvb/fat_test.riscv
./scripts/build.sh tests/march_test/ build           # -> tests/march_test/march_test.riscv
```

---

## 3. Load & run a baremetal app (JTAG)

```bash
./scripts/load.sh <path/to/app.riscv>
```

Runs 5 s, halts, dumps memory at 0x85000000, drops into GDB. Exit GDB with `quit`.

```bash
./scripts/load.sh hello_culsans/hello_culsans.riscv
./scripts/load.sh tests/pmu_test/pmu_test.riscv
./scripts/load.sh tests/pmu_mempol_synth/fat_test.riscv     # note: fat_test, not pmu_mempol_synth
./scripts/load.sh tests/pmu_sdvb/fat_test.riscv             # note: fat_test
./scripts/load.sh tests/march_test/march_test.riscv
```

---

## 4. Boot Linux (JTAG)

```bash
pkill -9 openocd 2>/dev/null; sleep 2
./linux/scripts/boot_linux.sh
```

(defaults to `linux/prebuilt/alsaqr.dtb` + `fw_payload.elf`; override with `DTB=...` / `PAYLOAD=...`)

- Uploads the 15 MB payload over JTAG → **~15 min of silence** (`Loading section .payload`). Normal, don't interrupt.
- After `Continuing.`, watch **serial** for OpenSBI banner → Linux → `$` shell.
- `Remote connection closed` in the GDB terminal AFTER `Continuing.` is harmless — the cores run on their own.

---

## 5. Serial console

**Match the bitstream's baud** (see table in §1). Two ways:

**A) VSCode Serial Monitor in TERMINAL MODE (preferred — fixes the garble).**
Set Baud to the bitstream's rate, then click the **top-right button (box with `>`)** to switch to terminal mode. It sends each keystroke immediately instead of line-buffering, so the UART buffer doesn't overload. Default (non-terminal) mode is what causes the garble.

**B) `screen`** (char-by-char too; releases cleanly):
```bash
screen /dev/ttyUSB3 38400        # xilinx_48 / opendram2
screen /dev/ttyUSB3 115200       # opendram (old)
```
Exit screen: `Ctrl-A`, then `k`, then `y`.

- Only ONE program can hold `/dev/ttyUSB3` — quit screen before using the Serial Monitor, and vice-versa.
- Even in terminal mode: **no arrow keys** (they send 3-byte bursts that overrun). Lower baud (38400) = far less garble than 115200.
- Quiet kernel spam once at the shell: `dmesg -n 1`

---

## 6. Recovery recipes (when things go wrong)

**GOLDEN RULE: reflash the bitstream before every Linux boot.**
Never run `boot_linux.sh` after a baremetal test (march_test, load.sh, anything) without reprogramming the FPGA first. Baremetal tests leave **dirty lines in the 1 MiB write-back LLC**; JTAG writes bypass the caches, so the DTB you load at 0x81800000 gets silently overwritten when those stale lines evict → kernel panics with "No DTB passed to the kernel" **before any console exists** → looks like a dead-silent boot stuck in `udelay` (it's actually panic()'s infinite mdelay loop). Fresh flash = empty caches = clean boot.


**`error while loading shared libraries: libhidapi-...`**
→ You forgot to source. `source source.sh` and retry.

**`could not connect: Connection timed out` / `Remote connection closed` (before Continuing.)**
→ Stale OpenOCD holds the port/adapter, or USB not settled after a kill:
```bash
pkill -9 openocd 2>/dev/null; sleep 2
```
Then retry. Right after programming a fresh bitstream, the first attach can fail while DDR calibrates — wait ~20–30 s and retry.

**`unable to halt hart N` / `dmstatus = 0x00030c82` (cores running, won't halt)**
→ Cores are wedged (ran into DDR before calibration). Reprogramming alone does NOT fix it.
→ **Power-cycle the board** (main power off, ~10 s, on), reprogram, wait ~30 s, retry.

**Nothing / garbage on serial**
→ Wrong baud. Match the bitstream (§1). `screen` must equal the SoC's hardwired rate; you can't fix it host-side.

---

## 7. Known facts

- Board JTAG target: `xilinx_tcf/Digilent/210308B3AC77` (fixed).
- Serial device: `/dev/ttyUSB3` (Silicon Labs CP210x).
- DRAM: 512 MB @ `0x80000000`–`0xA0000000`.
- Booted Linux comes up **single-core** (hart 0; others park in OpenSBI).
- OpenDRAM verified good (March C- + full Linux boot, 07/07) on `alsaqr_opendram.bit`.
