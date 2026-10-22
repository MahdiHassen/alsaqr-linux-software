#!/usr/bin/env python3
"""Host-test the actual OpenSBI helper with stubbed CSR writes (not an SBI boot)."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

sdk = Path(__file__).resolve().parents[2] / "cva6-sdk" / "opensbi"
source = (sdk / "lib/sbi/sbi_pmu.c").read_text()


def function(name):
    # Match only a top-level definition, not a call or forward declaration.
    match = re.search(r"^static (?:void|int) " + name + r"\(", source, re.M)
    if not match:
        raise SystemExit("missing OpenSBI helper: " + name)
    start = match.start()
    end = source.index("\n}\n", start) + 3
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define __riscv_xlen 64
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#define SBI_HART_EXT_SMCNTRPMF 1
struct sbi_scratch { int unused; };
static struct sbi_scratch scratch;
static int has_extension, writes;
static uint64_t written_csr, written_value;
static struct sbi_scratch *sbi_scratch_thishart_ptr(void) { return &scratch; }
static int sbi_hart_has_extension(struct sbi_scratch *s, int extension) {
    assert(s == &scratch && extension == SBI_HART_EXT_SMCNTRPMF);
    return has_extension;
}
static void csr_write_num(uint64_t csr, uint64_t value) {
    writes++; written_csr = csr; written_value = value;
}
'''
suffix = r'''
int main(void) {
    const unsigned long filters[] = {
        SBI_PMU_CFG_FLAG_SET_VUINH, SBI_PMU_CFG_FLAG_SET_VSINH,
        SBI_PMU_CFG_FLAG_SET_UINH, SBI_PMU_CFG_FLAG_SET_SINH,
        SBI_PMU_CFG_FLAG_SET_MINH,
    };
    const uint64_t bits[] = {
        MHPMEVENT_VUINH, MHPMEVENT_VSINH, MHPMEVENT_UINH,
        MHPMEVENT_SINH, MHPMEVENT_MINH,
    };
    unsigned checks = 0;
    for (has_extension = 0; has_extension <= 1; has_extension++) {
        for (int fixed = 0; fixed <= 2; fixed += 2) {
            for (unsigned combination = 0; combination < 32; combination++) {
                unsigned long flags = SBI_PMU_CFG_FLAG_CLEAR_VALUE |
                    SBI_PMU_CFG_FLAG_AUTO_START | SBI_PMU_CFG_FLAG_SKIP_MATCH;
                uint64_t expected = MHPMEVENT_MINH;
                for (int i = 0; i < 5; i++) if (combination & (1U << i)) {
                    flags |= filters[i]; expected |= bits[i];
                }
                writes = 0;
                int result = pmu_fixed_ctr_update_inhibit_bits(fixed, flags);
                if (!has_extension) {
                    assert(result == (combination ? SBI_ENOTSUPP : fixed));
                    assert(writes == 0);
                } else {
                    assert(result == fixed && writes == 1);
                    assert(written_csr == (fixed ? CSR_MINSTRETCFG : CSR_MCYCLECFG));
                    assert(written_value == expected);
                }
                checks++;
            }
        }
    }
    has_extension = 1;
    assert(pmu_fixed_ctr_update_inhibit_bits(3, 0) == SBI_EFAIL);
    printf("FIXED FALLBACK PASS %u checks\n", checks + 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pmu-fixed-") as temporary:
    directory = Path(temporary)
    test = directory / "test.c"
    test.write_text(prefix + function("pmu_update_inhibit_flags") +
                    function("pmu_fixed_ctr_update_inhibit_bits") + suffix)
    executable = directory / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", "-I" + str(sdk / "include"), str(test),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
