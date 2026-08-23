"""Performance harness for the Task Service benchmark.

Measures, per implementation and per tier:
  cold start        process spawn to the first successful GET /health
  cold latency      the first 50 requests, before any warm-up
  warm latency      p50 / p90 / p99 / mean / max over N requests after a warm-up
  throughput        requests per second, sequential
  CPU              process-tree CPU seconds consumed by the measured phase
  memory           peak process-tree resident set during the measured phase

Usage:
  python tools/perf.py --tier mid --lang go
  python tools/perf.py --tier small --counts 100,1000
  python tools/perf.py --all
  python tools/perf.py --all --counts 100,1000,10000

Design notes that matter when reading the results:

* The client is sequential, one request at a time. That is deliberate. Four of
  the ten implementations are single-threaded by design (C, C++, Zig and the PHP
  built-in server), so any concurrent measurement would report the threading
  model rather than the language. Sequential latency is the comparable number.
* The client keeps the connection alive when the server allows it and reconnects
  when the server sends `Connection: close`. All ten implementations now support
  HTTP/1.1 persistent connections, so none is penalised for a handshake the
  protocol does not require. C and C++ originally closed every connection, which
  cost them about a tenfold throughput penalty; that was our defect, not a
  property of either language, and it has been corrected.
* Each cycle is state-neutral: it creates a row and deletes it again, so a run of
  10000 requests does not grow the data set without bound. The large tier's audit
  trail and outbox do still grow, which is realistic and is reported.
* The large tier charges a per-session quota, so the harness watches
  `X-Quota-Remaining` and logs in again before it runs out.
"""

import argparse
import http.client
import json
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import psutil

ROOT = Path(__file__).resolve().parent.parent
JDK = Path(r"C:\Scoop\apps\temurin21-jdk\current")
HOST, PORT = "127.0.0.1", 8080

LANGUAGES = ["python", "typescript", "csharp", "go", "php", "java", "rust", "zig", "c", "cpp"]
TIER_DIR = {"small": "impl", "mid": "impl-mid", "large": "impl-large"}

# A measured phase projected to run longer than this is skipped and recorded.
TIME_BUDGET_SECONDS = 300.0

# Slow starters need a longer readiness budget, not a fixed sleep.
READY_TIMEOUT = {"java": 90.0, "csharp": 45.0, "python": 45.0}


def launch(tier: str, language: str):
    """Return (argv, cwd) for one implementation, or None when it is absent."""
    base = ROOT / TIER_DIR[tier] / language
    if not base.exists():
        return None
    entry = "api" if tier == "large" else "main"
    table = {
        "python": ([sys.executable, f"{entry}.py"], base),
        "typescript": (["node", str(base / "dist" / f"{entry}.js")], ROOT),
        "csharp": ([str(base / "bin" / "Release" / "net10.0" / "csharp.exe")], ROOT),
        "go": ([str(base / "taskservice.exe")], ROOT),
        "php": (["php", "-S", f"{HOST}:{PORT}", "-t", str(base / "public"),
                 str(base / "public" / "index.php")], ROOT),
        "java": ([str(JDK / "bin" / "java.exe"), "-jar",
                  str(base / "target" / "taskservice-0.1.0.jar")], ROOT),
        "rust": ([str(base / "target" / "release" / "taskservice.exe")], ROOT),
        "zig": ([str(base / "main.exe")], ROOT),
        "c": ([str(base / "taskservice.exe")], ROOT),
        "cpp": ([str(base / "taskservice.exe")], ROOT),
    }
    argv, cwd = table[language]
    # The artefact that must exist before this implementation can be launched.
    if language == "python":
        needed = base / f"{entry}.py"
    elif language == "typescript":
        needed = Path(argv[1])
    elif language.startswith("php"):
        needed = base / "public" / "index.php"
    elif language == "java":
        needed = Path(argv[2])
    else:
        needed = Path(argv[0])
    return (argv, cwd) if needed.exists() else None


class Client:
    """A sequential HTTP client that survives `Connection: close`."""

    def __init__(self):
        self.conn = None
        self.samples: list[float] = []
        self.errors = 0
        self.statuses: dict[int, int] = {}
        self.quota = None

    def close(self):
        if self.conn is not None:
            try:
                self.conn.close()
            except OSError:
                pass
            self.conn = None

    def request(self, method, path, body=None, headers=None, record=True):
        payload = json.dumps(body).encode("utf-8") if body is not None else None
        head = {"Content-Type": "application/json"} if payload is not None else {}
        if headers:
            head.update(headers)
        for attempt in (0, 1):
            if self.conn is None:
                self.conn = http.client.HTTPConnection(HOST, PORT, timeout=30)
            started = time.perf_counter()
            try:
                self.conn.request(method, path, payload, head)
                res = self.conn.getresponse()
                data = res.read()
                elapsed = (time.perf_counter() - started) * 1000.0
                quota = res.getheader("X-Quota-Remaining")
                if quota is not None and quota.lstrip("-").isdigit():
                    self.quota = int(quota)
                if res.will_close:
                    self.close()
                if record:
                    self.samples.append(elapsed)
                    self.statuses[res.status] = self.statuses.get(res.status, 0) + 1
                return res.status, data
            except (http.client.HTTPException, OSError):
                self.close()
                if attempt == 1:
                    if record:
                        self.errors += 1
                    return None, b""
        return None, b""

    def reset(self):
        self.samples = []
        self.errors = 0
        self.statuses = {}


class Sampler(threading.Thread):
    """Sample the resident set of the whole process tree every 20 ms."""

    def __init__(self, pid: int):
        super().__init__(daemon=True)
        self.pid = pid
        self.stop = threading.Event()
        self.peak = 0

    def tree(self):
        try:
            root = psutil.Process(self.pid)
            return [root] + root.children(recursive=True)
        except psutil.Error:
            return []

    def total_rss(self) -> int:
        total = 0
        for proc in self.tree():
            try:
                total += proc.memory_info().rss
            except psutil.Error:
                pass
        return total

    def cpu_seconds(self) -> float:
        total = 0.0
        for proc in self.tree():
            try:
                times = proc.cpu_times()
                total += times.user + times.system
            except psutil.Error:
                pass
        return total

    def run(self):
        while not self.stop.is_set():
            self.peak = max(self.peak, self.total_rss())
            time.sleep(0.02)


# ------------------------------------------------------------------ request mix


def login(client: Client, username: str, password: str):
    status, data = client.request("POST", "/auth/login",
                                  {"username": username, "password": password}, record=False)
    if status != 200:
        return None
    return {"Authorization": "Bearer " + json.loads(data)["token"]}


def setup(client: Client, tier: str) -> dict | None:
    """Log in and seed the one project the write cycle reuses."""
    if tier == "small":
        return {}
    auth = login(client, "admin", "admin-secret")
    if auth is None:
        return None
    status, data = client.request("POST", "/projects",
                                  {"name": "perf", "ownerId": 1}, headers=auth, record=False)
    if status != 201:
        return None
    return {"auth": auth, "project": json.loads(data)["id"]}


def refresh_quota(client: Client, context: dict) -> None:
    if client.quota is not None and client.quota < 120:
        auth = login(client, "admin", "admin-secret")
        if auth is not None:
            context["auth"] = auth
            client.quota = None


def cycle_small(client: Client, context: dict) -> None:
    client.request("GET", "/health")
    status, data = client.request("POST", "/tasks", {"title": "perf", "priority": 3})
    task_id = json.loads(data)["id"] if status == 201 else None
    client.request("GET", "/tasks")
    if task_id is not None:
        client.request("GET", f"/tasks/{task_id}")
        client.request("PUT", f"/tasks/{task_id}",
                       {"title": "perf run", "priority": 4, "done": True})
    client.request("GET", "/stats")
    if task_id is not None:
        client.request("DELETE", f"/tasks/{task_id}")


def cycle_mid(client: Client, context: dict) -> None:
    auth = context["auth"]
    project = context["project"]
    client.request("GET", "/health")
    client.request("GET", "/me", headers=auth)
    client.request("GET", "/projects", headers=auth)
    client.request("GET", f"/projects/{project}/tasks", headers=auth)
    status, data = client.request("POST", f"/projects/{project}/tasks",
                                  {"title": "perf", "priority": 3}, headers=auth)
    task_id = json.loads(data)["id"] if status == 201 else None
    if task_id is not None:
        client.request("GET", f"/tasks/{task_id}", headers=auth)
        client.request("PATCH", f"/tasks/{task_id}/status",
                       {"status": "in_progress"}, headers=auth)
    client.request("GET", "/stats", headers=auth)
    if task_id is not None:
        client.request("DELETE", f"/tasks/{task_id}", headers=auth)


def cycle_large(client: Client, context: dict) -> None:
    refresh_quota(client, context)
    auth = context["auth"]
    project = context["project"]
    client.request("GET", "/health")
    client.request("GET", "/me", headers=auth)
    client.request("GET", "/projects", headers=auth)
    client.request("GET", "/tasks", headers=auth)
    client.request("GET", "/search?q=perf", headers=auth)
    status, data = client.request("POST", f"/projects/{project}/tasks",
                                  {"title": "perf", "priority": 3}, headers=auth)
    if status == 201:
        row = json.loads(data)
        task_id, version = row["id"], row["version"]
        client.request("GET", f"/tasks/{task_id}", headers=auth)
        status, data = client.request("PATCH", f"/tasks/{task_id}/status",
                                      {"status": "in_progress"},
                                      headers={**auth, "If-Match": str(version)})
        if status == 200:
            version = json.loads(data)["version"]
        client.request("DELETE", f"/tasks/{task_id}",
                       headers={**auth, "If-Match": str(version)})
    client.request("GET", "/metrics", headers=auth)


CYCLES = {"small": cycle_small, "mid": cycle_mid, "large": cycle_large}


class Stalled(RuntimeError):
    """Raised when requests keep failing, so a run cannot make progress."""


def run_requests(client: Client, tier: str, context: dict, target: int) -> None:
    """Drive cycles until `target` samples exist, or give up loudly.

    The error ceiling is essential, not defensive. A failing request records no
    sample, so without it a server that stops answering turns this loop into a
    silent infinite hang rather than a reported failure. That happened once, to a
    single-threaded server whose accept loop was blocked by an abandoned probe
    connection, and it cost a whole benchmark run.
    """
    cycle = CYCLES[tier]
    ceiling = max(50, target // 10)
    while len(client.samples) < target:
        before = len(client.samples)
        cycle(client, context)
        if client.errors > ceiling:
            raise Stalled(f"{client.errors} failed requests with "
                          f"{len(client.samples)}/{target} samples")
        if len(client.samples) == before:
            raise Stalled(f"a full cycle produced no sample at "
                          f"{len(client.samples)}/{target}")


def summarize(samples: list[float]) -> dict:
    ordered = sorted(samples)
    def pct(fraction):
        return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]
    return {
        "requests": len(ordered),
        "meanMs": round(statistics.fmean(ordered), 4),
        "p50Ms": round(pct(0.50), 4),
        "p90Ms": round(pct(0.90), 4),
        "p99Ms": round(pct(0.99), 4),
        "maxMs": round(ordered[-1], 4),
        "minMs": round(ordered[0], 4),
    }


# --------------------------------------------------------------------- one run


def measure(tier: str, language: str, count: int) -> dict | None:
    """Measure one implementation at one request count, in a fresh process.

    A fresh process per count matters: the large tier's audit trail and outbox
    grow as requests run, so reusing one process would make the 10000 phase
    slower than the 100 phase for reasons unrelated to the request count.
    """
    spec = launch(tier, language)
    if spec is None:
        print(f"skip {tier}/{language}: not built")
        return None
    argv, cwd = spec
    free_port()
    if language.startswith("php"):
        (ROOT / TIER_DIR[tier] / language / "store.json").unlink(missing_ok=True)

    started = time.perf_counter()
    proc = subprocess.Popen(argv, cwd=str(cwd), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    cold_start = wait_ready(started, READY_TIMEOUT.get(language, 30.0))
    if cold_start is None:
        proc.kill()
        print(f"FAIL {tier}/{language}: never became ready")
        return None

    sampler = Sampler(proc.pid)
    sampler.start()
    result = {"tier": tier, "language": language, "count": count,
              "coldStartMs": round(cold_start, 2), "probeFloorMs": probe_floor()}
    client = Client()
    try:
        context = setup(client, tier)
        if context is None:
            result["failed"] = "setup did not complete"
            print(f"FAIL {tier}/{language}: {result['failed']}")
            return result

        result["idleRssMb"] = round(sampler.total_rss() / (1024 * 1024), 2)

        # Cold phase: the first 50 measured requests, before any warm-up.
        client.reset()
        run_requests(client, tier, context, 50)
        result["cold"] = summarize(client.samples)
        result["cold"]["errors"] = client.errors

        # Warm-up, discarded. Its observed latency also sizes the phase below.
        client.reset()
        run_requests(client, tier, context, 300)
        warmup_mean = statistics.fmean(client.samples)
        client.reset()

        # A slow implementation at a high request count can run for tens of
        # minutes. Skip rather than stall, and say so in the result.
        projected = warmup_mean * count / 1000.0
        if projected > TIME_BUDGET_SECONDS:
            result["skipped"] = (f"projected {projected:.0f}s exceeds the "
                                 f"{TIME_BUDGET_SECONDS:.0f}s budget at "
                                 f"{warmup_mean:.2f} ms per request")
            print(f"  skip phase: {result['skipped']}")
            client.close()
            return result

        # Seed the peak from a direct reading rather than zero. A fast
        # implementation can finish 100 requests in under one 20 ms sampling
        # interval, which would otherwise report a peak of 0 MB.
        sampler.peak = sampler.total_rss()
        cpu_before = sampler.cpu_seconds()
        wall_before = time.perf_counter()
        run_requests(client, tier, context, count)
        wall = time.perf_counter() - wall_before
        cpu = sampler.cpu_seconds() - cpu_before
        phase = summarize(client.samples)
        phase["errors"] = client.errors
        phase["statuses"] = {str(code): n for code, n in sorted(client.statuses.items())}
        phase["wallSeconds"] = round(wall, 4)
        phase["rps"] = round(len(client.samples) / wall, 1) if wall > 0 else 0
        phase["cpuSeconds"] = round(cpu, 4)
        phase["cpuMsPerRequest"] = round(cpu * 1000.0 / max(len(client.samples), 1), 4)
        phase["peakRssMb"] = round(sampler.peak / (1024 * 1024), 2)
        result["warm"] = phase
    except Stalled as stall:
        result["failed"] = str(stall)
        print(f"FAIL {tier}/{language} at {count}: {stall}")
    finally:
        client.close()
        sampler.stop.set()
        sampler.join(timeout=2)
        try:
            parent = psutil.Process(proc.pid)
            for child in parent.children(recursive=True):
                child.kill()
        except psutil.Error:
            pass
        proc.kill()
        proc.wait(timeout=10)
        time.sleep(0.3)
    return result


def wait_ready(started: float, budget: float) -> float | None:
    """Milliseconds until the server answers, or None if it never did.

    A raw socket probe with a short timeout is used rather than an HTTP client.
    On Windows a refused loopback connection through an HTTP client stack can
    take hundreds of milliseconds to report, which would put a floor of roughly
    half a second under every cold-start figure and make the fast binaries
    indistinguishable from each other.
    """
    while time.perf_counter() - started < budget:
        try:
            with socket.create_connection((HOST, PORT), timeout=0.05):
                pass
        except OSError:
            time.sleep(0.001)
            continue
        probe = Client()
        status, _ = probe.request("GET", "/health", record=False)
        probe.close()
        if status == 200:
            return (time.perf_counter() - started) * 1000.0
        time.sleep(0.001)
    return None


def probe_floor() -> float:
    """The harness's own overhead: time to detect a server that is already up."""
    started = time.perf_counter()
    value = wait_ready(started, 5.0)
    return round(value, 3) if value is not None else -1.0


def free_port() -> None:
    for conn in psutil.net_connections(kind="tcp"):
        if conn.laddr and conn.laddr.port == PORT and conn.status == "LISTEN" and conn.pid:
            try:
                psutil.Process(conn.pid).kill()
            except psutil.Error:
                pass
    time.sleep(0.4)


# ------------------------------------------------------------------- reporting


def report(rows: list[dict], counts: list[int]) -> None:
    for tier in ("small", "mid", "large"):
        for count in counts:
            picked = [row for row in rows
                      if row["tier"] == tier and row["count"] == count and "warm" in row]
            if not picked:
                continue
            picked.sort(key=lambda row: row["warm"]["meanMs"])
            print()
            print(f"## {tier} tier — sequential, {count} requests")
            print()
            print("| Language | Cold start | Cold mean | Warm mean | p50 | p90 | p99 | Max "
                  "| RPS | CPU ms/req | Idle RSS | Peak RSS |")
            print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
            for row in picked:
                warm = row["warm"]
                print(f"| {row['language']} | {row['coldStartMs']:.0f} ms "
                      f"| {row['cold']['meanMs']:.3f} ms | {warm['meanMs']:.3f} ms "
                      f"| {warm['p50Ms']:.3f} | {warm['p90Ms']:.3f} | {warm['p99Ms']:.3f} "
                      f"| {warm['maxMs']:.2f} | {warm['rps']:.0f} "
                      f"| {warm['cpuMsPerRequest']:.3f} "
                      f"| {row.get('idleRssMb', 0):.1f} MB | {warm['peakRssMb']:.1f} MB |")

    failed = [row for row in rows if "failed" in row]
    if failed:
        print()
        print("### Runs that failed")
        print()
        for row in failed:
            print(f"- {row['tier']}/{row['language']} at {row['count']}: {row['failed']}")

    skipped = [row for row in rows if "skipped" in row]
    if skipped:
        print()
        print("### Phases skipped as too slow to run")
        print()
        for row in skipped:
            print(f"- {row['tier']}/{row['language']} at {row['count']}: {row['skipped']}")

    suspect = [row for row in rows
               if "warm" in row and (row["warm"]["errors"]
                                     or any(int(code) >= 400 for code in row["warm"]["statuses"]))]
    if suspect:
        print()
        print("### Runs with a non-2xx or errored response")
        print()
        for row in suspect:
            print(f"- {row['tier']}/{row['language']} at {row['count']}: "
                  f"errors={row['warm']['errors']} statuses={row['warm']['statuses']}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier", choices=list(TIER_DIR))
    parser.add_argument("--lang", choices=LANGUAGES)
    parser.add_argument("--counts", default="100,1000,10000")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--out", default="perf-results.json")
    args = parser.parse_args()

    counts = [int(part) for part in args.counts.split(",")]
    tiers = list(TIER_DIR) if args.all or args.tier is None else [args.tier]
    languages = LANGUAGES if args.all or args.lang is None else [args.lang]

    output = ROOT / args.out
    rows = json.loads(output.read_text(encoding="utf-8")) if output.exists() else []
    rows = [row for row in rows
            if not (row["tier"] in tiers and row["language"] in languages
                    and row["count"] in counts)]

    for tier in tiers:
        for language in languages:
            for count in counts:
                print(f"--- {tier}/{language} at {count}", flush=True)
                row = measure(tier, language, count)
                if row is not None:
                    rows.append(row)
                    output.write_text(json.dumps(rows, indent=2), encoding="utf-8")

    report(rows, counts)
    print()
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
