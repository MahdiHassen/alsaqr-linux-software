#!/usr/bin/env python3
"""plot_sweep.py SWEEP.txt OUT.png: latency (cycles per hop) vs working set, one curve per LLC way count,
from the "llcpart: sweep ways=W kb=K cyc=C miss=M hops=H" lines of the module (make sweep)."""
import re
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src, out = sys.argv[1], sys.argv[2]
pts = defaultdict(list)
for line in open(src):
    m = re.search(r"sweep ways=(\d+) kb=(\d+) cyc=([\d.]+) miss=(\d+) hops=(\d+)", line)
    if m:
        w, kb, cyc, miss, hops = int(m[1]), int(m[2]), float(m[3]), int(m[4]), int(m[5])
        pts[w].append((kb, cyc, miss / hops))

fig, (ax, ax2) = plt.subplots(2, 1, figsize=(8, 8), sharex=True)
for w in sorted(pts, reverse=True):
    xs = [p[0] for p in pts[w]]
    ax.plot(xs, [p[1] for p in pts[w]], marker="o", label=f"{w} ways ({w * 64} KB of LLC)")
    ax2.plot(xs, [p[2] for p in pts[w]], marker="o", label=f"{w} ways")
ax.set_xscale("log", base=2)
ax.set_ylabel("cycles per hop (rdcycle, 50 MHz)")
ax.set_title("LLC way partitioning: pointer-chase latency vs working set (Linux, one hart)")
ax.grid(True, which="both", alpha=0.3)
ax.legend()
ax2.set_xlabel("working set (KB), 64 B lines, random cycle")
ax2.set_ylabel("LLC read misses per hop")
ax2.grid(True, which="both", alpha=0.3)
ax2.set_ylim(-0.05, 1.1)
ticks = sorted({p[0] for v in pts.values() for p in v})
ax2.set_xticks(ticks)
ax2.set_xticklabels([str(t) for t in ticks], rotation=45)
fig.tight_layout()
fig.savefig(out, dpi=120)
print(f"wrote {out}")
