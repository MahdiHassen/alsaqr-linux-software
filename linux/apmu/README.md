# APMU from Linux

This directory contains the pre-eBPF host stack for the AlSaqr APMU:

```text
application -> libapmu -> /dev/apmu (apmu.ko) -> apmu-os -> component
```

The host stack expects an 8 KiB ISPM at `0x10427000`, a 128 KiB DSPM at
`0x10429000`, coherent host access to both scratchpads, and the counter/event
register layout in `apmu_regs.h`. Component code is linked at its actual ISPM
load address.

## Contents

| Path | Purpose |
|---|---|
| `kmod/apmu_main.c` | Owns the hardware, boots apmu-os, allocates and links components, and routes messages. |
| `kmod/apmu_link.c` | Links RV32 relocatable component objects into assigned ISPM/DSPM ranges. |
| `libapmu.[ch]` | Userspace API over `/dev/apmu`. |
| `apmu.c` | `apmu` command-line client. |
| `apmu_mmio.[ch]`, `apmuctl.c` | Root-only raw-MMIO diagnostic path. |
| `test/` | Offline loader, queue interoperability, and component-linker tests. |

## Build and test

Use a RISC-V Linux cross compiler when producing target binaries. The loader
and queue tests run on the development host; the component-linker comparison
also needs an `apmu-os` checkout and its RV32 LLVM toolchain:

```sh
make test
make test-queue APMU_OS=/path/to/apmu-os
make test-link APMU_OS=/path/to/apmu-os
```

The parent Linux build integrates `apmu`, `apmuctl`, and `apmu.ko` into the
initramfs through `cva6-sdk/Makefile`.

## Board sanity test

After booting a compatible Linux image and transferring the `apmu-os` images
and component objects, run:

```sh
apmuctl info
apmuctl example_bubble
apmu boot /tmp/text_section.bin /tmp/data_rodata_bss.bin
apmu run /tmp/hello.o 5 6 7
apmu install /tmp/latency_binning.o
apmu list
apmu uninstall 1
```

Expected hello output is `00000006 00000007 00000008`.

## Behavior

- One mutex serializes hardware access and driver state.
- A component belongs to the file descriptor that installed it unless
  `APMU_INSTALL_PERSIST` is requested by root.
- The module first-fits component text and data into the dynamic ISPM/DSPM
  regions and links against the export table published by apmu-os. It writes
  the images without stopping the PE and sends a generation-tagged install
  message; apmu-os synchronizes instruction fetch and activates the component.
- Uninstall quiesces the component on the PE before the module scrubs and releases
  its ISPM/DSPM ranges. A host-requested halt is destructive; release performs
  a cold boot rather than resuming old component state.
- Requests and responses use bounded polling because the PE has no interrupt to
  the host. A timeout is returned to userspace; the driver does not replay
  images or silently reinstall components.
- A cold apmu-os restart is detected through the session word. Lost components
  are forgotten and callers receive `ENOENT`. Trap metadata is logged when
  present.
- `apmuctl` bypasses the module through `/dev/mem`; use its read-only
  commands while `apmu.ko` owns the device, and do not mutate both paths at
  once.

The module and apmu-os share `apmu_abi.h`; both sides must use the same ABI
definition.
