Raw repeat-run data for the variance study in METHODOLOGY.md section 9.

## Files

| Pattern | What it is |
| --- | --- |
| `govar{1,2,3}.json` | three consecutive Go runs, 1000 requests, all three tiers |
| `zigvar{1,2,3}.json` | the same for Zig |
| `phpvar{1,2,3}.json` | the same for PHP, taken while PHP still used Slim 4 |
| `php-run{1,2,3}.json` | three full PHP runs at 100 / 1000 / 10000, no framework |
| `removed-slim-run{1,2,3}.json` | the same three runs for the Slim 4 build, before it was dropped |

Produced by:

    python tools/perf.py --lang <language> --counts 1000 --out <file>

## The finding

`p50` is the stable figure and `req/s` is not. Three repeats of the same Go code
gave 580, 1095 and 1145 req/s while p50 moved about 13 percent, because
throughput derives from the mean and one 15.6 ms Windows timer stall costs a
sub-millisecond server the wall clock of roughly fifty normal requests. **Rank on
p50. The throughput order among C, C++, Zig, Go and Rust is not established by
this data.**

The slow implementations are the reproducible ones, for the same reason inverted:
a millisecond of real work per request dwarfs a scheduling artifact. PHP repeated
within 1.1 percent on p50 at the small tier, 0.6 percent at the mid tier and
4.3 percent at the large tier.

`removed-slim-run*.json` is kept as the evidence for one specific claim: that a
framework, not the language, was responsible both for PHP's earlier latency and
for its earlier run-to-run instability. The Slim build swung 13.73 / 6.07 /
6.10 ms at the small tier, because its Composer autoloader touches roughly a
hundred files per request and page-cache state decides the result. PHP is now
measured with no framework, so those files describe an implementation that is no
longer part of the benchmark.
