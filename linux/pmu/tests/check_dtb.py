#!/usr/bin/env python3
"""Validate PMU bindings from the compiled DTB (requires fdtget)."""
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("dtb")
parser.add_argument("--sscofpmf", action="store_true",
                    help="expect the sampling-capable hardware description")
args = parser.parse_args()
dtb = args.dtb

def cells(prop):
    text = subprocess.check_output(["fdtget", "-t", "x", dtb, "/pmu", prop], text=True)
    return [int(value, 16) for value in text.split()]

mapping = cells("riscv,event-to-mhpmevent")
assert mapping == [1, 0, 23, 2, 0, 24, 5, 0, 9, 6, 0, 10], mapping
counters = cells("riscv,event-to-mhpmcounters")
assert counters == [1, 2, 0x1f8, 5, 6, 0x1f8], counters
raw = cells("riscv,raw-event-to-mhpmcounters")
assert len(raw) == 24 * 5
for event in range(1, 25):
    assert raw[(event-1)*5:event*5] == [0, event, 0xffffffff, 0xffffffff, 0x1f8]
for hart in range(4):
    isa = subprocess.check_output(["fdtget", dtb, f"/cpus/cpu@{hart}", "riscv,isa"], text=True)
    assert ("sscofpmf" in isa) == args.sscofpmf, isa
print("PMU DT PASS: 4 generic mappings, 24 exact raw mappings, 6 counters, 4 harts")
