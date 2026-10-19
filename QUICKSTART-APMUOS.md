# Quickstart: a program on the APMU

Assumes Linux is booted on the FPGA and reachable from the PC as `root@10.42.0.2`.

## 1. Clone apmu-os

```sh
git clone https://github.com/maskedarray/apmu-os.git
cd apmu-os
```

## 2. Replace `main()`

In order to build without the traditional apmu-os architecture (base component, event loop), replace the `main()` function in `main.c` with:

```c
#include <debug_printf.h>

int main(void)
{
    init_print_mem();
    printf("hello from the APMU: %d\n", 42);
    return 0;
}
```

After `main()` returns, the PE parks in `sleep_loop` (`crt0.s`).

## 3. Build

Needs the APMU LLVM (clang 18 with the `cnt.*` instructions, [maskedarray/llvm-project](https://github.com/maskedarray/llvm-project) branch `rv-apmu-sw`) and riscv32 newlib-nano. Both are installed on rp-ox in `/HDrive8/a26rahma/apmu-toolchain`.

On rp-ox, in apmu-os dir:

```sh
source /HDrive8/a26rahma/apmu-toolchain/env.sh
make generate-bin-files
```

`env.sh` sets:

```sh
export LLVM_BIN=/HDrive8/a26rahma/apmu-toolchain/llvm/bin
export RISCV_SYSROOT=/HDrive8/a26rahma/apmu-toolchain/riscv32/sysroot
export NEWLIB_BUILD=/HDrive8/a26rahma/apmu-toolchain/riscv32/newlib-nano
export LIBGCC_A=/HDrive8/a26rahma/apmu-toolchain/riscv32/libgcc/libgcc.a
```

Check: `$LLVM_BIN/clang --version` prints `clang version 18.1.6`.

Output: `build/text_section.bin` (ISPM, 0x10427000) and `build/data_rodata_bss.bin` (DSPM, 0x10429000).

## 4. Copy to the board

```sh
scp -O build/text_section.bin build/data_rodata_bss.bin root@10.42.0.2:/tmp/
```

Replace the ip if needed

## 5. Load and run with apmuctl

On the linux booted on vcu118:

```sh
apmuctl load /tmp/text_section.bin /tmp/data_rodata_bss.bin
apmuctl run
apmuctl info
```

Stop the PE with `apmuctl halt`. `apmuctl run` restarts it from the ISPM base; it does not resume.

## 6. (Optional) Read back `printf` output

`printf` (`debug_printf.h`, `%d %c %s`) writes to the DSPM print buffer at DSPM + 0x1800 (up to 0x1FFC). Call `init_print_mem()` once before the first `printf`.

```c
printf("counter %d = %d\n", 3, value);
```

Raw words, on the board:

```sh
apmuctl dspm 0x1800 32
```

As text, from the host PC:

```sh
ssh root@10.42.0.2 apmuctl dspm 0x1800 32 | python3 -c 'import sys; print(b"".join(int(w, 16).to_bytes(4, "little") for l in sys.stdin for w in l.split()[1:]).split(b"\0")[0].decode())'
```
