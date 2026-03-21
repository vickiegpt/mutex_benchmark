#!/usr/bin/env python3
"""
Plot experiment results for the mutex benchmark paper.
Generates throughput vs thread count figures for each benchmark mode.

Usage:
    python3 plot_results.py [--bench max|min|both] [--modes dram,cachedsc] [--outdir data/figs]
"""

import os
import sys
import glob
import argparse
import numpy as np

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
except ImportError:
    print("ERROR: matplotlib not installed. Install with: pip install matplotlib")
    sys.exit(1)

# Lock display names for paper
LOCK_DISPLAY = {
    "bakery": "Bakery",
    "boulangerie": "Boulangerie",
    "lamport": "Lamport Fast",
    "linear_lamport_elevator": "Elevator-LF-Array",
    "linear_bl_elevator": "Elevator-BL-Array",
    "tree_lamport_elevator": "Elevator-LF-Tree",
    "tree_bl_elevator": "Elevator-BL-Tree",
    "peterson": "Peterson",
    "knuth": "Knuth",
    "spin": "Spin",
    "exp_spin": "Exp Spin",
    "ticket": "Ticket",
    "mcs": "MCS",
}

# Lock categories
HW_LOCKS = {"spin", "exp_spin", "ticket", "mcs"}
SW_LOCKS = {"bakery", "boulangerie", "lamport", "linear_lamport_elevator",
            "linear_bl_elevator", "tree_lamport_elevator", "tree_bl_elevator",
            "peterson", "knuth"}

# Colors matching paper style
COLORS = {
    "MCS": "#e6194b",
    "Ticket": "#3cb44b",
    "Spin": "#ffe119",
    "Exp Spin": "#f58231",
    "Elevator-BL-Array": "#4363d8",
    "Elevator-LF-Array": "#911eb4",
    "Elevator-BL-Tree": "#46f0f0",
    "Elevator-LF-Tree": "#f032e6",
    "Bakery": "#bcf60c",
    "Boulangerie": "#fabebe",
    "Lamport Fast": "#008080",
    "Peterson": "#9a6324",
    "Knuth": "#800000",
}

MARKERS = {
    "MCS": "o", "Ticket": "s", "Spin": "^", "Exp Spin": "D",
    "Elevator-BL-Array": "*", "Elevator-LF-Array": "X",
    "Elevator-BL-Tree": "v", "Elevator-LF-Tree": "<",
    "Bakery": ">", "Boulangerie": "P", "Lamport Fast": "H",
    "Peterson": "d", "Knuth": "p",
}


def parse_thread_level_csv(filepath):
    """Parse thread-level CSV: thread_id, runtime, iterations"""
    total_iters = 0
    runtime = 0
    try:
        with open(filepath) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(",")
                if len(parts) >= 3:
                    runtime = float(parts[1])
                    total_iters += int(parts[2])
    except Exception:
        return None
    if runtime <= 0:
        return None
    return total_iters / runtime  # throughput = total iterations / runtime


def parse_min_csv(filepath):
    """Parse min contention CSV: thread_id, iteration, time"""
    count = 0
    total_time = 0
    try:
        with open(filepath) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(",")
                if len(parts) >= 3:
                    total_time += float(parts[2])
                    count += 1
    except Exception:
        return None
    if count == 0:
        return None
    return count / total_time if total_time > 0 else None


def load_data(datadir, bench, mode, locks):
    """Load all data for a given bench type and mode.
    Returns: {lock_name: {thread_count: [throughputs across iterations]}}
    """
    data = {}
    for lock in locks:
        data[lock] = {}
        # Find all files for this lock/mode
        pattern = f"{datadir}/{lock}-*-{bench}-{mode}-threads=*.csv"
        files = glob.glob(pattern)
        for f in files:
            # Extract thread count from filename
            basename = os.path.basename(f)
            try:
                threads_part = basename.split("threads=")[1].replace(".csv", "")
                threads = int(threads_part)
            except (IndexError, ValueError):
                continue

            if bench == "max":
                throughput = parse_thread_level_csv(f)
            else:
                throughput = parse_min_csv(f)

            if throughput is not None:
                if threads not in data[lock]:
                    data[lock][threads] = []
                data[lock][threads].append(throughput)

    return data


def plot_throughput(data, title, outpath, log_scale=True):
    """Plot throughput vs thread count for all locks."""
    fig, ax = plt.subplots(1, 1, figsize=(8, 5))

    for lock_code, thread_data in sorted(data.items()):
        if not thread_data:
            continue
        display_name = LOCK_DISPLAY.get(lock_code, lock_code)
        color = COLORS.get(display_name, "#333333")
        marker = MARKERS.get(display_name, "o")

        threads_sorted = sorted(thread_data.keys())
        medians = [np.median(thread_data[t]) for t in threads_sorted]
        mins = [np.min(thread_data[t]) for t in threads_sorted]
        maxs = [np.max(thread_data[t]) for t in threads_sorted]

        ax.plot(threads_sorted, medians, marker=marker, color=color,
                label=display_name, linewidth=1.5, markersize=5)
        ax.fill_between(threads_sorted, mins, maxs, alpha=0.15, color=color)

    ax.set_xlabel("Thread Count", fontsize=11)
    ax.set_ylabel("Throughput (acquisitions/second)", fontsize=11)
    ax.set_title(title, fontsize=12)
    if log_scale:
        ax.set_yscale("log")
    ax.grid(True, linestyle='--', linewidth=0.5, alpha=0.7)
    ax.legend(loc='center left', bbox_to_anchor=(1.02, 0.5), fontsize=8)
    plt.tight_layout()
    plt.savefig(outpath, dpi=200, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {outpath}")


def plot_comparison(dram_data, sc_data, title, outpath):
    """Plot DRAM vs Cached-SC comparison (software locks only)."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    # Plot DRAM
    for lock_code, thread_data in sorted(dram_data.items()):
        if not thread_data or lock_code in HW_LOCKS:
            continue
        display_name = LOCK_DISPLAY.get(lock_code, lock_code)
        color = COLORS.get(display_name, "#333333")
        marker = MARKERS.get(display_name, "o")
        threads_sorted = sorted(thread_data.keys())
        medians = [np.median(thread_data[t]) for t in threads_sorted]
        ax1.plot(threads_sorted, medians, marker=marker, color=color,
                 label=display_name, linewidth=1.5, markersize=5)

    ax1.set_xlabel("Thread Count")
    ax1.set_ylabel("Throughput (acq/s)")
    ax1.set_title("DRAM Baseline (no flush)")
    ax1.set_yscale("log")
    ax1.grid(True, linestyle='--', linewidth=0.5, alpha=0.7)
    ax1.legend(fontsize=7)

    # Plot Cached-SC
    for lock_code, thread_data in sorted(sc_data.items()):
        if not thread_data or lock_code in HW_LOCKS:
            continue
        display_name = LOCK_DISPLAY.get(lock_code, lock_code)
        color = COLORS.get(display_name, "#333333")
        marker = MARKERS.get(display_name, "o")
        threads_sorted = sorted(thread_data.keys())
        medians = [np.median(thread_data[t]) for t in threads_sorted]
        ax2.plot(threads_sorted, medians, marker=marker, color=color,
                 label=display_name, linewidth=1.5, markersize=5)

    ax2.set_xlabel("Thread Count")
    ax2.set_ylabel("Throughput (acq/s)")
    ax2.set_title("Cached-SC (clflushopt + sfence)")
    ax2.set_yscale("log")
    ax2.grid(True, linestyle='--', linewidth=0.5, alpha=0.7)
    ax2.legend(fontsize=7)

    fig.suptitle(title, fontsize=13)
    plt.tight_layout()
    plt.savefig(outpath, dpi=200, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {outpath}")


def print_table(dram_data, sc_data, bench):
    """Print a summary table of throughput ratios."""
    print(f"\n{'='*80}")
    print(f"  {bench.upper()} Contention: DRAM vs Cached-SC Throughput (32 threads)")
    print(f"{'='*80}")
    print(f"  {'Lock':<25} {'DRAM':>12} {'Cached-SC':>12} {'Slowdown':>10}")
    print(f"  {'-'*25} {'-'*12} {'-'*12} {'-'*10}")

    for lock in sorted(LOCK_DISPLAY.keys()):
        if lock in HW_LOCKS:
            continue
        # Find closest thread count to 32
        dram_t = dram_data.get(lock, {})
        sc_t = sc_data.get(lock, {})
        for target in [32, 24, 48, 16, 64, 8, 1]:
            if target in dram_t and target in sc_t:
                dram_val = np.median(dram_t[target])
                sc_val = np.median(sc_t[target])
                ratio = dram_val / sc_val if sc_val > 0 else float('inf')
                name = LOCK_DISPLAY[lock]
                print(f"  {name:<25} {dram_val:>12,.0f} {sc_val:>12,.0f} {ratio:>9.1f}x")
                break


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", default="both", choices=["max", "min", "both"])
    parser.add_argument("--modes", default="dram,cachedsc")
    parser.add_argument("--datadir", default="data/generated")
    parser.add_argument("--outdir", default="data/figs")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    modes = args.modes.split(",")
    locks = list(LOCK_DISPLAY.keys())
    benches = ["max", "min"] if args.bench == "both" else [args.bench]

    for bench in benches:
        for mode in modes:
            print(f"\nLoading {bench} contention, mode={mode}...")
            data = load_data(args.datadir, bench, mode, locks)

            # Count data points
            total = sum(len(v) for td in data.values() for v in td.values())
            if total == 0:
                print(f"  No data found for {bench}/{mode}")
                continue
            print(f"  Found {total} data points")

            plot_throughput(
                data,
                f"{bench.title()} Contention: {mode.upper()} Baseline",
                f"{args.outdir}/{bench}_{mode}_throughput.png"
            )

        # If both modes available, generate comparison
        if "dram" in modes and "cachedsc" in modes:
            dram_data = load_data(args.datadir, bench, "dram", locks)
            sc_data = load_data(args.datadir, bench, "cachedsc", locks)
            dram_total = sum(len(v) for td in dram_data.values() for v in td.values())
            sc_total = sum(len(v) for td in sc_data.values() for v in td.values())
            if dram_total > 0 and sc_total > 0:
                plot_comparison(
                    dram_data, sc_data,
                    f"{bench.title()} Contention: DRAM vs Cached-SC",
                    f"{args.outdir}/{bench}_dram_vs_cachedsc.png"
                )
                print_table(dram_data, sc_data, bench)


if __name__ == "__main__":
    main()
