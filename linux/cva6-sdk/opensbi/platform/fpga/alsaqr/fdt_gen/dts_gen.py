#!/usr/bin/env python3

import sys
import fileinput

CPU_TARGET="""CPUhartid: cpu@hartid {
      device_type = "cpu";
      status = "okay";
      compatible = "eth,ariane", "riscv";
      clock-frequency = <targetfreq>;
      riscv,isa = "rv64fimadch";
      mmu-type = "riscv,sv39";
      tlb-split;
      reg = <hartid>;
      CPUhartid_intc: interrupt-controller {
        #address-cells = <1>;
        #interrupt-cells = <1>;
        interrupt-controller;
        compatible = "riscv,cpu-intc";
      };
    };
"""

CLINT="""clint@2000000 {
      compatible = "riscv,clint0";
      interrupts-extended = all-interrupts;
      reg = <0x0 0x2000000 0x0 0xc0000>;
      reg-names = "control";
    };
"""
PLIC="""    PLIC0: interrupt-controller@c000000 {
      #address-cells = <0>;
      #interrupt-cells = <1>;
      compatible = "riscv,plic0";
      interrupt-controller;
      interrupts-extended = all-interrupts;
      reg = <0x0 0xc000000 0x0 0x4000000>;
      riscv,max-priority = <7>;
      riscv,ndev = <255>;
    };
"""
DEBUG="""    debug-controller@0 {
      compatible = "riscv,debug-013";
      interrupts-extended = all-interrupts;
      reg = <0x0 0x0 0x0 0x1000>;
      reg-names = "control";
    };"""

def replace_strings(file_path, old_string, new_string):
    with fileinput.FileInput(file_path, inplace=True, backup=".bak") as file:
        for line in file:
            print(line.replace(old_string, new_string), end='')

if __name__ == "__main__":
    if len(sys.argv) != 7:
        print("Usage: python3 dts_gen.py <file-path> <num_harts> <target-freq> <half-freq> <target-baud> <mem-size>")
        sys.exit(1)

    # Extract command-line arguments
    file_path = sys.argv[1]
    num_harts = sys.argv[2]

    cpus = ""
    clint_interrupts = ""
    plic_interrupts = ""
    dbg_interrupts = ""

    for i in range(int(num_harts)):
        cpus = cpus + "    " + CPU_TARGET.replace("hartid",str(i))
        clint_interrupts = f"{clint_interrupts} <&CPU{i}_intc 3 &CPU{i}_intc 7> "
        plic_interrupts = f"{plic_interrupts} <&CPU{i}_intc 11 &CPU{i}_intc 9> "
        dbg_interrupts = f"{dbg_interrupts} <&CPU{i}_intc 65535> "
        if(i!=int(num_harts)-1):
            clint_interrupts = clint_interrupts + ","
            plic_interrupts = plic_interrupts + ","
            dbg_interrupts = dbg_interrupts + ","

    clint = CLINT.replace("all-interrupts",clint_interrupts)
    plic = PLIC.replace("all-interrupts",plic_interrupts)
    debug= DEBUG.replace("all-interrupts",dbg_interrupts)

    replace_strings(file_path,"target_cpus",cpus)
    replace_strings(file_path,"ariane_peripherals",clint+plic+debug)
    replace_strings(file_path,"targetfreq",sys.argv[3])
    replace_strings(file_path,"halffreq",sys.argv[4])
    replace_strings(file_path,"targetbaud",sys.argv[5])
    replace_strings(file_path,"mem_size",sys.argv[6])
