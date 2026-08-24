# Runtime benchmark — 126 measurements

Sequential client, one request at a time. Counts measured: 100, 1000, 10000. Detail tables below use 10000 requests.

## Data quality

- NON-2XX large/c at 10000: errors=0 statuses={'200': 8781, '201': 477, '409': 748}

## Work against waiting

`p50` minus CPU per request. A language that computes for its whole wall clock sits near zero. A large positive value means the host made it wait, which is not a property of the language and is not comparable across languages. Large tier, 10000 requests.

A negative value is not an error and does not mean the work was free. CPU is counted across the whole process tree, so a runtime that uses other cores — a JVM running its garbage collector and JIT compiler alongside the request — can spend more CPU than the request took in wall-clock time. It marks parallelism, not waiting.

| Language | p50 | CPU ms/req | waiting | reading |
|---|---:|---:|---:|---|
| Kotlin | 0.722 | 2.041 | -1.318 | compute-bound, latency is comparable |
| Java | 0.697 | 1.841 | -1.143 | compute-bound, latency is comparable |
| C# | 0.468 | 0.642 | -0.174 | compute-bound, latency is comparable |
| Python | 1.830 | 1.884 | -0.054 | compute-bound, latency is comparable |
| TypeScript | 0.457 | 0.386 | +0.071 | compute-bound, latency is comparable |
| JavaScript | 0.461 | 0.359 | +0.102 | compute-bound, latency is comparable |
| Go | 0.364 | 0.248 | +0.115 | compute-bound, latency is comparable |
| Zig | 0.237 | 0.072 | +0.165 | compute-bound, latency is comparable |
| Common Lisp | 0.562 | 0.394 | +0.168 | compute-bound, latency is comparable |
| C | 0.232 | 0.064 | +0.168 | compute-bound, latency is comparable |
| C++ | 0.254 | 0.081 | +0.173 | compute-bound, latency is comparable |
| Rust | 0.320 | 0.141 | +0.180 | compute-bound, latency is comparable |
| PHP | 13.329 | 4.948 | +8.381 | **host-limited, do not rank on latency** |
| Ruby | 15.608 | 1.938 | +13.671 | **host-limited, do not rank on latency** |

## Cold start

Milliseconds from process spawn to the first successful `GET /health`. The probe column is the harness's own cost to detect a server already listening, measured on every run; startup is the difference.

| Language | small | mid | large | median probe | median startup |
|---|---:|---:|---:|---:|---:|
| Zig | 71 ms | 79 ms | 65 ms | 16.9 ms | **49 ms** |
| Go | 69 ms | 79 ms | 60 ms | 17.1 ms | **51 ms** |
| C | 78 ms | 70 ms | 67 ms | 12.4 ms | **55 ms** |
| Rust | 73 ms | 77 ms | 70 ms | 17.9 ms | **59 ms** |
| C++ | 62 ms | 73 ms | 89 ms | 11.7 ms | **71 ms** |
| PHP | 134 ms | 132 ms | 137 ms | 17.0 ms | **116 ms** |
| TypeScript | 309 ms | 358 ms | 305 ms | 13.5 ms | **295 ms** |
| JavaScript | 303 ms | 337 ms | 330 ms | 14.3 ms | **305 ms** |
| Common Lisp | 483 ms | 542 ms | 601 ms | 16.8 ms | **525 ms** |
| C# | 449 ms | 560 ms | 542 ms | 5.8 ms | **536 ms** |
| Python | 834 ms | 815 ms | 920 ms | 4.3 ms | **830 ms** |
| Ruby | 1418 ms | 1313 ms | 1392 ms | 13.1 ms | **1376 ms** |
| Java | 5521 ms | 5339 ms | 5265 ms | 5.3 ms | **5334 ms** |
| Kotlin | 5477 ms | 5881 ms | 6002 ms | 18.3 ms | **5849 ms** |

## small tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C | **0.189** | 0.276 | 0.347 | 10.8 | 26.8 | 2771 |
| C++ | **0.195** | 0.261 | 0.358 | 10.9 | 57.2 | 2678 |
| Zig | **0.210** | 0.287 | 0.357 | 10.8 | 45.3 | 2709 |
| Rust | **0.243** | 0.332 | 0.440 | 11.1 | 65.9 | 2206 |
| Go | **0.272** | 0.342 | 0.501 | 11.0 | 75.9 | 1943 |
| Common Lisp | **0.391** | 0.509 | 0.785 | 14.8 | 66.1 | 1251 |
| TypeScript | **0.393** | 0.534 | 0.663 | 11.2 | 58.1 | 1476 |
| JavaScript | **0.393** | 0.540 | 0.673 | 11.3 | 69.1 | 1454 |
| C# | **0.417** | 0.591 | 0.792 | 11.7 | 75.7 | 1238 |
| Java | **0.583** | 1.214 | 1.249 | 15.8 | 75.5 | 790 |
| Kotlin | **0.660** | 1.348 | 1.368 | 15.1 | 81.4 | 722 |
| Python | **1.310** | 2.162 | 2.345 | 17.0 | 81.1 | 423 |
| PHP | **11.260** | 29.507 | 11.152 | 41.6 | 97.6 | 89 |
| Ruby | **15.403** | 25.681 | 15.719 | 59.0 | 95.9 | 64 |

## mid tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C | **0.222** | 0.309 | 0.385 | 10.9 | 27.0 | 2516 |
| C++ | **0.228** | 0.306 | 0.424 | 11.1 | 67.5 | 2287 |
| Zig | **0.231** | 0.334 | 0.416 | 11.1 | 54.2 | 2330 |
| Go | **0.291** | 0.366 | 0.563 | 11.4 | 75.4 | 1733 |
| Rust | **0.300** | 0.428 | 0.537 | 11.7 | 44.7 | 1810 |
| C# | **0.408** | 0.579 | 0.906 | 13.1 | 98.5 | 1086 |
| JavaScript | **0.422** | 0.588 | 0.729 | 11.6 | 72.5 | 1344 |
| TypeScript | **0.437** | 0.629 | 0.754 | 11.8 | 47.9 | 1299 |
| Common Lisp | **0.468** | 0.665 | 0.841 | 12.6 | 77.3 | 1168 |
| Java | **0.627** | 1.307 | 1.380 | 16.7 | 95.1 | 716 |
| Kotlin | **0.742** | 1.680 | 1.585 | 16.3 | 79.0 | 623 |
| Python | **1.640** | 2.726 | 2.812 | 15.9 | 76.1 | 353 |
| PHP | **2.060** | 29.526 | 10.641 | 42.3 | 88.8 | 94 |
| Ruby | **15.548** | 29.227 | 16.880 | 71.7 | 205.7 | 59 |

## large tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C | **0.232** | 0.316 | 0.407 | 10.9 | 64.5 | 2379 |
| Zig | **0.237** | 0.306 | 0.396 | 10.7 | 45.0 | 2433 |
| C++ | **0.254** | 0.329 | 0.454 | 11.3 | 48.8 | 2126 |
| Rust | **0.320** | 0.409 | 0.541 | 11.5 | 50.7 | 1790 |
| Go | **0.364** | 0.520 | 0.743 | 12.8 | 93.6 | 1314 |
| TypeScript | **0.457** | 0.648 | 0.784 | 12.0 | 52.5 | 1245 |
| JavaScript | **0.461** | 0.651 | 0.784 | 11.8 | 49.4 | 1245 |
| C# | **0.468** | 0.670 | 0.903 | 12.6 | 88.7 | 1083 |
| Common Lisp | **0.562** | 0.810 | 0.976 | 12.5 | 73.8 | 1005 |
| Java | **0.697** | 1.438 | 1.494 | 16.6 | 77.0 | 659 |
| Kotlin | **0.722** | 1.606 | 1.571 | 16.7 | 80.6 | 627 |
| Python | **1.830** | 10.866 | 3.502 | 37.0 | 90.3 | 284 |
| PHP | **13.329** | 31.914 | 16.347 | 58.3 | 104.9 | 61 |
| Ruby | **15.608** | 26.399 | 16.579 | 61.5 | 100.8 | 60 |

## small tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.059 | **3.1 MB** | 3.2 MB |
| C | 0.056 | **4.2 MB** | 4.3 MB |
| C++ | 0.039 | **4.2 MB** | 4.3 MB |
| Rust | 0.106 | **5.7 MB** | 6.0 MB |
| Go | 0.156 | **8.0 MB** | 15.0 MB |
| PHP | 0.922 | **33.7 MB** | 34.3 MB |
| Ruby | 1.656 | **42.2 MB** | 43.4 MB |
| C# | 0.603 | **48.1 MB** | 65.1 MB |
| Python | 1.290 | **48.4 MB** | 48.6 MB |
| TypeScript | 0.247 | **65.3 MB** | 78.4 MB |
| JavaScript | 0.275 | **65.5 MB** | 71.7 MB |
| Common Lisp | 0.212 | **122.6 MB** | 128.6 MB |
| Java | 1.667 | **184.6 MB** | 220.8 MB |
| Kotlin | 1.709 | **229.6 MB** | 279.7 MB |

## mid tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.066 | **3.3 MB** | 3.4 MB |
| C | 0.053 | **4.2 MB** | 4.3 MB |
| C++ | 0.067 | **5.1 MB** | 5.2 MB |
| Rust | 0.134 | **6.0 MB** | 6.3 MB |
| Go | 0.245 | **8.3 MB** | 15.4 MB |
| PHP | 1.079 | **34.4 MB** | 34.8 MB |
| Ruby | 1.878 | **42.5 MB** | 43.5 MB |
| C# | 0.607 | **48.8 MB** | 64.2 MB |
| Python | 1.667 | **49.3 MB** | 49.9 MB |
| TypeScript | 0.315 | **65.9 MB** | 73.6 MB |
| JavaScript | 0.290 | **66.8 MB** | 73.7 MB |
| Common Lisp | 0.267 | **126.5 MB** | 129.0 MB |
| Java | 1.641 | **190.3 MB** | 206.8 MB |
| Kotlin | 1.953 | **202.1 MB** | 374.1 MB |

## large tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.072 | **3.3 MB** | 4.2 MB |
| C | 0.064 | **4.2 MB** | 4.7 MB |
| C++ | 0.081 | **5.2 MB** | 6.0 MB |
| Rust | 0.141 | **6.1 MB** | 7.6 MB |
| Go | 0.248 | **8.6 MB** | 16.1 MB |
| PHP | 4.948 | **34.7 MB** | 36.9 MB |
| Ruby | 1.938 | **43.1 MB** | 45.9 MB |
| Python | 1.884 | **50.1 MB** | 52.1 MB |
| C# | 0.642 | **51.0 MB** | 66.7 MB |
| TypeScript | 0.386 | **66.5 MB** | 80.4 MB |
| JavaScript | 0.359 | **66.8 MB** | 80.6 MB |
| Common Lisp | 0.394 | **129.1 MB** | 131.2 MB |
| Java | 1.841 | **188.1 MB** | 218.1 MB |
| Kotlin | 2.041 | **195.9 MB** | 242.0 MB |

## Does latency hold as the load rises?

p50 milliseconds at each request count, large tier. A rising value means the implementation degrades under sustained load; a flat one means it does not.

| Language | 100 | 1000 | 10000 | drift |
|---|---:|---:|---:|---:|
| Kotlin | 2.345 | 1.503 | 0.722 | 0.31x |
| Java | 1.750 | 1.384 | 0.697 | 0.40x |
| C# | 0.629 | 0.563 | 0.468 | 0.74x |
| JavaScript | 0.529 | 0.482 | 0.461 | 0.87x |
| TypeScript | 0.523 | 0.475 | 0.457 | 0.87x |
| Ruby | 16.278 | 15.620 | 15.608 | 0.96x |
| C | 0.239 | 0.222 | 0.232 | 0.97x |
| Zig | 0.241 | 0.235 | 0.237 | 0.98x |
| Python | 1.817 | 1.707 | 1.830 | 1.01x |
| PHP | 12.854 | 3.672 | 13.329 | 1.04x |
| Go | 0.339 | 0.322 | 0.364 | 1.07x |
| Rust | 0.293 | 0.320 | 0.320 | 1.09x |
| C++ | 0.229 | 0.239 | 0.254 | 1.11x |
| Common Lisp | 0.484 | 0.514 | 0.562 | 1.16x |

## Reproducibility across repeated passes

The whole suite was run 3 times and each published row is the median pass by p50, chosen as a whole row so every figure in it comes from one real run. This column is how far the passes moved.

| Language | median spread | worst spread |
|---|---:|---:|
| PHP | 6.33x  **bimodal** | 7.17x |
| Python | 1.24x | 1.36x |
| TypeScript | 1.13x | 1.19x |
| Java | 1.13x | 1.39x |
| Zig | 1.12x | 1.21x |
| Common Lisp | 1.12x | 1.19x |
| Rust | 1.11x | 1.21x |
| Kotlin | 1.11x | 1.36x |
| JavaScript | 1.10x | 1.17x |
| C | 1.10x | 1.27x |
| C++ | 1.07x | 1.20x |
| Go | 1.06x | 1.17x |
| C# | 1.05x | 1.65x |
| Ruby | 1.04x | 1.25x |

