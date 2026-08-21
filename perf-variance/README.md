Raw repeat-run data for the variance study in METHODOLOGY.md section 9.

Three consecutive runs each of PHP, Go and Zig, at 1000 requests, all three
tiers, produced by:

    python tools/perf.py --lang <language> --counts 1000 --out <file>

The finding: p50 is stable for fast implementations and unstable for PHP, while
req/s is the reverse. Rank on p50; treat req/s for a sub-millisecond server as
indicative only.