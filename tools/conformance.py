"""Conformance test for the Task Service. Run it against a live server.

Usage:  python tools/conformance.py [base_url]
Default base_url is http://127.0.0.1:8080
"""

import json
import sys
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
LONG_TITLE = "x" * 81

failures: list[str] = []
passes = 0


def call(method: str, path: str, body: str | None = None):
    """Return (status, parsed_json_or_None, raw_text)."""
    data = body.encode("utf-8") if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as res:
            raw = res.read().decode("utf-8")
            status = res.status
    except urllib.error.HTTPError as err:
        raw = err.read().decode("utf-8")
        status = err.code
    try:
        parsed = json.loads(raw) if raw.strip() else None
    except json.JSONDecodeError:
        parsed = None
    return status, parsed, raw


def check(number: int, label: str, method: str, path: str, body, want_status: int, want_body):
    global passes
    status, parsed, raw = call(method, path, body)
    problems = []
    if status != want_status:
        problems.append(f"status {status} != {want_status}")
    if want_body is None:
        if raw.strip() != "":
            problems.append(f"body {raw!r} is not empty")
    elif not matches(parsed, want_body):
        problems.append(f"body {raw!r} != {want_body!r}")
    if problems:
        failures.append(f"case {number:>2} {label}: " + "; ".join(problems))
    else:
        passes += 1


def matches(got, want) -> bool:
    """Deep compare. A float compares with a small tolerance."""
    if isinstance(want, dict):
        if not isinstance(got, dict) or set(got) != set(want):
            return False
        return all(matches(got[key], want[key]) for key in want)
    if isinstance(want, list):
        if not isinstance(got, list) or len(got) != len(want):
            return False
        return all(matches(g, w) for g, w in zip(got, want))
    if isinstance(want, bool) or isinstance(got, bool):
        return got is want
    if isinstance(want, float):
        return isinstance(got, (int, float)) and abs(got - want) < 1e-9
    return got == want


def task(task_id: int, title: str, priority: int, done: bool, score: int) -> dict:
    return {"id": task_id, "title": title, "priority": priority, "done": done, "score": score}


T1 = task(1, "write spec", 3, False, 35)
T2 = task(2, "ship it", 5, False, 55)
T3 = task(3, "cleanup", 1, False, 15)
T3_DONE = task(3, "cleanup done", 2, True, 20)

check(1, "GET /health empty", "GET", "/health", None, 200, {"status": "ok", "count": 0})
check(2, "POST create 1", "POST", "/tasks", '{"title":"write spec","priority":3}', 201, T1)
check(3, "POST create 2", "POST", "/tasks", '{"title":"ship it","priority":5}', 201, T2)
check(4, "POST create 3", "POST", "/tasks", '{"title":"cleanup","priority":1}', 201, T3)
check(5, "POST empty title", "POST", "/tasks", '{"title":"","priority":3}', 400,
      {"error": "title is required"})
check(6, "POST bad priority", "POST", "/tasks", '{"title":"x","priority":9}', 400,
      {"error": "priority is out of range"})
check(7, "POST long title", "POST", "/tasks",
      json.dumps({"title": LONG_TITLE, "priority": 3}), 400, {"error": "title is too long"})
check(8, "POST bad json", "POST", "/tasks", "{not json", 400, {"error": "invalid json"})
check(9, "GET one", "GET", "/tasks/2", None, 200, T2)
check(10, "GET missing", "GET", "/tasks/99", None, 404, {"error": "task not found"})
check(11, "GET bad id", "GET", "/tasks/abc", None, 400, {"error": "invalid id"})
check(12, "PUT update", "PUT", "/tasks/3",
      '{"title":"cleanup done","priority":2,"done":true}', 200, T3_DONE)
check(13, "PUT missing", "PUT", "/tasks/99",
      '{"title":"nope","priority":2,"done":true}', 404, {"error": "task not found"})
check(14, "GET all sorted", "GET", "/tasks", None, 200, {"tasks": [T2, T1, T3_DONE], "total": 3})
check(15, "GET done=true", "GET", "/tasks?done=true", None, 200, {"tasks": [T3_DONE], "total": 1})
check(16, "GET done=false", "GET", "/tasks?done=false", None, 200, {"tasks": [T2, T1], "total": 2})
check(17, "GET bad filter", "GET", "/tasks?done=maybe", None, 400,
      {"error": "done must be true or false"})
check(18, "GET stats", "GET", "/stats", None, 200,
      {"total": 3, "doneCount": 1, "openCount": 2, "avgScore": 36.67, "topOpenTitle": "ship it"})
check(19, "DELETE one", "DELETE", "/tasks/1", None, 204, None)
check(20, "DELETE again", "DELETE", "/tasks/1", None, 404, {"error": "task not found"})
check(21, "GET /health after delete", "GET", "/health", None, 200, {"status": "ok", "count": 2})
check(22, "GET unknown route", "GET", "/nope", None, 404, {"error": "not found"})

total = passes + len(failures)
for line in failures:
    print("FAIL " + line)
print(f"{passes}/{total} cases pass")
sys.exit(1 if failures else 0)
