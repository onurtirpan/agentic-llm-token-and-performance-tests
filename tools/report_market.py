"""Join third-party popularity figures to this repo's own measurements.

Produces one table that answers a practical question: for the languages a team
actually chooses between, what does each one cost in tokens and at runtime?

    python tools/report_market.py            # markdown
    python tools/report_market.py --html     # table rows for the results pages

Popularity comes from popularity.json and is NOT measured here. Everything else
comes from results.json and perf-results.json, which are. The two are kept in
separate columns on purpose, because they carry very different confidence.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TIER = "large"
COUNT = 10000

NAMES = {"python": "Python", "typescript": "TypeScript", "csharp": "C#", "go": "Go",
         "php": "PHP", "java": "Java", "rust": "Rust", "zig": "Zig", "c": "C",
         "cpp": "C++", "kotlin": "Kotlin", "ruby": "Ruby", "javascript": "JavaScript",
         "lisp": "Common Lisp"}

WEIGHT = {"very high": "çok yüksek", "high": "yüksek", "medium": "orta", "low": "düşük"}


def load():
    pop = json.loads((ROOT / "popularity.json").read_text(encoding="utf-8"))
    sizes = json.loads((ROOT / "results.json").read_text(encoding="utf-8"))
    perf = json.loads((ROOT / "perf-results.json").read_text(encoding="utf-8"))
    return pop, sizes, perf


def build():
    pop, sizes, perf = load()
    entries = pop["languages"]
    total = sum(e["so2025"] for e in entries.values())

    tokens = {r["language"]: r["source"]["tokens"]
              for r in sizes if r["tier"] == TIER}
    runtime = {r["language"]: r for r in perf
               if r["tier"] == TIER and r["count"] == COUNT}

    # Ranks are computed only over languages that have been measured, so an
    # unmeasured row shows a blank rank rather than a misleading one.
    by_tokens = sorted(tokens, key=lambda k: tokens[k])
    by_speed = sorted(runtime, key=lambda k: runtime[k]["warm"]["p50Ms"])
    token_rank = {k: i for i, k in enumerate(by_tokens, start=1)}
    speed_rank = {k: i for i, k in enumerate(by_speed, start=1)}

    rows = []
    for key, entry in sorted(entries.items(), key=lambda kv: -kv[1]["so2025"]):
        centre = entry["so2025"] / total * 100
        low, high = centre * 0.85, centre * 1.15
        run = runtime.get(key)
        rows.append({
            "key": key,
            "name": NAMES.get(key, key),
            "so2025": entry["so2025"],
            "share": f"{low:.1f}–{high:.1f}%" if centre >= 1 else f"{low:.2f}–{high:.2f}%",
            "backend": WEIGHT[entry["backendWeight"]],
            "note": entry["note"],
            "tokens": tokens.get(key),
            "tokenRank": token_rank.get(key),
            "p50": run["warm"]["p50Ms"] if run else None,
            "speedRank": speed_rank.get(key),
            "cpu": run["warm"]["cpuMsPerRequest"] if run else None,
            "idleRss": run["idleRssMb"] if run else None,
            "peakRss": run["warm"]["peakRssMb"] if run else None,
        })
    return rows, len(tokens)


def cell(value, fmt="{}", blank="—"):
    return blank if value is None else fmt.format(value)


def markdown(rows, measured):
    print("## Sektör payı ve ölçülen maliyet\n")
    print(f"Pay tahmini üçüncü taraf anketlerinden türetildi ve bu depoda ölçülmedi. "
          f"Diğer bütün sütunlar burada ölçüldü: büyük ölçek, {COUNT:,} istek. "
          f"{measured} dil ölçüldü.\n")
    print("| Dil | Sektör payı | Backend ağırlığı | Token | Token sırası "
          "| p50 ms | Hız sırası | CPU ms/istek | Boşta RAM | Tepe RAM |")
    print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print(f"| {r['name']} | {r['share']} | {r['backend']} "
              f"| {cell(r['tokens'], '{:,}')} | {cell(r['tokenRank'])} "
              f"| {cell(r['p50'], '{:.3f}')} | {cell(r['speedRank'])} "
              f"| {cell(r['cpu'], '{:.3f}')} | {cell(r['idleRss'], '{:.1f} MB')} "
              f"| {cell(r['peakRss'], '{:.1f} MB')} |")
    print("\n### Satır notları\n")
    for r in rows:
        print(f"- **{r['name']}** — {r['note']}")


def html(rows):
    for r in rows:
        print(f'          <tr><td class="name">{r["name"]}</td>'
              f'<td class="num" data-sort="{r["so2025"]}">{r["share"]}</td>'
              f'<td>{r["backend"]}</td>'
              f'<td class="num" data-sort="{r["tokens"] or 0}">{cell(r["tokens"], "{:,}")}</td>'
              f'<td class="num">{cell(r["tokenRank"])}</td>'
              f'<td class="num" data-sort="{r["p50"] or 0}">{cell(r["p50"], "{:.3f}")}</td>'
              f'<td class="num">{cell(r["speedRank"])}</td>'
              f'<td class="num" data-sort="{r["cpu"] or 0}">{cell(r["cpu"], "{:.3f}")}</td>'
              f'<td class="num" data-sort="{r["idleRss"] or 0}">{cell(r["idleRss"], "{:.1f} MB")}</td></tr>')


rows, measured = build()
if "--html" in sys.argv:
    html(rows)
else:
    markdown(rows, measured)
