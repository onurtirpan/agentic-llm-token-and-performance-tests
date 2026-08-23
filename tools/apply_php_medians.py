"""Replace the PHP rows at the highest request count with their median run.

PHP is the one implementation whose p50 is not reproducible from a single run,
and the cause is specific: Slim's Composer autoloader touches roughly a hundred
files per request, so the number depends on whether those hit the OS page cache.
Three repeat runs of the Slim variant at the small tier gave 13.73, 6.07 and
6.10 ms. The bare variant, with no autoloader, gave 1.36, 1.36 and 1.38 ms.

So the Slim rows are published as the median of three runs rather than as
whichever single run the sweep happened to catch. This script selects the median
run by p50 and substitutes that whole row, so every field in the published row
comes from one real measurement rather than being averaged across runs.

Usage:  python tools/apply_php_medians.py
Inputs: perf-variance/phpr{1,2,3}.json and perf-variance/pbr{1,2,3}.json
Output: perf-results.json, updated in place
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VARIANCE = ROOT / "perf-variance"
SOURCES = {"php": "phpr", "php-bare": "pbr"}
TIERS = ("small", "mid", "large")


def median_row(language: str, prefix: str, tier: str) -> dict | None:
    rows = []
    for index in (1, 2, 3):
        path = VARIANCE / f"{prefix}{index}.json"
        if not path.exists():
            continue
        for row in json.loads(path.read_text(encoding="utf-8")):
            if row["tier"] == tier and row["language"] == language and "warm" in row:
                rows.append(row)
    if not rows:
        return None
    rows.sort(key=lambda row: row["warm"]["p50Ms"])
    chosen = rows[len(rows) // 2]
    chosen["medianOf"] = len(rows)
    chosen["allP50Ms"] = sorted(round(row["warm"]["p50Ms"], 3) for row in rows)
    return chosen


target = ROOT / "perf-results.json"
results = json.loads(target.read_text(encoding="utf-8"))
counts = sorted({row["count"] for row in results})
highest = counts[-1]

replaced = 0
for language, prefix in SOURCES.items():
    for tier in TIERS:
        chosen = median_row(language, prefix, tier)
        if chosen is None:
            print(f"no repeat data for {tier}/{language}")
            continue
        chosen["count"] = highest
        results = [row for row in results
                   if not (row["tier"] == tier and row["language"] == language
                           and row["count"] == highest)]
        results.append(chosen)
        replaced += 1
        print(f"{tier}/{language}: median p50 {chosen['warm']['p50Ms']:.3f} ms "
              f"of {chosen['allP50Ms']}")

target.write_text(json.dumps(results, indent=2), encoding="utf-8")
print(f"\nreplaced {replaced} rows in {target}, {len(results)} rows total")
