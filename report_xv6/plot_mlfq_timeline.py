#!/usr/bin/env python3
"""
Plots the MLFQ queue-vs-time timeline for the MLFQ Analysis
"""
import re
import sys

import matplotlib.pyplot as plt

QLOG_RE = re.compile(r"QLOG tick=(\d+) pid=(\d+) queue=(\d+)")
BOOST_PERIOD = 48  # must match the boost period in kernel/trap.c's clockintr()


def parse_qlog(path, min_samples=5):
  
    by_pid = {}
    with open(path) as f:
        for line in f:
            m = QLOG_RE.search(line)
            if not m:
                continue
            tick, pid, queue = (int(g) for g in m.groups())
            by_pid.setdefault(pid, []).append((tick, queue))
    for pid in by_pid:
        by_pid[pid].sort()
    return {pid: pts for pid, pts in by_pid.items() if len(pts) >= min_samples}


def plot(by_pid, out_path, watermark):
    fig, ax = plt.subplots(figsize=(11, 5.5), dpi=150)
    colors = plt.get_cmap("tab10")
    for i, pid in enumerate(sorted(by_pid)):
        points = by_pid[pid]
        ticks = [t for t, _ in points]
        queues = [q for _, q in points]
        color = colors(i % 10)
        ax.step(ticks, queues, where="post", color=color, linewidth=1.4, alpha=0.85)
        ax.scatter(ticks, queues, color=color, s=10, label=f"pid {pid}", zorder=3)

    max_tick = max(t for pts in by_pid.values() for t, _ in pts)
    boost = BOOST_PERIOD
    first_boost = True
    while boost <= max_tick:
        ax.axvline(boost, color="gray", linestyle="--", linewidth=1,
                   label="priority boost (every 48 ticks)" if first_boost else None)
        first_boost = False
        boost += BOOST_PERIOD

    ax.set_xlabel("Ticks elapsed since scheduler start")
    ax.set_ylabel("MLFQ queue ID")
    ax.set_yticks([0, 1, 2, 3])
    ax.set_ylim(-0.5, 3.5)
    ax.set_title("MLFQ queue occupancy over time (schedulertest workload)")
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)
    ax.grid(axis="y", linestyle=":", alpha=0.4)

    # Watermark, per the assignment: the part of the IIIT email before the @.
    fig.text(0.5, 0.5, watermark, fontsize=42, color="gray", alpha=0.15,
              ha="center", va="center", rotation=30)

    fig.tight_layout()
    fig.savefig(out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    in_path = sys.argv[1] if len(sys.argv) > 1 else "mlfq_qlog_raw.log"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "mlfq_timeline.png"
    watermark = sys.argv[3] if len(sys.argv) > 3 else "saanvi.jain"

    by_pid = parse_qlog(in_path)
    if not by_pid:
        sys.exit(f"no QLOG lines found in {in_path}")
    plot(by_pid, out_path, watermark)
