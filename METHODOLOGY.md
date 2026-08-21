# Methodology

This document is the full record of how the benchmark was built and measured. It
exists so a reader can judge the result, reproduce it, or attack it. Read it
before quoting any number.

## 1. The question

**How many tokens does it cost to express the same backend service in each
language, and does that cost ratio change with the size of the service?**

A secondary question, added after the first results: **what does each
implementation cost at runtime** — latency, throughput, CPU and memory?

The first question matters because an LLM coding agent re-reads source on every
task. Source size is a recurring context cost, not a one-off. The second matters
because token efficiency is worthless if the result is too slow to ship.

## 2. What is measured

For every implementation:

| Quantity | Definition |
| --- | --- |
| lines | physical lines in the application source |
| code lines | lines that are neither blank nor a comment |
| characters | Unicode code points |
| bytes | UTF-8 encoded length |
| tokens | tiktoken `o200k_base`, and again `cl100k_base` |
| manifest tokens | the build or dependency file, counted separately |

Application source is discovered by walking the implementation directory for the
language's source extensions, skipping build output and dependency trees
(`node_modules`, `dist`, `bin`, `obj`, `target`, `vendor`, `.zig-cache`,
`zig-out`, `build`, `__pycache__`, `.git`). A Zig `build.zig` counts as a
manifest, not source. C and C++ headers count as source, because a header is
code a reader must load.

## 3. What is deliberately not measured

- **Not** how fast a model writes the code.
- **Not** how many attempts a model needs to get it right. That is the metric
  where static typing repays its extra tokens, and it is not captured here.
- **Not** long-term maintenance cost.
- **Not** the tokenizer any particular model actually uses. See §8.

## 4. The three tiers

Each tier is a written specification, fixed before any implementation existed.

| Tier | Routes | Cases | Scope |
| --- | ---: | ---: | --- |
| small | 7 | 22 | CRUD over one resource, validation, a derived field, a two-key sort, an aggregate with a rounded mean and a nullable field |
| mid | 16 | 98 | session authentication, role checks, row-level access, a middleware chain, structured logging, an error envelope with machine codes and a request id, multi-error validation, pagination, whitelisted sorting, a status state machine, cascade delete, conflict detection |
| large | 34 | 139 | a mandated four-module split, optimistic concurrency with `ETag` and `If-Match`, idempotency keys with replay, soft delete and restore, an audit trail, an outbox, per-session request quotas, bulk writes with per-item results, search, grouped reports, metrics keyed by route pattern, comments, user administration, admin-only field visibility |

The specifications are `SPEC.md`, `SPEC_MID.md` and `SPEC_LARGE.md`. Each builds
on the previous one and states only its additions.

The large tier **mandates** a four-module split — `domain`, `store`, `service`,
`api` — with each module importing only from the ones above it. This is the
structure a long-lived service actually acquires, and it is where statically
typed languages pay declaration cost twice: C and C++ write a header for every
module.

## 5. Invariants held across all ten languages

1. **Same framework at every tier.** A language does not switch frameworks
   between tiers.
2. **Same identifiers.** Only the casing follows the native convention, so
   `next_id`, `nextID` and `NextId` are the same name. Every deviation is
   recorded in §11.
3. **Byte-identical JSON keys**, in `camelCase`, everywhere.
4. **Same port**, 8080, and all state in memory.
5. **No database, no disk, no outbound network call** — with one forced
   exception, PHP, recorded in §11.

## 6. Deliberate exclusions, and why

Three things a real enterprise service would have are excluded. Each is excluded
to keep the workload identical across ten languages rather than to flatter any
of them.

1. **No password hashing.** Credentials are seeded in plaintext and compared
   directly. bcrypt or argon2 is one library call in nine of these languages and
   a hundred-line detour in C. Including it would measure crypto availability,
   not API structure.
2. **No signed tokens.** A session token is an opaque server-side handle, not a
   JWT. HMAC-SHA256 is a standard-library call in nine languages and absent in
   C. Same reasoning.
3. **No wall-clock timestamps in any response body.** Every response must be
   byte-comparable for the conformance test to be exact. The large tier
   therefore orders its audit trail with a monotonic `seq` counter instead of a
   timestamp, and rate-limits with a per-session request quota instead of a
   sliding time window.

Each exclusion **reduces** the measured spread, because each is cheap in a
high-level language and expensive in C. The reported ratios are therefore
conservative with respect to C's disadvantage.

## 7. Verification protocol

Nothing is measured until it works. Two independent gates:

### Gate 1 — conformance

`tools/conformance.py`, `tools/conformance_mid.py` and
`tools/conformance_large.py` drive a **live server over real HTTP** on port
8080. There is no mocking and no static analysis. Cases run in a fixed order and
depend on each other, so the server must start fresh.

Assertions are exact: status code, the complete top-level key set of the body,
every value, the ordering of list results, the contents and ordering of
validation-error detail arrays, and specific response headers (`X-Request-Id`,
`ETag`, `X-Quota-Remaining`, `Idempotency-Replayed`). Floating-point values
compare with a 1e-9 tolerance; everything else compares exactly.

### Gate 2 — structured logging

From the mid tier on, `tools/check_logs.py` parses the server's captured stdout
and validates **every** log record: that it is one JSON object per line, that its
keys appear in the required order, that `level` matches the status class, that
`path` carries no query string, and that every field has the right type. The
specification also requires that nothing but log records reaches stdout, which
means each implementation had to silence its framework's own logging.

This gate exists because logging is otherwise the easiest requirement to fake.

### Order of work

For each tier: the specification was written first, then the conformance suite,
then a single Python reference implementation was driven to a full pass. Only
then were the other nine written, each given the specification, the passing
reference, and its own previous-tier implementation as a style guide. Every
implementation was then re-verified by the author on port 8080, independently of
the report it came with.

## 8. Token measurement

Counts come from tiktoken `o200k_base`, and independently from `cl100k_base`.

**Neither is Claude's tokenizer.** No offline Claude tokenizer is available here,
and using a network token-count endpoint would have made the measurement
non-reproducible. The consequence is stated plainly: absolute token counts will
differ under another tokenizer.

What does *not* differ is the ordering. The two encoders produce the same
ranking at every tier, and their ratio columns agree to within about 0.06x. That
agreement is the evidence that the ranking is a property of the source rather
than of the encoder. **Use the ratio columns, not the raw counts.**

Manifests are reported separately from source because a manifest is a fixed cost
that does not grow with the logic. Java's `pom.xml` is 305 tokens and Go's
`go.mod` is 10; that difference is real but it never gets larger.

## 9. Performance measurement

`tools/perf.py` measures each implementation at each tier.

| Quantity | Definition |
| --- | --- |
| cold start | milliseconds from process spawn to the first successful `GET /health`, polled every 5 ms |
| cold latency | mean over the first 50 requests, before any warm-up |
| warm latency | mean, p50, p90, p99 and max over N requests, after 300 discarded warm-up requests |
| throughput | requests per second, sequential |
| CPU | process-tree CPU seconds consumed during the measured phase, divided by request count |
| idle memory | process-tree resident set after start-up, before the measured phase |
| peak memory | peak process-tree resident set during the measured phase, sampled every 20 ms |

N is run at 100, 1000 and 10000.

**Each request count runs in a freshly spawned process.** This is not merely
tidiness. The large tier's audit trail and outbox grow as requests run, and
`GET /metrics` reports the pending outbox count, so reusing one process would
make the 10000-request phase slower than the 100-request phase for reasons that
have nothing to do with the request count. A fresh process per count also yields
three independent cold-start measurements per implementation rather than one.

Five decisions shape these numbers, and each is a limitation to state rather
than hide:

1. **The client is sequential.** One request at a time. Four of the ten
   implementations are single-threaded by design — C, C++, Zig and the PHP
   built-in server — so a concurrent benchmark would report the threading model
   rather than the language. Sequential latency is the comparable figure. A
   concurrent throughput comparison is a separate question and is labelled as
   such wherever it appears.
2. **Connection reuse is left to the server.** The client keeps the connection
   alive when the server allows it and reconnects when the server sends
   `Connection: close`. C, C++ and Zig close every connection and therefore pay
   a TCP handshake per request. That is a real property of those
   implementations.
3. **The request mix is state-neutral per cycle.** Each cycle creates a row and
   deletes it again, so a 10000-request run does not grow the data set without
   bound. The large tier's audit trail and outbox do still grow, which is
   realistic; where that changes a result it is reported.
4. **The client is Python.** Client overhead is included in every latency
   figure and is roughly constant across implementations, so it compresses the
   ratios slightly. It does not reorder them.

The harness also watches `X-Quota-Remaining` and logs in again before the large
tier's per-session quota runs out, so quota exhaustion never distorts a run.

### Read p50, not p99

The latency distributions on this host have a clean body and a contaminated
tail. A representative small-tier run at 1000 requests:

| Language | min | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Rust | 0.146 | 0.247 | 0.309 | 12.57 | 68.36 |
| Go | 0.182 | 0.290 | 0.426 | 25.50 | 76.97 |
| TypeScript | 0.240 | 0.455 | 0.643 | 14.04 | 44.72 |
| C# | 0.363 | 0.532 | 0.846 | 47.05 | 62.52 |

The p99 figures cluster on multiples of roughly 15.6 ms, which is the default
Windows timer tick. A 40-fold jump between p90 and p99 is not a property of any
of these servers; it is the client thread losing its scheduling quantum. The p50
and p90 columns are stable and reproducible, and they order the implementations
consistently.

**Therefore p50 is reported as the primary latency figure**, with the mean given
alongside for completeness and flagged as tail-dominated. The p99 and max columns
are published but should be read as a property of the measurement host, not of
the language. A dedicated tail-latency study would need a real load generator, a
tuned timer resolution, and process pinning; none of that is done here.

The harness records `probeFloorMs` on every run — the time it takes to detect a
server that is already listening — so a reader can see the floor under the
cold-start figures rather than having to infer it.

## 9a. Fairness corrections

Two implementations were being handicapped by our own choices rather than by
their language. Both were found by asking why a result looked worse than it
should, and both were corrected before publication. The pre-correction dataset is
kept as `perf-results-before-opcache.json` so the effect is auditable.

### PHP had OPcache switched off

`php -S` runs under the CLI SAPI, and `opcache.enable_cli` defaults to `Off`.
Every request was therefore recompiling all of Slim from source. No production
PHP deployment runs that way. Enabling it roughly halved PHP's median latency at
the mid tier, from 10.43 ms to 6.29 ms.

Having fixed that, a layered probe was run to find where PHP's remaining time
actually goes. Each row adds one layer, `GET` only, warm, with a fresh store:

| Layer | p50 | added by this layer |
| --- | ---: | ---: |
| bare PHP, no framework | 0.874 ms | — |
| plus Slim construction, no store, no logic | 5.113 ms | **+4.24 ms** |
| plus our small-tier store and routes | 5.384 ms | +0.27 ms |
| plus our large-tier store and routes | 6.410 ms | +1.30 ms |

**PHP the language is fast here — 0.874 ms, better than C# or Python.** Roughly
94 percent of the gap is Slim's container, router and middleware stack being
rebuilt from scratch on every request, which is what PHP's shared-nothing model
requires. Every other language in this benchmark builds its router once at
start-up and reuses it for the life of the process.

Our file-backed store is *not* the main cost at normal size. It becomes the main
cost only at the large tier under sustained load, where the audit trail and
outbox grow and the whole store is re-serialized per request: PHP's p50 goes
9.20 ms at 100 requests, 12.29 ms at 1000, 32.26 ms at 10000. That growth is a
consequence of the specification forbidding a database combined with PHP being
unable to hold memory between requests, and APCu was not available on this host.
A production PHP service would keep an audit trail in a database and would not
pay it.

The honest summary: PHP's numbers here measure the shared-nothing execution
model, not the language. A persistent runtime — Swoole, RoadRunner, FrankenPHP —
removes the per-request framework rebuild entirely, and is not tested here.

### C and C++ were not using keep-alive

Both sent `Connection: close` on every response, forcing a fresh TCP connection
per request. HTTP/1.1 defaults to persistent connections, so this was a
deviation from the protocol default, and it cost them roughly a tenfold
throughput penalty — 181 and 213 requests per second against Zig's 2,684.

Zig is also hand-written over raw sockets with no framework, and it implements
keep-alive correctly, which is what identified this as our defect rather than a
property of C or C++. All six C and C++ implementations were changed to honour
the client's `Connection` header, reuse the socket, and carry a receive timeout
so a single idle client cannot block a single-threaded accept loop.

### What was checked and left alone

Everything else was reviewed for a comparable handicap and found fair: all ten
run a single process with default framework settings, the compiled languages all
build in their standard release mode (`-O2`, `--release`, `ReleaseSafe`), and no
language is given tuning flags another is denied.

## 10. Threats to validity

Stated bluntly, in rough order of importance.

1. **Framework choice measures the ecosystem, not just the language.** Every
   implementation uses the framework a real team would pick. FastAPI and ASP.NET
   Minimal API hide an HTTP parser, a JSON parser, a JSON writer and a socket
   loop that C, C++ and Zig must write by hand. That gap is real and a team
   feels it, but it is not a measurement of language syntax. A
   standard-library-only variant would compress the band substantially and is
   not run here.
2. **One workload shape.** A JSON-over-HTTP CRUD service with in-memory state.
   Nothing here speaks to numeric computing, streaming, concurrency-heavy work,
   or anything with a real database.
3. **Author effect.** The same author wrote or directed all thirty
   implementations. Style is therefore more consistent than it would be across
   thirty independent teams, which likely *reduces* variance relative to the
   real world. Conversely, per-language idiom expertise is uneven, and a
   specialist might write any given implementation more tersely.
4. **Three tiers is three data points.** The convergence effect between tiers is
   large and consistent across eight of ten languages, but three points do not
   establish the shape of a curve.
5. **Tokenizer proxy.** See §8.
6. **`code lines` uses a heuristic.** A line is treated as a comment when its
   first non-space characters are a comment marker. It does not parse the
   language, so a trailing comment on a code line is not subtracted, and a
   commented-out block inside a string literal would be miscounted. The `lines`,
   `characters` and `tokens` figures are exact; only `code lines` is
   approximate.
7. **Single machine, single run.** Performance figures come from one Windows
   Server host with no repetition-and-averaging across reboots. They are
   indicative of relative cost, not absolute capacity.

## 11. Divergence register

Every known place where the ten implementations are not identical. No
conformance case distinguishes any of these; each is recorded because it affects
the token count slightly or because a language forced the choice.

1. **C# names the entity `TaskItem`.** `Task` collides with
   `System.Threading.Tasks.Task`.
2. **Every language uses `baseScore`, not `base`.** `base` is a C# keyword.
3. **PHP persists its store to `store.json`.** PHP starts a fresh process per
   request and cannot hold state in memory at all. At the mid tier this costs
   about 20 extra lines. At the large tier it is worse: the per-session quota
   counter means nearly every request mutates state and must write the file.
   This is the only implementation that touches disk, and it is a property of
   PHP's execution model, not a shortcut.
4. **C, C++ and Zig reject an unknown JSON key** with `400`. The others ignore
   it. C and C++ would need a recursive value skipper; Zig inherits the
   behaviour from `std.json`'s `ignore_unknown_fields = false` default.
5. **String length units differ.** Python, Rust, PHP, Go and Zig count
   characters or code points. Java and C# count UTF-16 units. C and C++ count
   bytes. Every test string is ASCII, so all agree on every case.
6. **An explicit `"ownerId": null`** is rejected by the strict reading, since
   the specification says the owner must be an existing user. Six
   implementations reported this independently, all stricter than the original
   Python reference; the reference was corrected to match them.
7. **Go and Zig need extra identifiers** the reference does not: a
   `ResponseWriter` wrapper to capture the status for the log line, a sorted-key
   helper because Go maps iterate randomly, and per-row sort comparators.
8. **Java and Go added a `Page` record / struct**, because neither language has
   a tuple to return the four pagination parameters in.
9. **Large tier, header timing.** ASP.NET and Spring flush response headers
   inside the handler, so `X-Quota-Remaining` and `Idempotency-Replayed` are set
   from a pre-commit callback rather than after the handler returns as FastAPI
   allows.
10. **Large tier, small pinned details.** A `429` carries no
    `X-Quota-Remaining` (the quota was not charged). A comment response carries
    no `ETag` (a comment has no version). A replayed idempotent body keeps the
    first request's `requestId`, because it is returned byte for byte.
11. **The label for an unmatched path in `metrics.byRoute` differs**, because
    each router names its catch-all differently: `unmatched` (Python,
    TypeScript, Rust, Zig, C, C++), `GET /` (Go's `ServeMux`),
    `GET {*path:nonfile}` (ASP.NET), `GET /**` (Spring). The conformance test
    asserts only that no segment is a bare number, which all satisfy.
12. **Where the idempotency replay sits, and which routes honour it.** Two
    related gaps between the prose and the reference, both left in place because
    no conformance case reaches either.
    - *Position.* The specification's check order was corrected mid-build to
      match the reference, which runs the role check, the existence check and
      the body parse *before* the replay. Rust and C implement the earlier
      published order and replay right after the quota charge.
    - *Scope.* `SPEC_LARGE.md` says every `POST` accepts an `Idempotency-Key`.
      The reference wires it on four routes only — `POST /users`,
      `POST /projects`, `POST /projects/{id}/tasks` and
      `POST /tasks/{id}/comments` — so the restore routes, `/tasks/bulk`,
      `/auth/login`, `/auth/logout` and `/outbox/flush` ignore the header.
      TypeScript, C#, Go, PHP, Java, Zig and C++ follow the reference; C follows
      the prose and honours it on every `POST`. This is the one substantive
      unpinned behaviour in the benchmark, and it is a defect in the test rather
      than in any implementation.
13. **Java entities are immutable records**, so a write builds a new instance and
    re-puts it into the store, and the service layer returns the new row. The
    reference mutates a dataclass in place. Rust's `Session` likewise omits the
    `token` field, because the token is already the map key and duplicating it
    would be dead code.
14. **Fixed capacities in C.** C uses fixed-size arrays rather than dynamic
    allocation, so a table overflow returns `409 conflict` and a JSON string
    longer than 255 bytes is rejected as `400 bad_request` before it can reach
    the `is too long` `422` rule. Every test string is well under that bound.
    This is a real ceiling the other nine implementations do not have.
15. **Duplicate keys in one JSON object.** Python keeps the last occurrence; the
    hand-written C and C++ scanners return the first. Unspecified and untested.
16. **Header timing forced three different mechanisms.** FastAPI can set response
    headers after the handler returns. ASP.NET needs a `Response.OnStarting`
    callback, and Spring needs a `ThreadLocal<HttpServletResponse>` written
    during the handler, because both commit the response while writing the body.
    Spring additionally rewrites a bare `ETag` value into a quoted one unless it
    is set on the raw servlet response.

## 12. Environment

| Component | Version |
| --- | --- |
| OS | Windows Server 2025 Datacenter, 10.0.26100 |
| Python | CPython 3.13 |
| Node | via nvm, tsc 5 |
| .NET SDK | 10.0.302 |
| Go | 1.26.4 |
| PHP | 8.5.9 (with `openssl` and `mbstring` enabled) |
| Java | Temurin JDK 21.0.12, Maven 3.9.16 |
| Rust | rustc stable, `x86_64-pc-windows-gnu` host |
| Zig | 0.16.0 |
| C / C++ | gcc / g++ 16.2.0 (mingw-w64), `-O2 -Wall -Wextra -static` |
| tiktoken | `o200k_base` and `cl100k_base` |
| psutil | 7.2.2 |

Rust runs on the GNU host toolchain because the machine has no MSVC linker.
C and C++ link statically so the binaries run without mingw on `PATH`.

## 13. Reproduction

```powershell
pwsh -File tools\run_all.ps1          # small: build, serve, test, measure
pwsh -File tools\run_all_mid.ps1      # mid:   the same, plus the log check
pwsh -File tools\run_all_large.ps1    # large: the same
python tools\perf.py --all            # latency, throughput, CPU, memory
```

Each script builds every implementation, starts each server on port 8080, runs
the full case list, validates the log, stops the server, and prints the
measurement table. `results.json` holds the size measurement and
`perf-results.json` the performance measurement.
