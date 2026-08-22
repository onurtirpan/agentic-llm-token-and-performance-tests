# Runtime benchmark — 99 measurements

Sequential client, one request at a time. Counts measured: 100, 1000, 10000. Detail tables below use 10000 requests.

## Data quality

- NON-2XX large/c at 10000: errors=0 statuses={'200': 8781, '201': 477, '409': 748}

## Cold start

Milliseconds from process spawn to the first successful `GET /health`. The probe column is the harness's own cost to detect a server already listening, measured on every run; startup is the difference.

| Language | small | mid | large | median probe | median startup |
|---|---:|---:|---:|---:|---:|
| C | 63 ms | 81 ms | 59 ms | 18.5 ms | **47 ms** |
| Go | 90 ms | 68 ms | 73 ms | 13.6 ms | **55 ms** |
| Zig | 60 ms | 70 ms | 59 ms | 1.8 ms | **57 ms** |
| C++ | 58 ms | 72 ms | 75 ms | 13.4 ms | **58 ms** |
| Rust | 71 ms | 71 ms | 72 ms | 2.4 ms | **69 ms** |
| PHP bare | 223 ms | 189 ms | 148 ms | 14.6 ms | **186 ms** |
| PHP + Slim | 245 ms | 310 ms | 184 ms | 32.1 ms | **209 ms** |
| TypeScript | 463 ms | 452 ms | 465 ms | 3.3 ms | **452 ms** |
| C# | 962 ms | 722 ms | 455 ms | 10.5 ms | **712 ms** |
| Python | 868 ms | 1230 ms | 1027 ms | 14.8 ms | **1012 ms** |
| Java | 6218 ms | 5294 ms | 4976 ms | 19.4 ms | **5275 ms** |

## small tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C | **0.166** | 0.234 | 0.279 | 0.7 | 27.7 | 3454 |
| C++ | **0.200** | 0.265 | 0.378 | 9.8 | 81.8 | 2561 |
| Zig | **0.211** | 0.276 | 0.378 | 3.1 | 50.9 | 2562 |
| Rust | **0.251** | 0.320 | 0.522 | 12.4 | 67.0 | 1868 |
| Go | **0.292** | 0.449 | 1.216 | 33.4 | 119.0 | 812 |
| C# | **0.404** | 0.675 | 1.457 | 34.3 | 82.0 | 679 |
| TypeScript | **0.426** | 0.589 | 0.969 | 17.5 | 75.9 | 1014 |
| Java | **0.583** | 1.199 | 1.439 | 18.7 | 85.2 | 687 |
| Python | **1.256** | 3.190 | 3.321 | 49.6 | 110.7 | 299 |
| PHP bare | **1.355** | 20.541 | 5.984 | 53.4 | 109.3 | 166 |
| PHP + Slim | **6.854** | 30.415 | 15.045 | 60.8 | 99.6 | 66 |

## mid tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| Zig | **0.192** | 0.257 | 0.315 | 10.8 | 17.4 | 3065 |
| C++ | **0.214** | 0.287 | 0.391 | 11.3 | 57.0 | 2481 |
| C | **0.216** | 0.287 | 0.423 | 11.7 | 59.6 | 2280 |
| Rust | **0.264** | 0.331 | 0.434 | 11.5 | 17.5 | 2240 |
| Go | **0.277** | 0.356 | 0.463 | 9.9 | 24.3 | 2103 |
| C# | **0.380** | 0.513 | 0.631 | 10.5 | 46.5 | 1551 |
| TypeScript | **0.428** | 0.571 | 0.773 | 13.4 | 72.2 | 1269 |
| Java | **0.568** | 1.049 | 1.176 | 16.3 | 66.9 | 838 |
| Python | **1.397** | 2.170 | 2.232 | 13.0 | 65.9 | 445 |
| PHP bare | **1.989** | 28.140 | 10.404 | 34.6 | 91.8 | 96 |
| PHP + Slim | **13.027** | 30.176 | 15.411 | 53.3 | 88.8 | 65 |

## large tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C++ | **0.218** | 0.280 | 0.379 | 11.2 | 57.7 | 2543 |
| C | **0.240** | 0.306 | 0.466 | 11.6 | 57.1 | 2084 |
| Zig | **0.254** | 0.317 | 0.487 | 12.0 | 66.4 | 1986 |
| Go | **0.311** | 0.394 | 0.514 | 11.5 | 17.7 | 1888 |
| Rust | **0.327** | 0.403 | 0.690 | 16.3 | 98.1 | 1412 |
| TypeScript | **0.441** | 0.618 | 0.817 | 14.6 | 55.3 | 1198 |
| C# | **0.444** | 0.645 | 1.157 | 18.7 | 69.9 | 850 |
| Java | **0.595** | 1.141 | 1.296 | 16.8 | 77.4 | 760 |
| Python | **1.621** | 4.360 | 3.743 | 50.4 | 72.7 | 266 |
| PHP bare | **25.181** | 44.498 | 26.169 | 82.2 | 134.6 | 38 |
| PHP + Slim | **31.571** | 61.006 | 33.266 | 89.9 | 141.5 | 30 |

## small tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.053 | **3.1 MB** | 3.2 MB |
| C | 0.028 | **4.2 MB** | 4.3 MB |
| C++ | 0.045 | **4.2 MB** | 4.3 MB |
| Rust | 0.097 | **5.8 MB** | 6.0 MB |
| Go | 0.169 | **8.3 MB** | 15.2 MB |
| PHP bare | 0.834 | **33.6 MB** | 34.2 MB |
| PHP + Slim | 4.391 | **35.5 MB** | 36.0 MB |
| C# | 0.701 | **47.9 MB** | 64.8 MB |
| Python | 1.254 | **48.3 MB** | 48.5 MB |
| TypeScript | 0.297 | **65.2 MB** | 71.5 MB |
| Java | 1.692 | **181.1 MB** | 210.3 MB |

## mid tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.053 | **3.3 MB** | 3.4 MB |
| C | 0.053 | **4.2 MB** | 4.3 MB |
| C++ | 0.075 | **5.1 MB** | 5.2 MB |
| Rust | 0.105 | **6.0 MB** | 6.3 MB |
| Go | 0.186 | **8.4 MB** | 15.5 MB |
| PHP bare | 0.860 | **34.3 MB** | 34.8 MB |
| PHP + Slim | 4.395 | **35.8 MB** | 36.2 MB |
| C# | 0.490 | **48.5 MB** | 63.9 MB |
| Python | 1.455 | **49.2 MB** | 50.1 MB |
| TypeScript | 0.297 | **66.1 MB** | 74.3 MB |
| Java | 1.478 | **185.5 MB** | 204.6 MB |

## large tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.069 | **3.3 MB** | 4.2 MB |
| C | 0.059 | **4.2 MB** | 4.7 MB |
| C++ | 0.070 | **5.2 MB** | 6.0 MB |
| Rust | 0.150 | **6.2 MB** | 7.5 MB |
| Go | 0.230 | **8.6 MB** | 16.2 MB |
| PHP bare | 14.059 | **34.5 MB** | 44.0 MB |
| PHP + Slim | 18.477 | **36.1 MB** | 45.7 MB |
| Python | 1.764 | **50.0 MB** | 52.4 MB |
| C# | 0.653 | **50.9 MB** | 66.6 MB |
| TypeScript | 0.356 | **67.0 MB** | 80.4 MB |
| Java | 1.613 | **191.0 MB** | 204.1 MB |

## Does latency hold as the load rises?

p50 milliseconds at each request count, large tier. A rising value means the implementation degrades under sustained load; a flat one means it does not.

| Language | 100 | 1000 | 10000 | drift |
|---|---:|---:|---:|---:|
| Java | 1.304 | 1.182 | 0.595 | 0.46x |
| C# | 0.499 | 0.548 | 0.444 | 0.89x |
| C++ | 0.229 | 0.246 | 0.218 | 0.95x |
| TypeScript | 0.458 | 0.464 | 0.441 | 0.96x |
| C | 0.247 | 0.231 | 0.240 | 0.97x |
| Rust | 0.332 | 0.336 | 0.327 | 0.99x |
| Python | 1.598 | 1.464 | 1.621 | 1.01x |
| Go | 0.294 | 0.293 | 0.311 | 1.06x |
| Zig | 0.238 | 0.269 | 0.254 | 1.06x |
| PHP + Slim | 25.639 | 15.867 | 31.571 | 1.23x |
| PHP bare | 14.072 | 14.437 | 25.181 | **1.79x** |

