"""Conformance test for the mid-tier Task Service.

Usage:  python tools/conformance_mid.py [base_url]
Default base_url is http://127.0.0.1:8080

The cases run in order and depend on each other. The server must start fresh.
"""

import json
import sys
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"

failures: list[str] = []
passes = 0
counter = 0
tokens: dict[str, str] = {}


def call(method, path, body=None, token=None, request_id=None, raw_body=None):
    """Return (status, parsed, text, headers)."""
    if raw_body is not None:
        data = raw_body.encode("utf-8")
    elif body is not None:
        data = json.dumps(body).encode("utf-8")
    else:
        data = None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token is not None:
        req.add_header("Authorization", token)
    if request_id is not None:
        req.add_header("X-Request-Id", request_id)
    try:
        with urllib.request.urlopen(req, timeout=15) as res:
            text = res.read().decode("utf-8")
            status, raw_headers = res.status, res.headers
    except urllib.error.HTTPError as err:
        text = err.read().decode("utf-8")
        status, raw_headers = err.code, err.headers
    # HTTP header names are case-insensitive; servers differ on the casing sent.
    headers = {name.lower(): value for name, value in raw_headers.items()}
    try:
        parsed = json.loads(text) if text.strip() else None
    except json.JSONDecodeError:
        parsed = None
    return status, parsed, text, headers


def bearer(name):
    return "Bearer " + tokens[name]


def matches(got, want):
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


def record(label, problems):
    global counter, passes
    counter += 1
    if problems:
        failures.append(f"case {counter:>2} {label}: " + "; ".join(problems))
    else:
        passes += 1


def check(label, method, path, want_status, want_body, **kwargs):
    status, parsed, text, headers = call(method, path, **kwargs)
    problems = []
    if status != want_status:
        problems.append(f"status {status} != {want_status}")
    if want_body is None:
        if text.strip() != "":
            problems.append(f"body {text!r} is not empty")
    elif not matches(parsed, want_body):
        problems.append(f"body {text!r} != {want_body!r}")
    record(label, problems)
    return parsed, headers


def check_error(label, method, path, want_status, code, message, details=None, **kwargs):
    status, parsed, text, headers = call(method, path, **kwargs)
    problems = []
    if status != want_status:
        problems.append(f"status {status} != {want_status}")
    envelope = parsed.get("error") if isinstance(parsed, dict) else None
    if not isinstance(envelope, dict):
        problems.append(f"body {text!r} has no error object")
    else:
        if set(envelope) != {"code", "message", "requestId", "details"}:
            problems.append(f"error keys {sorted(envelope)} are wrong")
        if envelope.get("code") != code:
            problems.append(f"code {envelope.get('code')!r} != {code!r}")
        if envelope.get("message") != message:
            problems.append(f"message {envelope.get('message')!r} != {message!r}")
        request_id = envelope.get("requestId")
        if not isinstance(request_id, str) or request_id == "":
            problems.append(f"requestId {request_id!r} is empty")
        if not matches(envelope.get("details"), details if details is not None else []):
            problems.append(f"details {envelope.get('details')!r} != {details!r}")
    record(label, problems)
    return parsed, headers


def detail(field, message):
    return {"field": field, "message": message}


def project(pid, name, owner_id, task_count):
    return {"id": pid, "name": name, "ownerId": owner_id, "taskCount": task_count}


def task(tid, project_id, title, priority, status, assignee_id, score):
    return {"id": tid, "projectId": project_id, "title": title, "priority": priority,
            "status": status, "assigneeId": assignee_id, "score": score}


def page(items, total, limit=20, offset=0):
    return {"items": items, "total": total, "limit": limit, "offset": offset}


UNAUTH = ("unauthorized", "authentication is required")
FORBID = ("forbidden", "you may not access this resource")
NOTFOUND = ("not_found", "the resource does not exist")
CONFLICT = ("conflict", "the resource already exists")
TRANSITION = ("invalid_transition", "the status change is not allowed")
INVALID = ("validation_failed", "the request body is not valid")
MALFORMED = ("bad_request", "the request is malformed")
CREDS = ("invalid_credentials", "the username or password is wrong")

# ---------------------------------------------------------------- health, auth

check("health empty", "GET", "/health", 200, {"status": "ok", "projects": 0, "tasks": 0})
check_error("me without a token", "GET", "/me", 401, *UNAUTH)
check_error("me with a bad scheme", "GET", "/me", 401, *UNAUTH, token="Token abc")
check_error("me with an unknown token", "GET", "/me", 401, *UNAUTH, token="Bearer nope")
check_error("login with an empty body", "POST", "/auth/login", 422, *INVALID,
            details=[detail("password", "password is required"),
                     detail("username", "username is required")], body={})
check_error("login with a wrong password", "POST", "/auth/login", 401, *CREDS,
            body={"username": "admin", "password": "wrong"})
check_error("login with an unknown user", "POST", "/auth/login", 401, *CREDS,
            body={"username": "ghost", "password": "x"})
check_error("login with a malformed body", "POST", "/auth/login", 400, *MALFORMED,
            raw_body="{not json")

for name, password, user_id, role in [("admin", "admin-secret", 1, "admin"),
                                      ("alice", "alice-secret", 2, "user"),
                                      ("bob", "bob-secret", 3, "user")]:
    status, parsed, text, _ = call("POST", "/auth/login",
                                   body={"username": name, "password": password})
    problems = []
    if status != 200:
        problems.append(f"status {status} != 200")
    if not isinstance(parsed, dict) or set(parsed) != {"token", "userId", "role"}:
        problems.append(f"body {text!r} has wrong keys")
    else:
        if parsed["userId"] != user_id:
            problems.append(f"userId {parsed['userId']} != {user_id}")
        if parsed["role"] != role:
            problems.append(f"role {parsed['role']!r} != {role!r}")
        if not isinstance(parsed["token"], str) or len(parsed["token"]) < 16:
            problems.append(f"token {parsed['token']!r} is too short")
        else:
            tokens[name] = parsed["token"]
    record(f"login as {name}", problems)

if len(tokens) < 3:
    print("FAIL login did not return three tokens; the rest cannot run")
    print(f"{passes}/{counter} cases pass")
    sys.exit(1)

if len({tokens["admin"], tokens["alice"], tokens["bob"]}) != 3:
    failures.append("case -- tokens are not unique per session")

check("me as admin", "GET", "/me", 200,
      {"userId": 1, "username": "admin", "role": "admin"}, token=bearer("admin"))
check("me as alice", "GET", "/me", 200,
      {"userId": 2, "username": "alice", "role": "user"}, token=bearer("alice"))

# ------------------------------------------------------------------ request id

_, headers = check("health echoes the request id", "GET", "/health", 200,
                   {"status": "ok", "projects": 0, "tasks": 0}, request_id="probe-1")
record("X-Request-Id header echo",
       [] if headers.get("x-request-id") == "probe-1"
       else [f"header {headers.get('X-Request-Id')!r} != 'probe-1'"])

parsed = call("GET", "/me", request_id="probe-2")[1]
record("error body carries the request id",
       [] if isinstance(parsed, dict) and parsed.get("error", {}).get("requestId") == "probe-2"
       else [f"requestId is not 'probe-2' in {parsed!r}"])

_, headers = check("generated request id when none is sent", "GET", "/health", 200,
                   {"status": "ok", "projects": 0, "tasks": 0})
generated = headers.get("x-request-id")
record("generated request id is long enough",
       [] if isinstance(generated, str) and len(generated) >= 8
       else [f"header {generated!r} is missing or too short"])

# -------------------------------------------------------------------- projects

check_error("create a project as a user", "POST", "/projects", 403, *FORBID,
            body={"name": "Nope"}, token=bearer("alice"))
check_error("create a project with an empty name", "POST", "/projects", 422, *INVALID,
            details=[detail("name", "name is required")],
            body={"name": ""}, token=bearer("admin"))
check_error("create a project with a long name", "POST", "/projects", 422, *INVALID,
            details=[detail("name", "name is too long")],
            body={"name": "x" * 61}, token=bearer("admin"))
check_error("create a project with an unknown owner", "POST", "/projects", 422, *INVALID,
            details=[detail("ownerId", "ownerId is not a known user")],
            body={"name": "Apollo", "ownerId": 99}, token=bearer("admin"))
check_error("create a project with a malformed body", "POST", "/projects", 400, *MALFORMED,
            raw_body="{", token=bearer("admin"))

check("create project 1", "POST", "/projects", 201, project(1, "Apollo", 2, 0),
      body={"name": "Apollo", "ownerId": 2}, token=bearer("admin"))
check_error("create a duplicate name for the same owner", "POST", "/projects", 409, *CONFLICT,
            body={"name": "Apollo", "ownerId": 2}, token=bearer("admin"))
check("same name for a different owner", "POST", "/projects", 201, project(2, "Apollo", 3, 0),
      body={"name": "Apollo", "ownerId": 3}, token=bearer("admin"))
check("ownerId defaults to the caller", "POST", "/projects", 201, project(3, "Zephyr", 1, 0),
      body={"name": "Zephyr"}, token=bearer("admin"))
check("create project 4", "POST", "/projects", 201, project(4, "Borealis", 2, 0),
      body={"name": "Borealis", "ownerId": 2}, token=bearer("admin"))

P1 = project(1, "Apollo", 2, 0)
P2 = project(2, "Apollo", 3, 0)
P3 = project(3, "Zephyr", 1, 0)
P4 = project(4, "Borealis", 2, 0)

check("admin lists every project", "GET", "/projects", 200,
      page([P1, P2, P3, P4], 4), token=bearer("admin"))
check("alice lists only her projects", "GET", "/projects", 200,
      page([P1, P4], 2), token=bearer("alice"))
check("bob lists only his project", "GET", "/projects", 200,
      page([P2], 1), token=bearer("bob"))
check("projects paginate", "GET", "/projects?limit=1&offset=1", 200,
      page([P2], 4, limit=1, offset=1), token=bearer("admin"))
check("projects sort by name descending", "GET", "/projects?sort=name&order=desc", 200,
      page([P3, P4, P1, P2], 4), token=bearer("admin"))
check("projects offset past the end", "GET", "/projects?offset=99", 200,
      page([], 4, offset=99), token=bearer("admin"))

check_error("bad sort field", "GET", "/projects?sort=bogus", 422, *INVALID,
            details=[detail("sort", "sort is not a valid field")], token=bearer("admin"))
check_error("limit of zero", "GET", "/projects?limit=0", 422, *INVALID,
            details=[detail("limit", "limit is out of range")], token=bearer("admin"))
check_error("limit above the maximum", "GET", "/projects?limit=101", 422, *INVALID,
            details=[detail("limit", "limit is out of range")], token=bearer("admin"))
check_error("non-numeric limit", "GET", "/projects?limit=abc", 422, *INVALID,
            details=[detail("limit", "limit is out of range")], token=bearer("admin"))
check_error("negative offset", "GET", "/projects?offset=-1", 422, *INVALID,
            details=[detail("offset", "offset is out of range")], token=bearer("admin"))
check_error("bad order", "GET", "/projects?order=sideways", 422, *INVALID,
            details=[detail("order", "order must be asc or desc")], token=bearer("admin"))
check_error("three bad parameters together", "GET", "/projects?limit=0&order=x&sort=y",
            422, *INVALID,
            details=[detail("limit", "limit is out of range"),
                     detail("order", "order must be asc or desc"),
                     detail("sort", "sort is not a valid field")], token=bearer("admin"))

check("alice reads her project", "GET", "/projects/1", 200, P1, token=bearer("alice"))
check_error("alice reads bob's project", "GET", "/projects/2", 403, *FORBID,
            token=bearer("alice"))
check_error("404 wins over 403", "GET", "/projects/99", 404, *NOTFOUND, token=bearer("alice"))
check_error("non-integer project id", "GET", "/projects/abc", 400, *MALFORMED,
            token=bearer("alice"))

check_error("alice patches a project", "PATCH", "/projects/1", 403, *FORBID,
            body={"name": "Hers"}, token=bearer("alice"))
check("admin renames project 1", "PATCH", "/projects/1", 200, project(1, "Apollo II", 2, 0),
      body={"name": "Apollo II"}, token=bearer("admin"))
check("an empty patch changes nothing", "PATCH", "/projects/1", 200,
      project(1, "Apollo II", 2, 0), body={}, token=bearer("admin"))
check("renaming to its own name is allowed", "PATCH", "/projects/1", 200,
      project(1, "Apollo II", 2, 0), body={"name": "Apollo II"}, token=bearer("admin"))
check_error("renaming onto a sibling name", "PATCH", "/projects/4", 409, *CONFLICT,
            body={"name": "Apollo II"}, token=bearer("admin"))

P1 = project(1, "Apollo II", 2, 0)

# ----------------------------------------------------------------------- tasks

check_error("bob creates a task in alice's project", "POST", "/projects/1/tasks", 403, *FORBID,
            body={"title": "Nope", "priority": 1}, token=bearer("bob"))
check_error("three broken task fields", "POST", "/projects/1/tasks", 422, *INVALID,
            details=[detail("assigneeId", "assigneeId is not a known user"),
                     detail("priority", "priority is out of range"),
                     detail("title", "title is required")],
            body={"title": "", "priority": 9, "assigneeId": 99}, token=bearer("alice"))
check_error("a missing priority defaults to zero", "POST", "/projects/1/tasks", 422, *INVALID,
            details=[detail("priority", "priority is out of range")],
            body={"title": "Design"}, token=bearer("alice"))
check_error("tasks under an unknown project", "POST", "/projects/99/tasks", 404, *NOTFOUND,
            body={"title": "Design", "priority": 1}, token=bearer("alice"))

check("create task 1", "POST", "/projects/1/tasks", 201,
      task(1, 1, "Design", 3, "todo", None, 30),
      body={"title": "Design", "priority": 3}, token=bearer("alice"))
check("create task 2 with an assignee", "POST", "/projects/1/tasks", 201,
      task(2, 1, "Build", 5, "todo", 2, 50),
      body={"title": "Build", "priority": 5, "assigneeId": 2}, token=bearer("alice"))
check("admin creates task 3 in alice's project", "POST", "/projects/1/tasks", 201,
      task(3, 1, "Ship", 1, "todo", None, 10),
      body={"title": "Ship", "priority": 1}, token=bearer("admin"))
check("create task 4 in project 4", "POST", "/projects/4/tasks", 201,
      task(4, 4, "Other", 2, "todo", None, 20),
      body={"title": "Other", "priority": 2}, token=bearer("alice"))

T1 = task(1, 1, "Design", 3, "todo", None, 30)
T2 = task(2, 1, "Build", 5, "todo", 2, 50)
T3 = task(3, 1, "Ship", 1, "todo", None, 10)

check("list the tasks of project 1", "GET", "/projects/1/tasks", 200,
      page([T1, T2, T3], 3), token=bearer("alice"))
check("tasks sort by priority descending", "GET", "/projects/1/tasks?sort=priority&order=desc",
      200, page([T2, T1, T3], 3), token=bearer("alice"))
check("tasks sort by title", "GET", "/projects/1/tasks?sort=title", 200,
      page([T2, T1, T3], 3), token=bearer("alice"))
check("an equal sort key falls back to id", "GET", "/projects/1/tasks?sort=status", 200,
      page([T1, T2, T3], 3), token=bearer("alice"))
check_error("bad task sort field", "GET", "/projects/1/tasks?sort=bogus", 422, *INVALID,
            details=[detail("sort", "sort is not a valid field")], token=bearer("alice"))
check_error("bob lists alice's tasks", "GET", "/projects/1/tasks", 403, *FORBID,
            token=bearer("bob"))

check("taskCount is derived", "GET", "/projects/1", 200, project(1, "Apollo II", 2, 3),
      token=bearer("alice"))

check("read task 1", "GET", "/tasks/1", 200, T1, token=bearer("alice"))
check_error("bob reads alice's task", "GET", "/tasks/1", 403, *FORBID, token=bearer("bob"))
check_error("unknown task", "GET", "/tasks/99", 404, *NOTFOUND, token=bearer("alice"))

check("replace task 1", "PUT", "/tasks/1", 200,
      task(1, 1, "Design v2", 4, "todo", 3, 40),
      body={"title": "Design v2", "priority": 4, "assigneeId": 3}, token=bearer("alice"))
check_error("replace task 1 with bad fields", "PUT", "/tasks/1", 422, *INVALID,
            details=[detail("priority", "priority is out of range")],
            body={"title": "x", "priority": 0}, token=bearer("alice"))

# -------------------------------------------------------------- state machine

check_error("status is required", "PATCH", "/tasks/1/status", 422, *INVALID,
            details=[detail("status", "status is not valid")], body={}, token=bearer("alice"))
check_error("status must be known", "PATCH", "/tasks/1/status", 422, *INVALID,
            details=[detail("status", "status is not valid")],
            body={"status": "bogus"}, token=bearer("alice"))
check_error("todo to todo is not allowed", "PATCH", "/tasks/1/status", 409, *TRANSITION,
            body={"status": "todo"}, token=bearer("alice"))
check_error("todo to done is not allowed", "PATCH", "/tasks/1/status", 409, *TRANSITION,
            body={"status": "done"}, token=bearer("alice"))
check("todo to in_progress", "PATCH", "/tasks/1/status", 200,
      task(1, 1, "Design v2", 4, "in_progress", 3, 43),
      body={"status": "in_progress"}, token=bearer("alice"))
check("in_progress to done", "PATCH", "/tasks/1/status", 200,
      task(1, 1, "Design v2", 4, "done", 3, 45),
      body={"status": "done"}, token=bearer("alice"))
check_error("done to todo is not allowed", "PATCH", "/tasks/1/status", 409, *TRANSITION,
            body={"status": "todo"}, token=bearer("alice"))
check("done to archived", "PATCH", "/tasks/1/status", 200,
      task(1, 1, "Design v2", 4, "archived", 3, 40),
      body={"status": "archived"}, token=bearer("alice"))
check_error("archived is terminal", "PATCH", "/tasks/1/status", 409, *TRANSITION,
            body={"status": "todo"}, token=bearer("alice"))

# ----------------------------------------------------------------------- stats

check_error("alice reads stats", "GET", "/stats", 403, *FORBID, token=bearer("alice"))
check("admin reads stats", "GET", "/stats", 200, {
    "projects": 4, "tasks": 4, "users": 3, "sessions": 3,
    "byStatus": {"todo": 3, "in_progress": 0, "done": 0, "archived": 1},
    "avgScore": 30.0, "topProjectName": "Apollo II",
}, token=bearer("admin"))

# -------------------------------------------------------- delete and cascade

check("delete task 3", "DELETE", "/tasks/3", 204, None, token=bearer("alice"))
check_error("delete task 3 twice", "DELETE", "/tasks/3", 404, *NOTFOUND, token=bearer("alice"))
check("taskCount drops", "GET", "/projects/1", 200, project(1, "Apollo II", 2, 2),
      token=bearer("alice"))
check_error("alice deletes a project", "DELETE", "/projects/1", 403, *FORBID,
            token=bearer("alice"))
check("admin deletes project 1", "DELETE", "/projects/1", 204, None, token=bearer("admin"))
check_error("the cascade removed task 1", "GET", "/tasks/1", 404, *NOTFOUND,
            token=bearer("admin"))
check_error("the cascade removed task 2", "GET", "/tasks/2", 404, *NOTFOUND,
            token=bearer("admin"))
check_error("project 1 is gone", "GET", "/projects/1", 404, *NOTFOUND, token=bearer("admin"))
check("health after the cascade", "GET", "/health", 200,
      {"status": "ok", "projects": 3, "tasks": 1})

# --------------------------------------------------------------------- logout

check("bob logs out", "POST", "/auth/logout", 204, None, token=bearer("bob"))
check_error("bob's token is dead", "GET", "/me", 401, *UNAUTH, token=bearer("bob"))
check("sessions dropped by one", "GET", "/stats", 200, {
    "projects": 3, "tasks": 1, "users": 3, "sessions": 2,
    "byStatus": {"todo": 1, "in_progress": 0, "done": 0, "archived": 0},
    "avgScore": 20.0, "topProjectName": "Borealis",
}, token=bearer("admin"))

status, parsed, text, _ = call("POST", "/auth/login",
                               body={"username": "alice", "password": "alice-secret"})
second = parsed.get("token") if isinstance(parsed, dict) else None
record("a second login issues a second token",
       [] if status == 200 and isinstance(second, str) and second != tokens["alice"]
       else [f"status {status}, token {second!r} vs {tokens['alice']!r}"])
check("the first alice token still works", "GET", "/me", 200,
      {"userId": 2, "username": "alice", "role": "user"}, token=bearer("alice"))
if isinstance(second, str):
    check("the second alice token works too", "GET", "/me", 200,
          {"userId": 2, "username": "alice", "role": "user"}, token="Bearer " + second)

# ------------------------------------------------------------------- fallback

check_error("an unknown path", "GET", "/nope", 404, *NOTFOUND, token=bearer("admin"))
check_error("an unknown nested path", "GET", "/projects/1/nope", 404, *NOTFOUND,
            token=bearer("admin"))

for line in failures:
    print("FAIL " + line)
print(f"{passes}/{counter} cases pass")
sys.exit(1 if failures else 0)
