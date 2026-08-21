"""Validate the structured log that a mid-tier or large-tier server writes.

Usage:  python tools/check_logs.py <captured-stdout-file>

The specification requires exactly one JSON object per completed request, on its
own line, on stdout, with a fixed key order. This tool proves the logging is
real rather than decorative.
"""

import json
import sys
from pathlib import Path

MID_KEYS = ["level", "requestId", "method", "path", "status", "durationMs", "userId"]
LARGE_KEYS = MID_KEYS + ["quotaRemaining", "auditSeq"]
LEVELS = {"info", "warn", "error"}
METHODS = {"GET", "POST", "PUT", "PATCH", "DELETE"}

path = Path(sys.argv[1])
if not path.exists():
    print(f"FAIL the capture file {path} does not exist")
    sys.exit(1)

problems: list[str] = []
records = 0

for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
    stripped = line.strip()
    if stripped == "":
        continue
    try:
        entry = json.loads(stripped)
    except json.JSONDecodeError:
        problems.append(f"line {number} is not JSON: {stripped[:120]!r}")
        continue
    if not isinstance(entry, dict):
        problems.append(f"line {number} is not a JSON object")
        continue
    records += 1
    if list(entry) not in (MID_KEYS, LARGE_KEYS):
        problems.append(f"line {number} key order {list(entry)} is neither "
                        f"{MID_KEYS} nor {LARGE_KEYS}")
        continue
    if list(entry) == LARGE_KEYS:
        if entry["quotaRemaining"] is not None and (
                not isinstance(entry["quotaRemaining"], int) or entry["quotaRemaining"] < 0):
            problems.append(f"line {number} quotaRemaining {entry['quotaRemaining']!r} "
                            "is not a non-negative int or null")
        if not isinstance(entry["auditSeq"], int) or entry["auditSeq"] < 0:
            problems.append(f"line {number} auditSeq {entry['auditSeq']!r} "
                            "is not a non-negative int")
    if entry["level"] not in LEVELS:
        problems.append(f"line {number} level {entry['level']!r} is unknown")
    if entry["method"] not in METHODS:
        problems.append(f"line {number} method {entry['method']!r} is unknown")
    if not isinstance(entry["requestId"], str) or entry["requestId"] == "":
        problems.append(f"line {number} requestId is empty")
    if not isinstance(entry["path"], str) or not entry["path"].startswith("/"):
        problems.append(f"line {number} path {entry['path']!r} is not a path")
    if "?" in str(entry["path"]):
        problems.append(f"line {number} path {entry['path']!r} still carries a query string")
    if not isinstance(entry["status"], int) or not 100 <= entry["status"] <= 599:
        problems.append(f"line {number} status {entry['status']!r} is not a status code")
    if not isinstance(entry["durationMs"], int) or entry["durationMs"] < 0:
        problems.append(f"line {number} durationMs {entry['durationMs']!r} is not valid")
    if entry["userId"] is not None and not isinstance(entry["userId"], int):
        problems.append(f"line {number} userId {entry['userId']!r} is not an int or null")
    status, level = entry["status"], entry["level"]
    want = "error" if status >= 500 else "warn" if status >= 400 else "info"
    if level != want:
        problems.append(f"line {number} status {status} wants level {want!r}, got {level!r}")

if records == 0:
    problems.append("no log records were written at all")

for problem in problems[:25]:
    print("FAIL " + problem)
if len(problems) > 25:
    print(f"FAIL ... and {len(problems) - 25} more")
print(f"{records} log records, {len(problems)} problems")
sys.exit(1 if problems else 0)
