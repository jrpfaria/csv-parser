#!/usr/bin/env python3
"""
Benchmark report generator for csv-parser.
Reads bench_results.txt and produces:
  - bench_report.txt   (human-readable summary)
  - bench_report.csv   (machine-readable table)
  - bench_phases.csv   (per-phase breakdown)

Usage: python3 report.py [bench_results.txt]
"""
import sys
import os
from collections import defaultdict, OrderedDict

def parse_results(path):
    """Parse the bench_results.txt file into structured records."""
    records = []
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("# date:"):
                meta["date"] = line.split(":", 1)[1].strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            # Support old format (file,workers,mode,...) and new (file,parser,workers,mode,...)
            if len(parts) >= 11:
                # New format: file,parser,workers,mode,best,avg,mmap,scan,dispatch,parse,total
                records.append({
                    "file": parts[0],
                    "parser": parts[1],
                    "workers": int(parts[2]),
                    "mode": parts[3],
                    "best_wall_s": float(parts[4]),
                    "avg_wall_s": float(parts[5]),
                    "mmap_s": float(parts[6]),
                    "scan_s": float(parts[7]),
                    "dispatch_s": float(parts[8]),
                    "parse_s": float(parts[9]),
                    "total_s": float(parts[10]),
                })
            elif len(parts) >= 10:
                # Old 10-column format: file,workers,mode,best,avg,mmap,scan,dispatch,parse,total
                records.append({
                    "file": parts[0],
                    "parser": "scalar",
                    "workers": int(parts[1]),
                    "mode": parts[2],
                    "best_wall_s": float(parts[3]),
                    "avg_wall_s": float(parts[4]),
                    "mmap_s": float(parts[5]),
                    "scan_s": float(parts[6]),
                    "dispatch_s": float(parts[7]),
                    "parse_s": float(parts[8]),
                    "total_s": float(parts[9]),
                })
            elif len(parts) >= 9:
                # Legacy 9-column format
                records.append({
                    "file": parts[0],
                    "parser": "scalar",
                    "workers": int(parts[1]),
                    "mode": parts[2],
                    "best_wall_s": float(parts[3]),
                    "avg_wall_s": float(parts[3]),
                    "mmap_s": float(parts[4]),
                    "scan_s": float(parts[5]),
                    "dispatch_s": float(parts[6]),
                    "parse_s": float(parts[7]),
                    "total_s": float(parts[8]),
                })
    return meta, records


def file_size_label(filename):
    """Map bench filename to a human label."""
    labels = {
        "bench_100.csv": "100 rows",
        "bench_1k.csv": "1K rows",
        "bench_10k.csv": "10K rows",
        "bench_100k.csv": "100K rows",
    }
    return labels.get(filename, filename)


WORKER_CONFIGS = [1, 2, 4, 6, 8]
WORKER_LABELS = {
    1: "Single",
    2: "Dist+W(2)",
    4: "Dist+S(4)",
    6: "Dist+S(6)",
    8: "Dist+S(8)",
}


def generate_text_report(meta, records, out_path):
    """Generate a human-readable text report."""
    with open(out_path, "w") as f:
        f.write("=" * 70 + "\n")
        f.write("  csv-parser Benchmark Report\n")
        if "date" in meta:
            f.write(f"  Generated: {meta['date']}\n")
        f.write("=" * 70 + "\n\n")

        # Group by file and parser
        by_file_parser = defaultdict(lambda: defaultdict(list))
        for r in records:
            by_file_parser[r["file"]][r["parser"]].append(r)
        parser_labels = sorted({r["parser"] for r in records})

        # Summary table — best, for each parser
        col_w = 12
        hdr_labels = [WORKER_LABELS[w] for w in WORKER_CONFIGS]
        hdr = f"{'File':<16}" + "".join(f"{l:>{col_w}}" for l in hdr_labels)
        sep = "-" * len(hdr)

        for plabel in parser_labels:
            pname = plabel.upper()
            f.write(f"BEST wall-clock time — {pname} parser (seconds)\n")
            f.write(sep + "\n")
            f.write(hdr + "\n")
            f.write(sep + "\n")
            for fname in ["bench_100.csv", "bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]:
                if fname not in by_file_parser or plabel not in by_file_parser[fname]:
                    continue
                row = {}
                for r in by_file_parser[fname][plabel]:
                    row[r["workers"]] = r["best_wall_s"]
                f.write(f"{file_size_label(fname):<16}")
                for w in WORKER_CONFIGS:
                    if w in row:
                        f.write(f"{row[w]:>{col_w}.4f}")
                    else:
                        f.write(f"{'n/a':>{col_w}}")
                f.write("\n")
            f.write(sep + "\n\n")

        # Speedup table — SIMD vs scalar, grouped by approach
        approaches = sorted({p.rsplit("-", 1)[0] for p in parser_labels if "-" in p})
        for approach in approaches:
            scalar_label = f"{approach}-scalar"
            simd_label = f"{approach}-simd"
            if scalar_label not in parser_labels or simd_label not in parser_labels:
                continue

            f.write(f"SIMD SPEEDUP vs SCALAR (best) — {approach.upper()}\n")
            f.write(sep + "\n")
            f.write(hdr + "\n")
            f.write(sep + "\n")
            for fname in ["bench_100.csv", "bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]:
                scalar_data = {r["workers"]: r for r in by_file_parser.get(fname, {}).get(scalar_label, [])}
                simd_data = {r["workers"]: r for r in by_file_parser.get(fname, {}).get(simd_label, [])}
                if not scalar_data or not simd_data:
                    continue
                f.write(f"{file_size_label(fname):<16}")
                for w in WORKER_CONFIGS:
                    if w in scalar_data and w in simd_data and simd_data[w]["best_wall_s"] > 0:
                        sp = scalar_data[w]["best_wall_s"] / simd_data[w]["best_wall_s"]
                        f.write(f"{sp:>{col_w - 1}.2f}x")
                    else:
                        f.write(f"{'n/a':>{col_w}}")
                f.write("\n")
            f.write(sep + "\n\n")

        # Per-phase breakdown
        f.write("PHASE BREAKDOWN (seconds) — from internal timing\n")
        f.write("-" * 88 + "\n")
        f.write(f"{'File':<18} {'P':>5} {'W':>2} {'Mode':<18} "
                f"{'mmap':>8} {'scan':>8} {'disp':>8} {'parse':>8} {'total':>8}\n")
        f.write("-" * 88 + "\n")
        for r in records:
            f.write(f"{r['file']:<18} {r['parser']:>5} {r['workers']:>2} {r['mode']:<18} "
                    f"{r['mmap_s']:>8.5f} {r['scan_s']:>8.5f} "
                    f"{r['dispatch_s']:>8.5f} {r['parse_s']:>8.5f} "
                    f"{r['total_s']:>8.5f}\n")
        f.write("-" * 88 + "\n\n")

        # Analysis
        f.write("ANALYSIS\n")
        f.write("-" * 70 + "\n")
        big = [r for r in records if r["file"] == "bench_100k.csv"]
        if big:
            single = [r for r in big if r["workers"] == 1]
            by_w = {r["workers"]: r for r in big}

            if single:
                s = single[0]
                f.write(f"  Single-threaded (100K): {s['best_wall_s']:.4f}s wall, "
                        f"{s['parse_s']:.4f}s parse\n")
                size_mb = 8.1
                f.write(f"  Throughput: {size_mb / s['best_wall_s']:.1f} MB/s\n")

            for w in WORKER_CONFIGS[1:]:
                if w in by_w:
                    d = by_w[w]
                    f.write(f"  {WORKER_LABELS[w]} (100K): {d['best_wall_s']:.4f}s wall, "
                            f"scan={d['scan_s']:.4f}s parse={d['parse_s']:.4f}s\n")
                    if d['total_s'] > 0:
                        f.write(f"    Scan overhead: {d['scan_s'] / d['total_s'] * 100:.1f}% of total\n")

            # Best multi-threaded config
            multi = [r for r in big if r["workers"] > 1]
            if multi and single:
                best = min(multi, key=lambda r: r["best_wall_s"])
                speedup = single[0]["best_wall_s"] / best["best_wall_s"]
                f.write(f"\n  Best config: {WORKER_LABELS[best['workers']]} "
                        f"({speedup:.2f}x speedup)\n")
        f.write("-" * 70 + "\n")

    print(f"  Text report:  {out_path}")


def generate_csv_report(records, out_path):
    """Generate a CSV with summary data."""
    with open(out_path, "w") as f:
        f.write("file,parser,workers,mode,best_wall_s,avg_wall_s\n")
        for r in records:
            f.write(f"{r['file']},{r['parser']},{r['workers']},{r['mode']},"
                    f"{r['best_wall_s']:.6f},{r['avg_wall_s']:.6f}\n")
    print(f"  CSV summary:  {out_path}")


def generate_phases_csv(records, out_path):
    """Generate a CSV with per-phase breakdown."""
    with open(out_path, "w") as f:
        f.write("file,parser,workers,mode,mmap_s,scan_s,dispatch_s,parse_s,total_s\n")
        for r in records:
            f.write(f"{r['file']},{r['parser']},{r['workers']},{r['mode']},"
                    f"{r['mmap_s']:.6f},{r['scan_s']:.6f},"
                    f"{r['dispatch_s']:.6f},{r['parse_s']:.6f},"
                    f"{r['total_s']:.6f}\n")
    print(f"  Phase detail: {out_path}")


def generate_pdf_report(meta, records, out_path):
    """Generate a PDF with speedup graphs for each phase and overall."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages

    FILES = ["bench_100.csv", "bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]
    PHASES = ["scan_s", "dispatch_s", "parse_s", "total_s"]
    PHASE_LABELS = {"scan_s": "Scan", "dispatch_s": "Dispatch",
                    "parse_s": "Parse", "total_s": "Total (internal)"}
    COLORS = {
        ("bench_100.csv", "scalar"): "#aec7e8", ("bench_100.csv", "simd"): "#1f77b4",
        ("bench_1k.csv", "scalar"): "#ffbb78", ("bench_1k.csv", "simd"): "#ff7f0e",
        ("bench_10k.csv", "scalar"): "#98df8a", ("bench_10k.csv", "simd"): "#2ca02c",
        ("bench_100k.csv", "scalar"): "#ff9896", ("bench_100k.csv", "simd"): "#d62728",
    }
    MARKERS = {
        ("bench_100.csv", "scalar"): "o", ("bench_100.csv", "simd"): "o",
        ("bench_1k.csv", "scalar"): "s", ("bench_1k.csv", "simd"): "s",
        ("bench_10k.csv", "scalar"): "^", ("bench_10k.csv", "simd"): "^",
        ("bench_100k.csv", "scalar"): "D", ("bench_100k.csv", "simd"): "D",
    }
    LSTYLES = {"scalar": "--", "simd": "-"}

    # Index: (file, parser) -> workers -> record
    idx = defaultdict(dict)
    for r in records:
        idx[(r["file"], r["parser"])][r["workers"]] = r

    workers = sorted({r["workers"] for r in records})
    parsers_found = sorted({r["parser"] for r in records})

    def scalar_baseline_record(fname):
        for cand in ["control-scalar", "fsm-scalar", "lut-scalar", "scalar"]:
            rec = idx.get((fname, cand), {}).get(1)
            if rec:
                return rec
        for plabel in parsers_found:
            if plabel.endswith("scalar"):
                rec = idx.get((fname, plabel), {}).get(1)
                if rec:
                    return rec
        return None

    with PdfPages(out_path) as pdf:
        # --- Page 1: Overall wall-clock speedup (both parsers) ---
        fig, (ax_best, ax_avg) = plt.subplots(1, 2, figsize=(14, 6))
        fig.suptitle("csv-parser Benchmark Report", fontsize=16, fontweight="bold")

        for ax, metric, title in [
            (ax_best, "best_wall_s", "Overall Speedup (best)"),
            (ax_avg, "avg_wall_s", "Overall Speedup (average)"),
        ]:
            subtitle = f"{title}  —  {meta['date']}" if "date" in meta else title
            ax.set_title(subtitle, fontsize=11)
            ax.set_xlabel("Workers")
            ax.set_ylabel("Speedup vs scalar single-threaded")
            ax.set_yscale("log", base=2)
            ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.6)

            for fname in FILES:
                base = scalar_baseline_record(fname)
                base_val = base.get(metric, 0) if base else 0
                if base_val <= 0:
                    continue
                for plabel in parsers_found:
                    key = (fname, plabel)
                    if key not in idx:
                        continue
                    xs = [w for w in workers if w in idx[key]]
                    ys = [base_val / idx[key][w][metric] for w in xs]
                    color = COLORS.get(key, "#333")
                    marker = MARKERS.get(key, "o")
                    ls = LSTYLES.get(plabel, "-")
                    ax.plot(xs, ys, marker=marker, color=color, linestyle=ls,
                            label=f"{file_size_label(fname)} ({plabel})",
                            linewidth=2, markersize=6)

            ax.set_xticks(workers)
            ax.legend(loc="upper left", fontsize=7, ncol=2)
            ax.grid(True, alpha=0.3, which="both")
            ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{v:.2f}x"))

        fig.tight_layout(rect=[0, 0, 1, 0.93])
        pdf.savefig(fig)
        plt.close(fig)

        # --- Page 2: Per-phase speedup (2x2 grid) ---
        fig, axes = plt.subplots(2, 2, figsize=(12, 9))
        fig.suptitle("Per-Phase Speedup vs Scalar Single-Threaded", fontsize=14, fontweight="bold")

        for ax, phase in zip(axes.flat, PHASES):
            ax.set_title(PHASE_LABELS[phase], fontsize=11)
            ax.set_xlabel("Workers")
            ax.set_ylabel("Speedup")
            ax.set_yscale("log", base=2)
            ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.6)

            for fname in FILES:
                base = scalar_baseline_record(fname)
                base_val = base.get(phase, 0) if base else 0
                if base_val <= 0:
                    continue
                for plabel in parsers_found:
                    key = (fname, plabel)
                    if key not in idx:
                        continue
                    xs = [w for w in workers if w in idx[key] and idx[key][w].get(phase, 0) > 0]
                    ys = [base_val / idx[key][w][phase] for w in xs]
                    color = COLORS.get(key, "#333")
                    marker = MARKERS.get(key, "o")
                    ls = LSTYLES.get(plabel, "-")
                    ax.plot(xs, ys, marker=marker, color=color, linestyle=ls,
                            label=f"{file_size_label(fname)} ({plabel})",
                            linewidth=1.5, markersize=5)

            ax.set_xticks(workers)
            ax.legend(fontsize=6, loc="upper left", ncol=2)
            ax.grid(True, alpha=0.3, which="both")
            ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{v:.1f}x"))

        fig.tight_layout(rect=[0, 0, 1, 0.95])
        pdf.savefig(fig)
        plt.close(fig)

        # --- Page 3: Absolute time breakdown (stacked bars, scalar vs simd side by side) ---
        fig, axes = plt.subplots(2, 2, figsize=(12, 9))
        fig.suptitle("Phase Time Breakdown (absolute seconds)", fontsize=14, fontweight="bold")

        stack_phases = ["mmap_s", "scan_s", "dispatch_s", "parse_s"]
        stack_colors = ["#aec7e8", "#ffbb78", "#98df8a", "#ff9896"]
        stack_labels = ["mmap", "scan", "dispatch", "parse"]

        for ax, fname in zip(axes.flat, FILES):
            ax.set_title(file_size_label(fname), fontsize=11)
            ax.set_xlabel("Config")
            ax.set_ylabel("Time (s)")

            bar_labels = []
            for plabel in parsers_found:
                key = (fname, plabel)
                if key not in idx:
                    continue
                for w in workers:
                    if w in idx[key]:
                        bar_labels.append(f"{plabel[0]}{w}")

            x_pos = list(range(len(bar_labels)))
            bottoms = [0.0] * len(bar_labels)
            for sp, sc, sl in zip(stack_phases, stack_colors, stack_labels):
                vals = []
                for plabel in parsers_found:
                    key = (fname, plabel)
                    if key not in idx:
                        continue
                    for w in workers:
                        if w in idx[key]:
                            vals.append(idx[key][w].get(sp, 0))
                ax.bar(x_pos, vals, bottom=bottoms,
                       color=sc, label=sl, edgecolor="white", linewidth=0.5)
                bottoms = [b + v for b, v in zip(bottoms, vals)]

            ax.set_xticks(x_pos)
            ax.set_xticklabels(bar_labels, fontsize=7, rotation=45)
            ax.legend(fontsize=7, loc="upper right")
            ax.grid(True, alpha=0.3, axis="y")

        fig.tight_layout(rect=[0, 0, 1, 0.95])
        pdf.savefig(fig)
        plt.close(fig)

        # --- Page 4: Parse-only speedup (the real parallel work) ---
        fig, ax = plt.subplots(figsize=(10, 6))
        ax.set_title("Parse Phase Speedup (parallel work only)", fontsize=13, fontweight="bold")
        ax.set_xlabel("Workers")
        ax.set_ylabel("Speedup vs scalar single-threaded parse")
        ax.set_yscale("log", base=2)
        ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.6)
        ax.plot(workers, workers, color="gray", linestyle=":", linewidth=1, alpha=0.5, label="Ideal linear")

        for fname in FILES:
            base = idx.get((fname, "scalar"), {}).get(1, {}).get("parse_s", 0)
            if base <= 0:
                continue
            for plabel in parsers_found:
                key = (fname, plabel)
                if key not in idx:
                    continue
                xs = [w for w in workers if w in idx[key] and idx[key][w].get("parse_s", 0) > 0]
                ys = [base / idx[key][w]["parse_s"] for w in xs]
                color = COLORS.get(key, "#333")
                marker = MARKERS.get(key, "o")
                ls = LSTYLES.get(plabel, "-")
                ax.plot(xs, ys, marker=marker, color=color, linestyle=ls,
                        label=f"{file_size_label(fname)} ({plabel})",
                        linewidth=2, markersize=6)

        ax.set_xticks(workers)
        ax.legend(loc="upper left", fontsize=7, ncol=2)
        ax.grid(True, alpha=0.3, which="both")
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{v:.2f}x"))
        fig.tight_layout()
        pdf.savefig(fig)
        plt.close(fig)

    print(f"  PDF report:   {out_path}")


def main():
    no_pdf = "--no-pdf" in sys.argv
    pdf_only = "--pdf-only" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    results_path = args[0] if args else os.path.join(
        os.path.dirname(__file__), "bench_results.txt")

    if not os.path.exists(results_path):
        print(f"Error: {results_path} not found. Run bench.sh first.")
        sys.exit(1)

    meta, records = parse_results(results_path)
    if not records:
        print("Error: No benchmark records found.")
        sys.exit(1)

    out_dir = os.path.dirname(results_path)
    print(f"Generating reports from {results_path}:")
    if not pdf_only:
        generate_text_report(meta, records, os.path.join(out_dir, "bench_report.txt"))
        generate_csv_report(records, os.path.join(out_dir, "bench_report.csv"))
        generate_phases_csv(records, os.path.join(out_dir, "bench_phases.csv"))
    if not no_pdf:
        generate_pdf_report(meta, records, os.path.join(out_dir, "bench_report.pdf"))
    print("Done.")


if __name__ == "__main__":
    main()
