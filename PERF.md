# Runtime benchmark — 126 measurements

Sequential client, one request at a time. Counts measured: 100, 1000, 10000. Detail tables below use 10000 requests.

## Data quality

- NON-2XX large/c at 10000: errors=0 statuses={'200': 8781, '201': 477, '409': 748}

## Cold start

Milliseconds from process spawn to the first successful `GET /health`. The probe column is the harness's own cost to detect a server already listening, measured on every run; startup is the difference.

| Language | small | mid | large | median probe | median startup |
|---|---:|---:|---:|---:|---:|
| C | 66 ms | 69 ms | 76 ms | 12.7 ms | **56 ms** |
| Zig | 63 ms | 69 ms | 72 ms | 1.7 ms | **61 ms** |
| Rust | 80 ms | 74 ms | 70 ms | 2.4 ms | **68 ms** |
| Go | 77 ms | 68 ms | 73 ms | 2.2 ms | **71 ms** |
| C++ | 62 ms | 73 ms | 83 ms | 2.1 ms | **71 ms** |
| PHP | 145 ms | 138 ms | 149 ms | 14.5 ms | **131 ms** |
| TypeScript | 263 ms | 318 ms | 371 ms | 4.1 ms | **315 ms** |
| JavaScript | 265 ms | 337 ms | 345 ms | 14.4 ms | **320 ms** |
| Common Lisp | 416 ms | 591 ms | 697 ms | 13.2 ms | **588 ms** |
| C# | 436 ms | 642 ms | 1308 ms | 16.6 ms | **625 ms** |
| Python | 700 ms | 1598 ms | 1015 ms | 6.4 ms | **1009 ms** |
| Ruby | 1014 ms | 2138 ms | 1309 ms | 17.3 ms | **1294 ms** |
| Java | 9376 ms | 5339 ms | 5265 ms | 5.4 ms | **5334 ms** |
| Kotlin | 7514 ms | 6800 ms | 5987 ms | 6.4 ms | **6793 ms** |

## small tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C++ | **0.195** | 0.261 | 0.358 | 10.9 | 57.2 | 2678 |
| C | **0.196** | 0.267 | 0.356 | 11.2 | 42.7 | 2718 |
| Zig | **0.203** | 0.273 | 0.350 | 10.9 | 48.3 | 2766 |
| Rust | **0.229** | 0.292 | 0.403 | 11.1 | 52.4 | 2411 |
| Go | **0.260** | 0.317 | 0.417 | 10.6 | 15.8 | 2328 |
| TypeScript | **0.349** | 0.457 | 0.557 | 10.7 | 16.7 | 1758 |
| JavaScript | **0.359** | 0.466 | 0.569 | 10.8 | 16.0 | 1720 |
| Common Lisp | **0.391** | 0.509 | 0.785 | 14.8 | 66.1 | 1251 |
| C# | **0.396** | 0.534 | 0.647 | 11.0 | 17.5 | 1515 |
| Java | **0.576** | 1.307 | 1.207 | 16.2 | 85.7 | 817 |
| Kotlin | **0.629** | 1.468 | 1.581 | 21.2 | 95.1 | 626 |
| Python | **1.066** | 1.472 | 1.645 | 12.1 | 72.1 | 602 |
| PHP | **12.459** | 28.914 | 11.803 | 33.3 | 61.7 | 84 |
| Ruby | **15.082** | 26.205 | 15.692 | 66.0 | 105.9 | 64 |

## mid tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| C++ | **0.228** | 0.306 | 0.424 | 11.1 | 67.5 | 2287 |
| C | **0.229** | 0.304 | 0.426 | 11.9 | 50.1 | 2272 |
| Zig | **0.234** | 0.307 | 0.440 | 12.0 | 53.1 | 2208 |
| Go | **0.288** | 0.377 | 0.611 | 11.7 | 86.8 | 1602 |
| Rust | **0.306** | 0.377 | 0.608 | 13.2 | 69.3 | 1605 |
| C# | **0.408** | 0.579 | 0.906 | 13.1 | 98.5 | 1086 |
| TypeScript | **0.450** | 0.626 | 0.866 | 15.5 | 71.3 | 1133 |
| JavaScript | **0.450** | 0.635 | 0.867 | 15.1 | 60.7 | 1133 |
| Common Lisp | **0.474** | 0.637 | 0.899 | 15.3 | 72.9 | 1093 |
| Java | **0.627** | 1.307 | 1.380 | 16.7 | 95.1 | 716 |
| Kotlin | **0.767** | 1.998 | 2.231 | 33.9 | 97.5 | 444 |
| PHP | **1.652** | 23.010 | 7.383 | 55.5 | 113.5 | 135 |
| Python | **2.070** | 12.409 | 4.245 | 47.9 | 99.2 | 234 |
| Ruby | **12.649** | 30.894 | 15.528 | 76.5 | 120.2 | 64 |

## large tier — latency and throughput at 10000 requests

| Language | p50 | p90 | mean | p99 | max | req/s |
|---|---:|---:|---:|---:|---:|---:|
| Zig | **0.235** | 0.310 | 0.404 | 10.9 | 69.7 | 2386 |
| C | **0.241** | 0.321 | 0.408 | 11.1 | 39.1 | 2365 |
| C++ | **0.246** | 0.319 | 0.434 | 11.0 | 62.5 | 2224 |
| Rust | **0.320** | 0.409 | 0.541 | 11.5 | 50.7 | 1790 |
| Go | **0.397** | 0.650 | 1.640 | 43.1 | 96.5 | 602 |
| JavaScript | **0.453** | 0.648 | 0.817 | 12.3 | 58.6 | 1196 |
| C# | **0.470** | 1.106 | 2.093 | 48.1 | 99.6 | 473 |
| TypeScript | **0.491** | 0.712 | 1.015 | 16.8 | 74.7 | 966 |
| Common Lisp | **0.562** | 0.824 | 0.981 | 12.5 | 74.9 | 999 |
| Java | **0.697** | 1.438 | 1.494 | 16.6 | 77.0 | 659 |
| Kotlin | **0.722** | 1.606 | 1.571 | 16.7 | 80.6 | 627 |
| Python | **1.830** | 10.866 | 3.502 | 37.0 | 90.3 | 284 |
| PHP | **8.126** | 32.856 | 15.762 | 67.7 | 144.5 | 63 |
| Ruby | **15.747** | 29.485 | 17.559 | 69.7 | 131.3 | 57 |

## small tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.064 | **3.1 MB** | 3.2 MB |
| C | 0.050 | **4.2 MB** | 4.3 MB |
| C++ | 0.039 | **4.2 MB** | 4.3 MB |
| Rust | 0.091 | **5.8 MB** | 6.0 MB |
| Go | 0.142 | **8.1 MB** | 15.0 MB |
| PHP | 0.829 | **33.7 MB** | 34.4 MB |
| Ruby | 1.387 | **40.5 MB** | 41.9 MB |
| C# | 0.609 | **48.0 MB** | 64.9 MB |
| Python | 1.018 | **48.2 MB** | 48.4 MB |
| JavaScript | 0.216 | **65.2 MB** | 72.3 MB |
| TypeScript | 0.223 | **65.5 MB** | 72.0 MB |
| Common Lisp | 0.212 | **122.6 MB** | 128.6 MB |
| Java | 1.687 | **182.6 MB** | 215.6 MB |
| Kotlin | 1.826 | **257.5 MB** | 273.2 MB |

## mid tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.069 | **3.3 MB** | 3.4 MB |
| C | 0.045 | **4.2 MB** | 4.2 MB |
| C++ | 0.067 | **5.1 MB** | 5.2 MB |
| Rust | 0.147 | **6.0 MB** | 6.3 MB |
| Go | 0.214 | **8.4 MB** | 15.7 MB |
| PHP | 0.920 | **34.3 MB** | 34.8 MB |
| Ruby | 1.635 | **41.8 MB** | 42.5 MB |
| C# | 0.607 | **48.8 MB** | 64.2 MB |
| Python | 2.044 | **49.3 MB** | 50.1 MB |
| TypeScript | 0.320 | **66.3 MB** | 74.0 MB |
| JavaScript | 0.342 | **66.5 MB** | 73.2 MB |
| Common Lisp | 0.272 | **126.5 MB** | 129.1 MB |
| Java | 1.641 | **190.3 MB** | 206.8 MB |
| Kotlin | 2.019 | **201.0 MB** | 225.9 MB |

## large tier — CPU and memory at 10000 requests

| Language | CPU ms/req | idle RSS | peak RSS |
|---|---:|---:|---:|
| Zig | 0.067 | **3.3 MB** | 4.2 MB |
| C | 0.062 | **4.2 MB** | 4.7 MB |
| C++ | 0.066 | **5.2 MB** | 6.0 MB |
| Rust | 0.141 | **6.1 MB** | 7.6 MB |
| Go | 0.270 | **8.3 MB** | 16.6 MB |
| PHP | 5.105 | **34.6 MB** | 36.7 MB |
| Ruby | 1.864 | **42.7 MB** | 45.4 MB |
| Python | 1.884 | **50.1 MB** | 52.1 MB |
| C# | 0.617 | **50.7 MB** | 65.7 MB |
| TypeScript | 0.366 | **66.7 MB** | 80.2 MB |
| JavaScript | 0.338 | **66.8 MB** | 80.5 MB |
| Common Lisp | 0.366 | **129.4 MB** | 131.6 MB |
| Java | 1.841 | **188.1 MB** | 218.1 MB |
| Kotlin | 2.041 | **195.9 MB** | 242.0 MB |

## Does latency hold as the load rises?

p50 milliseconds at each request count, large tier. A rising value means the implementation degrades under sustained load; a flat one means it does not.

| Language | 100 | 1000 | 10000 | drift |
|---|---:|---:|---:|---:|
| Kotlin | 2.479 | 1.645 | 0.722 | 0.29x |
| Java | 1.735 | 1.384 | 0.697 | 0.40x |
| C# | 0.677 | 0.901 | 0.470 | 0.69x |
| JavaScript | 0.525 | 0.472 | 0.453 | 0.86x |
| Zig | 0.258 | 0.231 | 0.235 | 0.91x |
| Ruby | 17.068 | 15.620 | 15.747 | 0.92x |
| TypeScript | 0.523 | 0.461 | 0.491 | 0.94x |
| Python | 1.887 | 1.707 | 1.830 | 0.97x |
| Rust | 0.313 | 0.292 | 0.320 | 1.02x |
| C++ | 0.229 | 0.231 | 0.246 | 1.07x |
| C | 0.224 | 0.218 | 0.241 | 1.07x |
| Common Lisp | 0.518 | 0.562 | 0.562 | 1.09x |
| Go | 0.340 | 0.343 | 0.397 | 1.17x |
| PHP | 2.736 | 3.156 | 8.126 | **2.97x** |

