# APMU from Linux userspace

Control the AlSaqr APMU — counters and the Ibex processing element (PE) with
its ISPM/DSPM scratchpads — from a normal Linux process on the booted CVA6,
instead of from a baremetal image.

| File | Role |
|---|---|
| `apmu_regs.h` | Register map. Single source of truth. |
| `libapmu.h` / `libapmu.c` | The library: map, MMIO, PE control, counters, firmware loading. |
| `apmuctl.c` | CLI over the library. Ships at `/usr/bin/apmuctl`. |
| `example/apmu_hello.c` | Smallest complete load-and-start program. |
| `test/` | Offline loader test — runs on the dev host, no board needed. |

## Build

Installed automatically at `/usr/bin/apmuctl` by a from-source image build
(wired into `cva6-sdk/Makefile`'s `ROOTFS_EXTRAS`).

For an already-booted target, `make dist` builds both flavours stripped and
prints their md5s:

- `apmuctl` (~27 KB, dynamic) — for a rootfs built from this tree.
- `apmuctl-static` (~410 KB) — for any other rootfs, including `linux/prebuilt`.
  Use it if the dynamic one fails with a loader error.

Push it with `../../scripts/send_file.sh` + `recv` (RUNBOOK §8).

## Use

`apmuctl --help` is the reference. Start here:

```bash
apmuctl example_bubble
```

That loads and runs a program ported from `tests/pmu_test`, which is known to
work on this hardware from baremetal. If it passes, the `/dev/mem` path, the
loader and the register map are all good, and anything that fails afterwards is
in your own firmware.

Then:

```bash
apmuctl load text_section.bin data_rodata_bss.bin
apmuctl run
apmuctl dspm 0x1800 4
```

`apmu-os` prints into DSPM at `+0x1800` rather than to a UART, so that last
command is how you read its output. Words are little-endian, so read each one
in byte pairs right to left: `20554d50` is `"PMU "`.

## Things that will bite

- **Root, and 32-bit accesses only.** The APMU config port is AXI4-Lite;
  anything else silently misbehaves. The library enforces it.
- **A bad address returns garbage, it does not fault.** The SoC ties off bus
  error reporting. `0xba5e1e55` is the scratchpads' "decode failed" value and
  is the one bad-address signal you do get.
- **`run` is a boot, not a resume.** Releasing the stall restarts the PE at its
  boot address.
- **The first instruction of an image is discarded**, so it must be a NOP
  (`tests/pmu_test/pmu_test_func.c:176`). This is why firmware linker scripts
  set ISPM `ORIGIN` to the load address minus 4, and why `crt0.s` starts with
  NOPs. Keep the invariant if the address map moves.
- **DSPM survives a PE restart.** `load` does not clear it, and `crt0.s` has no
  `.data` copy loop — `.data` is initialised once, by `load`. So always `load`
  before `run` rather than re-running a stale image.
- **`-c N` must precede the command.** Flag parsing stops at the first
  non-flag argument.

## Writing your own program

Link against the library rather than shelling out to `apmuctl`:

```c
apmu_t a;
apmu_open(&a, 0);
apmu_load_raw(&a, text, tlen, data, dlen, 0, &img);
apmu_run(&a);
uint32_t v = apmu_rd32(&a, APMU_DSPM_BASE + 0x100);
apmu_close(&a);
```

`example/apmu_hello.c` is a complete working version. Every call is documented
in `libapmu.h`.
