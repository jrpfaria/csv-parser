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
from collections import defaultdict

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
            if len(parts) < 9:
                continue
            records.append({
                "file": parts[0],
                "workers": int(parts[1]),
                "mode": parts[2],
                "best_wall_s": float(parts[3]),
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
        "bench_100.csv": "100 rows (8K)",
        "bench_1k.csv": "1K rows (84K)",
        "bench_10k.csv": "10K rows (828K)",
        "bench_100k.csv": "100K rows (8.1M)",
    }
    return labels.get(filename, filename)


def generate_text_report(meta, records, out_path):
    """Generate a human-readable text report."""
    with open(out_path, "w") as f:
        f.write("=" * 70 + "\n")
        f.write("  csv-parser Benchmark Report\n")
        if "date" in meta:
            f.write(f"  Generated: {meta['date']}\n")
        f.write("=" * 70 + "\n\n")

        # Group by file
        by_file = defaultdict(list)
        for r in records:
            by_file[r["file"]].append(r)

        # Summary table
        f.write("SUMMARY: Best wall-clock time (seconds)\n")
        f.write("-" * 58 + "\n")
        f.write(f"{'File':<22} {'Single':>10} {'Dist+Work':>10} {'Dist+Slv':>10}\n")
        f.write("-" * 58 + "\n")
        for fname in ["bench_100.csv", "bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]:
            if fname not in by_file:
                continue
            row = {}
            for r in by_file[fname]:
                row[r["workers"]] = r["best_wall_s"]
            f.write(f"{file_size_label(fname):<22} "
                    f"{row.get(1, 0):>10.4f} "
                    f"{row.get(2, 0):>10.4f} "
                    f"{row.get(4, 0):>10.4f}\n")
        f.write("-" * 58 + "\n\n")

        # Speedup table
        f.write("SPEEDUP vs single-threaded\n")
        f.write("-" * 48 + "\n")
        f.write(f"{'File':<22} {'Dist+Work':>12} {'Dist+Slv':>12}\n")
        f.write("-" * 48 + "\n")
        for fname in ["bench_100.csv", "bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]:
            if fname not in by_file:
                continue
            row = {}
            for r in by_file[fname]:
                row[r["workers"]] = r["best_wall_s"]
            base = row.get(1, 0)
            if base > 0:
                sp2 = base / row.get(2, base) if row.get(2) else 0
                sp4 = base / row.get(4, base) if row.get(4) else 0
                f.write(f"{file_size_label(fname):<22} "
                        f"{sp2:>11.2f}x "
                        f"{sp4:>11.2f}x\n")
        f.write("-" * 48 + "\n\n")

        # Per-phase breakdown
        f.write("PHASE BREAKDOWN (seconds) — from internal timing\n")
        f.write("-" * 78 + "\n")
        f.write(f"{'File':<18} {'W':>2} {'Mode':<18} "
                f"{'mmap':>8} {'scan':>8} {'disp':>8} {'parse':>8} {'total':>8}\n")
        f.write("-" * 78 + "\n")
        for r in records:
            f.write(f"{r['file']:<18} {r['workers']:>2} {r['mode']:<18} "
                    f"{r['mmap_s']:>8.5f} {r['scan_s']:>8.5f} "
                    f"{r['dispatch_s']:>8.5f} {r['parse_s']:>8.5f} "
                    f"{r['total_s']:>8.5f}\n")
        f.write("-" * 78 + "\n\n")

        # Analysis
        f.write("ANALYSIS\n")
        f.write("-" * 70 + "\n")
        big = [r for r in records if r["file"] == "bench_100k.csv"]
        if big:
            single = [r for r in big if r["workers"] == 1]
            dw = [r for r in big if r["workers"] == 2]
            ds = [r for r in big if r["workers"] == 4]

            if single:
                s = single[0]
                f.write(f"  Single-threaded (100K): {s['best_wall_s']:.4f}s wall, "
                        f"{s['parse_s']:.4f}s parse\n")
                size_mb = 8.1
                f.write(f"  Throughput: {size_mb / s['best_wall_s']:.1f} MB/s\n")
            if dw:
                d = dw[0]
                f.write(f"  Dist-as-worker (100K): {d['best_wall_s']:.4f}s wall, "
                        f"scan={d['scan_s']:.4f}s parse={d['parse_s']:.4f}s\n")
                f.write(f"  Scan overhead: {d['scan_s'] / d['total_s'] * 100:.1f}% of total\n")
            if ds:
                d = ds[0]
                f.write(f"  Dist+slaves (100K): {d['best_wall_s']:.4f}s wall, "
                        f"scan={d['scan_s']:.4f}s parse={d['parse_s']:.4f}s\n")
            if single and dw:
                f.write(f"\n  Recommendation for 2-core ESP: "
                        f"{'dist-as-worker' if dw[0]['best_wall_s'] < single[0]['best_wall_s'] else 'single-threaded'}\n")
        f.write("-" * 70 + "\n")

    print(f"  Text report:  {out_path}")


def generate_csv_report(records, out_path):
    """Generate a CSV with summary data."""
    with open(out_path, "w") as f:
        f.write("file,workers,mode,best_wall_s\n")
        for r in records:
            f.write(f"{r['file']},{r['workers']},{r['mode']},{r['best_wall_s']:.6f}\n")
    print(f"  CSV summary:  {out_path}")


def generate_phases_csv(records, out_path):
    """Generate a CSV with per-phase breakdown."""
    with open(out_path, "w") as f:
        f.write("file,workers,mode,mmap_s,scan_s,dispatch_s,parse_s,total_s\n")
        for r in records:
            f.write(f"{r['file']},{r['workers']},{r['mode']},"
                    f"{r['mmap_s']:.6f},{r['scan_s']:.6f},"
                    f"{r['dispatch_s']:.6f},{r['parse_s']:.6f},"
                    f"{r['total_s']:.6f}\n")
    print(f"  Phase detail: {out_path}")


def main():
    results_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
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
    generate_text_report(meta, records, os.path.join(out_dir, "bench_report.txt"))
    generate_csv_report(records, os.path.join(out_dir, "bench_report.csv"))
    generate_phases_csv(records, os.path.join(out_dir, "bench_phases.csv"))
    print("Done.")


if __name__ == "__main__":
    main()
