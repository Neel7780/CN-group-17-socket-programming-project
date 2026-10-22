#!/usr/bin/env python3
# ==============================================================================
# plot.py — Publication-Quality Result Plotting Engine
# IT305 Computer Networks Lab · Group 17 (021 & 034)
# Author: Manal Patel (Roll No. 202401112)
#
# Generates all mandated performance graphs (.png) from benchmark CSV logs:
#   * Part1_SingleThread_throughput.png
#   * Part1_MultiThread_throughput.png
#   * Part2_Case1_throughput.png
#   * Part2_Case1_overhead.png
#   * Part2_Case2_throughput.png
#   * Part2_Case2_overhead.png
#   * Part2_Case2Enhanced_throughput.png
#   * Part2_Case2Enhanced_overhead.png
#   * Part2_Comparison_throughput.png
# ==============================================================================

import sys
import os
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# Setup paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, "Results")
RESULTS_DIR = os.path.abspath(RESULTS_DIR)

os.makedirs(RESULTS_DIR, exist_ok=True)

# Visual theme configurations
plt.rcParams.update({
    "figure.dpi": 300,
    "font.size": 11,
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans", "Helvetica", "Arial"],
    "axes.labelsize": 12,
    "axes.titlesize": 13,
    "axes.titleweight": "bold",
    "axes.grid": True,
    "grid.alpha": 0.35,
    "grid.linestyle": "--",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "lines.linewidth": 2.2,
    "lines.markersize": 7,
})

PALETTE = {
    "single": "#3949AB",       # Indigo
    "multi": "#00897B",        # Teal
    "case1": "#E53935",        # Coral Red
    "case2": "#1E88E5",        # Vibrant Blue
    "enhanced": "#43A047",     # Green
    "accent": "#FB8C00",       # Amber
    "dark": "#263238",
}

def save_fig(fig, filename):
    out_path = os.path.join(RESULTS_DIR, filename)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"[+] Saved figure: {out_path}")

def load_csv(filename):
    path = os.path.join(RESULTS_DIR, filename)
    if not os.path.exists(path):
        print(f"[!] Warning: File {filename} not found in {RESULTS_DIR}")
        return None
    try:
        df = pd.read_csv(path)
        df.columns = df.columns.str.strip()
        return df
    except Exception as e:
        print(f"[-] Error loading {filename}: {e}")
        return None

# ==============================================================================
# PART 1: Single-Threaded vs. Multi-Threaded Scalability
# ==============================================================================
def plot_part1_mode(csv_name, png_name, mode_title, color):
    df = load_csv(csv_name)
    if df is None or df.empty:
        return

    # Check columns
    if "n_clients" not in df.columns:
        print(f"[!] 'n_clients' missing in {csv_name}")
        return

    df["n_clients"] = pd.to_numeric(df["n_clients"], errors="coerce")

    # If Part1 raw client metrics row: throughput_mbps, duration_ms, payload_bytes
    if "throughput_mbps" in df.columns:
        df["throughput_mbps"] = pd.to_numeric(df["throughput_mbps"], errors="coerce")
    else:
        # compute from payload and duration
        df["throughput_mbps"] = (df["payload_bytes"] / 1e6) / (df["duration_ms"] / 1000.0)

    # Compute aggregate and per-client metrics
    grouped = df.groupby("n_clients").agg(
        mean_client_tput=("throughput_mbps", "mean"),
        std_client_tput=("throughput_mbps", "std"),
        total_payload=("payload_bytes", "sum"),
        max_duration_ms=("duration_ms", "max"),
    ).reset_index()

    # Aggregate throughput across all concurrent clients
    grouped["agg_throughput_mbps"] = (
        (grouped["total_payload"] / 1e6) / (grouped["max_duration_ms"] / 1000.0)
    ).fillna(grouped["mean_client_tput"] * grouped["n_clients"])

    fig, ax1 = plt.subplots(figsize=(7.5, 4.5))

    ax1.plot(grouped["n_clients"], grouped["agg_throughput_mbps"],
             marker="o", color=color, label="Aggregate Throughput", linewidth=2.5)
    ax1.plot(grouped["n_clients"], grouped["mean_client_tput"],
             marker="s", linestyle="--", color=PALETTE["accent"],
             label="Per-Client Mean Throughput", linewidth=2.0)

    ax1.set_xlabel("Number of Concurrent Clients (N)")
    ax1.set_ylabel("Throughput (MB/s)")
    ax1.set_title(f"Part I: {mode_title} Server Scalability")
    ax1.set_xticks(grouped["n_clients"].unique())
    ax1.legend(loc="upper left" if "Single" in mode_title else "center right", frameon=True)

    # Annotate plateau / scaling
    if "Single" in mode_title:
        ax1.text(0.95, 0.20, "Single thread serializes clients;\nAggregate throughput plateaus",
                 transform=ax1.transAxes, ha="right", va="bottom",
                 bbox=dict(boxstyle="round,pad=0.5", facecolor="#EEEEEE", alpha=0.8, edgecolor="#CCCCCC"))
    else:
        ax1.text(0.05, 0.70, "Multi-thread overlaps transfers;\nHigh aggregate throughput sustained",
                 transform=ax1.transAxes, ha="left", va="top",
                 bbox=dict(boxstyle="round,pad=0.5", facecolor="#E0F2F1", alpha=0.8, edgecolor="#80CBC4"))

    save_fig(fig, png_name)

# ==============================================================================
# PART 2: Throughput vs. Fault Probability
# ==============================================================================
def plot_part2_throughput(csv_name, png_name, case_title, color):
    df = load_csv(csv_name)
    if df is None or df.empty:
        return

    df["fault_prob"] = pd.to_numeric(df["fault_prob"], errors="coerce")
    df["throughput_mbps"] = pd.to_numeric(df["throughput_mbps"], errors="coerce")

    grouped = df.groupby("fault_prob").agg(
        mean_tput=("throughput_mbps", "mean"),
        std_tput=("throughput_mbps", "std"),
    ).reset_index()

    fig, ax = plt.subplots(figsize=(7.5, 4.5))
    ax.errorbar(grouped["fault_prob"], grouped["mean_tput"],
                yerr=grouped["std_tput"].fillna(0),
                marker="o", color=color, linewidth=2.5, capsize=5, capthick=1.5,
                label="Measured Throughput")

    ax.set_xlabel("Fault Injection Probability (p)")
    ax.set_ylabel("Effective Goodput (MB/s)")
    ax.set_title(f"Part II: {case_title} — Throughput vs. Fault Rate")
    ax.set_xticks([0.0, 0.2, 0.5, 0.8])
    ax.set_xlim(-0.05, 0.85)

    save_fig(fig, png_name)

# ==============================================================================
# PART 2: Wire Overhead vs. Fault Probability
# ==============================================================================
def plot_part2_overhead(csv_name, png_name, case_title, color):
    df = load_csv(csv_name)
    if df is None or df.empty:
        return

    df["fault_prob"] = pd.to_numeric(df["fault_prob"], errors="coerce")

    # Wire overhead ratio = wire_bytes / useful_bytes
    if "wire_bytes" in df.columns and "useful_bytes" in df.columns:
        df["wire_bytes"] = pd.to_numeric(df["wire_bytes"], errors="coerce")
        df["useful_bytes"] = pd.to_numeric(df["useful_bytes"], errors="coerce")
        df["overhead_ratio"] = df["wire_bytes"] / df["useful_bytes"].replace(0, np.nan)
    elif "redundant_bytes" in df.columns and "useful_bytes" in df.columns:
        df["overhead_ratio"] = 1.0 + (df["redundant_bytes"] / df["useful_bytes"].replace(0, np.nan))
    else:
        df["overhead_ratio"] = 1.0

    grouped = df.groupby("fault_prob").agg(
        mean_ovhd=("overhead_ratio", "mean"),
        std_ovhd=("overhead_ratio", "std"),
    ).reset_index()

    fig, ax = plt.subplots(figsize=(7.5, 4.5))

    bar_width = 0.08
    ax.bar(grouped["fault_prob"], grouped["mean_ovhd"],
           width=bar_width, color=color, alpha=0.85, edgecolor=PALETTE["dark"],
           yerr=grouped["std_ovhd"].fillna(0), capsize=4,
           label="Measured Wire/Payload Ratio")

    ax.axhline(1.0, color="#757575", linestyle="--", linewidth=1.5, label="Ideal Zero-Overhead Ratio (1.0)")

    ax.set_xlabel("Fault Injection Probability (p)")
    ax.set_ylabel("Overhead Factor (Wire Bytes / Useful Bytes)")
    ax.set_title(f"Part II: {case_title} — Transmission Overhead")
    ax.set_xticks([0.0, 0.2, 0.5, 0.8])
    ax.set_xlim(-0.08, 0.88)
    ax.legend(loc="upper left")

    save_fig(fig, png_name)

# ==============================================================================
# PART 2: Comprehensive Multi-Case Comparison
# ==============================================================================
def plot_comparison():
    cases = [
        ("Part2_Case1_throughput.csv", "Case 1: Naive Retry (Restart)", PALETTE["case1"], "s--"),
        ("Part2_Case2_throughput.csv", "Case 2: Session Checkpoint Resume", PALETTE["case2"], "o-"),
        ("Part2_Case2Enhanced_throughput.csv", "Case 2 Enhanced: 4x Parallel TCP", PALETTE["enhanced"], "^-"),
    ]

    fig, ax = plt.subplots(figsize=(8.5, 5.0))
    plotted = 0

    for csv_file, label, color, style in cases:
        df = load_csv(csv_file)
        if df is None or df.empty:
            continue
        df["fault_prob"] = pd.to_numeric(df["fault_prob"], errors="coerce")
        df["throughput_mbps"] = pd.to_numeric(df["throughput_mbps"], errors="coerce")

        grouped = df.groupby("fault_prob")["throughput_mbps"].mean().reset_index()
        ax.plot(grouped["fault_prob"], grouped["throughput_mbps"],
                style, color=color, label=label, linewidth=2.5, markersize=8)
        plotted += 1

    if plotted == 0:
        plt.close(fig)
        return

    ax.set_xlabel("Fault Injection Probability (p)", fontweight="bold")
    ax.set_ylabel("Effective Delivery Throughput (MB/s)", fontweight="bold")
    ax.set_title("Part II: Comparative Throughput Across Fault-Tolerant Strategies", pad=12)
    ax.set_xticks([0.0, 0.2, 0.5, 0.8])
    ax.set_xlim(-0.05, 0.85)
    ax.legend(loc="upper right", framealpha=0.95, edgecolor="#CCCCCC")

    save_fig(fig, "Part2_Comparison_throughput.png")

# ==============================================================================
# MAIN EXECUTION
# ==============================================================================
if __name__ == "__main__":
    print("[*] Generating all assignment performance figures in:", RESULTS_DIR)

    # Part 1
    plot_part1_mode("Part1_SingleThread_throughput.csv", "Part1_SingleThread_throughput.png",
                    "Single-Threaded", PALETTE["single"])
    plot_part1_mode("Part1_MultiThread_throughput.csv", "Part1_MultiThread_throughput.png",
                    "Multi-Threaded", PALETTE["multi"])

    # Part 2 Case 1
    plot_part2_throughput("Part2_Case1_throughput.csv", "Part2_Case1_throughput.png",
                          "Case 1 (Naive Retry)", PALETTE["case1"])
    plot_part2_overhead("Part2_Case1_throughput.csv", "Part2_Case1_overhead.png",
                        "Case 1 (Naive Retry)", PALETTE["case1"])

    # Part 2 Case 2
    plot_part2_throughput("Part2_Case2_throughput.csv", "Part2_Case2_throughput.png",
                          "Case 2 (Session Resumption)", PALETTE["case2"])
    plot_part2_overhead("Part2_Case2_throughput.csv", "Part2_Case2_overhead.png",
                        "Case 2 (Session Resumption)", PALETTE["case2"])

    # Part 2 Case 2 Enhanced
    plot_part2_throughput("Part2_Case2Enhanced_throughput.csv", "Part2_Case2Enhanced_throughput.png",
                          "Case 2 Enhanced (Parallel TCP)", PALETTE["enhanced"])
    plot_part2_overhead("Part2_Case2Enhanced_throughput.csv", "Part2_Case2Enhanced_overhead.png",
                        "Case 2 Enhanced (Parallel TCP)", PALETTE["enhanced"])

    # Comparison Overlay
    plot_comparison()

    print("[+] All 9 visualization artifacts successfully created.")
