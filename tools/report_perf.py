"""Turn perf-results.json into publication tables.

Usage:  python tools/report_perf.py [--count 10000]

This is a separate step from `perf.py` on purpose. The raw measurement carries
three known artifacts, and each needs handling rather than printing:

1. `coldStartMs` includes one successful readiness probe. On this host that probe
   costs between 1 and 52 ms, which is negligible against Java's 5 seconds but
   is most of Zig's 56 ms. The tables therefore report the measured value, the
   probe floor, and the difference.
2. `cpuMsPerRequest` is derived from Windows process CPU counters, whose
   resolution is about 15.6 ms. At 100 requests the delta frequently rounds to
   zero, so CPU is reported only from runs of 1000 requests or more.
3. p99 and max cluster on multiples of the 15.6 ms Windows timer tick, because
   the sequential client loses its scheduling quantum. p50 is the primary
   latency figure; the tail columns are shown but attributed to the host.
"""

import argparse
import json
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TIERS = ("small", "mid", "large")
ORDER = ["python", "typescript", "csharp", "go", "php", "php-bare", "java", "rust",
         "zig", "c", "cpp"]
NAMES = {"python": "Python", "typescript": "TypeScript", "csharp": "C#", "go": "Go",
         "php": "PHP + Slim", "php-bare": "PHP bare", "java": "Java", "rust": "Rust",
         "zig": "Zig", "c": "C", "cpp": "C++"}


def load() -> list[dict]:
    path = ROOT / "perf-results.json"
    if not path.exists():
        raise SystemExit("perf-results.json is missing; run tools/perf.py first")
    return json.loads(path.read_text(encoding="utf-8"))


def pick(rows, tier, count):
    return {row["language"]: row for row in rows
            if row["tier"] == tier and row["count"] == count}


def cold_start_table(rows) -> None:
    print("## Cold start")
    print()
    print("Milliseconds from process spawn to the first successful `GET /health`. "
          "The probe column is the harness's own cost to detect a server already "
          "listening, measured on every run; startup is the difference.")
    print()
    print("| Language | small | mid | large | median probe | median startup |")
    print("|---|---:|---:|---:|---:|---:|")
    table = []
    for language in ORDER:
        cells, floors, nets = [], [], []
        for tier in TIERS:
            found = [row for row in rows
                     if row["tier"] == tier and row["language"] == language]
            if not found:
                cells.append(None)
                continue
            cold = statistics.median(row["coldStartMs"] for row in found)
            floor = statistics.median(row.get("probeFloorMs", 0) for row in found)
            cells.append(cold)
            floors.append(floor)
            nets.append(max(cold - floor, 0.0))
        if not any(cell is not None for cell in cells):
            continue
        table.append((language, cells, statistics.median(floors),
                      statistics.median(nets)))
    table.sort(key=lambda entry: entry[3])
    for language, cells, floor, net in table:
        shown = " | ".join(f"{cell:.0f} ms" if cell is not None else "—" for cell in cells)
        print(f"| {NAMES[language]} | {shown} | {floor:.1f} ms | **{net:.0f} ms** |")
    print()


def latency_table(rows, count) -> None:
    for tier in TIERS:
        found = pick(rows, tier, count)
        usable = {name: row for name, row in found.items() if "warm" in row}
        if not usable:
            continue
        print(f"## {tier} tier — latency and throughput at {count} requests")
        print()
        print("| Language | p50 | p90 | mean | p99 | max | req/s |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for language in sorted(usable, key=lambda name: usable[name]["warm"]["p50Ms"]):
            warm = usable[language]["warm"]
            print(f"| {NAMES[language]} | **{warm['p50Ms']:.3f}** | {warm['p90Ms']:.3f} "
                  f"| {warm['meanMs']:.3f} | {warm['p99Ms']:.1f} | {warm['maxMs']:.1f} "
                  f"| {warm['rps']:.0f} |")
        print()


def resource_table(rows, count) -> None:
    for tier in TIERS:
        found = pick(rows, tier, count)
        usable = {name: row for name, row in found.items() if "warm" in row}
        if not usable:
            continue
        print(f"## {tier} tier — CPU and memory at {count} requests")
        print()
        print("| Language | CPU ms/req | idle RSS | peak RSS |")
        print("|---|---:|---:|---:|")
        for language in sorted(usable, key=lambda name: usable[name].get("idleRssMb", 0)):
            row = usable[language]
            cpu = row["warm"]["cpuMsPerRequest"]
            cpu_text = f"{cpu:.3f}" if count >= 1000 else "n/a"
            print(f"| {NAMES[language]} | {cpu_text} | **{row.get('idleRssMb', 0):.1f} MB** "
                  f"| {row['warm']['peakRssMb']:.1f} MB |")
        print()


def scaling_table(rows, counts) -> None:
    print("## Does latency hold as the load rises?")
    print()
    print("p50 milliseconds at each request count, large tier. A rising value means "
          "the implementation degrades under sustained load; a flat one means it does not.")
    print()
    header = " | ".join(str(count) for count in counts)
    print(f"| Language | {header} | drift |")
    print("|---|" + "---:|" * (len(counts) + 1))
    table = []
    for language in ORDER:
        cells = []
        for count in counts:
            row = pick(rows, "large", count).get(language)
            cells.append(row["warm"]["p50Ms"] if row and "warm" in row else None)
        if cells[0] is None or cells[-1] is None:
            continue
        table.append((language, cells, cells[-1] / cells[0]))
    table.sort(key=lambda entry: entry[2])
    for language, cells, drift in table:
        shown = " | ".join(f"{cell:.3f}" if cell is not None else "—" for cell in cells)
        mark = "**" if drift > 1.5 else ""
        print(f"| {NAMES[language]} | {shown} | {mark}{drift:.2f}x{mark} |")
    print()


def problems(rows) -> None:
    failed = [row for row in rows if "failed" in row]
    skipped = [row for row in rows if "skipped" in row]
    dirty = [row for row in rows if "warm" in row
             and (row["warm"]["errors"]
                  or any(int(code) >= 400 for code in row["warm"]["statuses"]))]
    if not (failed or skipped or dirty):
        print("## Data quality")
        print()
        print("Every run completed with no failed request and no non-2xx response.")
        print()
        return
    print("## Data quality")
    print()
    for row in failed:
        print(f"- FAILED {row['tier']}/{row['language']} at {row['count']}: {row['failed']}")
    for row in skipped:
        print(f"- SKIPPED {row['tier']}/{row['language']} at {row['count']}: {row['skipped']}")
    for row in dirty:
        print(f"- NON-2XX {row['tier']}/{row['language']} at {row['count']}: "
              f"errors={row['warm']['errors']} statuses={row['warm']['statuses']}")
    print()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=10000)
    args = parser.parse_args()

    rows = load()
    counts = sorted({row["count"] for row in rows})
    target = args.count if args.count in counts else counts[-1]

    print(f"# Runtime benchmark — {len(rows)} measurements")
    print()
    print(f"Sequential client, one request at a time. Counts measured: "
          f"{', '.join(str(count) for count in counts)}. "
          f"Detail tables below use {target} requests.")
    print()
    problems(rows)
    cold_start_table(rows)
    latency_table(rows, target)
    resource_table(rows, target)
    if len(counts) > 1:
        scaling_table(rows, counts)


if __name__ == "__main__":
    main()
