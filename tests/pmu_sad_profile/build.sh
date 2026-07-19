#!/bin/bash
source /home/mhassen/mahdi_setup.sh
cd /home/mhassen/he-soc/software/pmu_sad_profile
make clean pmu_bench fpga=1 && \
riscv64-unknown-elf-objdump -d pmu_bench.riscv > pmu_bench.dump && \
python3 extract_addrs.py pmu_bench.dump
