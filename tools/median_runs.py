"""Combine repeated perf passes into one dataset by taking the median run.

    python tools/median_runs.py perf-variance/full-run1.json ... --out perf-results.json

This host carries a large and variable background load from unrelated processes,
so a single pass is not reproducible: PHP's small-tier p50 moved between 1.4 ms
and 12.5 ms across passes of identical code. Publishing one pass would report the
machine's mood.

For each (tier, language, count) the whole row is chosen by the median warm p50,
rather than taking a median of each field separately. Mixing fields from
different passes would produce a row that never happened — a p50 from one run
beside a CPU figure from another. Choosing a whole row keeps every figure
internally consistent, and records which pass it came from.
"""

import argparse
import json
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def key(row):
    return row["tier"], row["language"], row["count"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs="+", help="perf JSON files, one per pass")
    parser.add_argument("--out", default="perf-results.json")
    args = parser.parse_args()

    passes = []
    for name in args.runs:
        path = ROOT / name
        passes.append(json.loads(path.read_text(encoding="utf-8")))
        print(f"read {name}: {len(passes[-1])} rows")

    grouped: dict[tuple, list] = {}
    for index, rows in enumerate(passes):
        for row in rows:
            grouped.setdefault(key(row), []).append((index, row))

    chosen, partial = [], 0
    for group_key in sorted(grouped):
        candidates = grouped[group_key]
        if len(candidates) < len(passes):
            partial += 1
        p50s = sorted((row["warm"]["p50Ms"], index, row) for index, row in candidates)
        # The middle row by p50. With an even count this takes the lower middle,
        # which is deliberate: it is a real run rather than an interpolation.
        _, source, row = p50s[(len(p50s) - 1) // 2]
        row = dict(row)
        row["medianOf"] = len(candidates)
        row["fromPass"] = source + 1
        row["allP50Ms"] = [round(value, 4) for value, _, _ in p50s]
        chosen.append(row)

    output = ROOT / args.out
    output.write_text(json.dumps(chosen, indent=2), encoding="utf-8")
    print(f"\nwrote {output}: {len(chosen)} rows"
          + (f", {partial} of them from fewer than {len(passes)} passes" if partial else ""))

    # How far apart the passes were, so the spread is visible rather than implied.
    spreads = []
    for row in chosen:
        values = row["allP50Ms"]
        if len(values) > 1 and values[0] > 0:
            spreads.append((values[-1] / values[0], row["tier"], row["language"], values))
    spreads.sort(reverse=True)
    print("\nwidest p50 spread across passes:")
    for ratio, tier, language, values in spreads[:12]:
        pretty = ", ".join(f"{v:.3f}" for v in values)
        print(f"  {language:12} {tier:6} {ratio:5.2f}x   [{pretty}]")
    if spreads:
        print(f"\nmedian spread across all {len(spreads)} rows: "
              f"{statistics.median(r for r, _, _, _ in spreads):.2f}x")


main()
