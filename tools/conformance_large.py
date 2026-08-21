"""Conformance test for the large-tier Task Service.

Usage:  python tools/conformance_large.py [base_url]
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


def call(method, path, body=None, token=None, raw_body=None, headers=None):
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
        req.add_header("Authorization", "Bearer " + tokens.get(token, token))
    for name, value in (headers or {}).items():
        req.add_header(name, value)
    try:
        with urllib.request.urlopen(req, timeout=15) as res:
            text, status, raw = res.read().decode("utf-8"), res.status, res.headers
    except urllib.error.HTTPError as err:
        text, status, raw = err.read().decode("utf-8"), err.code, err.headers
    head = {name.lower(): value for name, value in raw.items()}
    try:
        parsed = json.loads(text) if text.strip() else None
    except json.JSONDecodeError:
        parsed = None
    return status, parsed, text, head


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
        failures.append(f"case {counter:>3} {label}: " + "; ".join(problems))
    else:
        passes += 1


def check(label, method, path, want_status, want_body, etag=None, quota=None,
          replayed=False, **kwargs):
    status, parsed, text, head = call(method, path, **kwargs)
    problems = []
    if status != want_status:
        problems.append(f"status {status} != {want_status}")
    if want_body is None:
        if text.strip() != "":
            problems.append(f"body {text!r} is not empty")
    elif not matches(parsed, want_body):
        problems.append(f"body {text!r} != {want_body!r}")
    if etag is not None and head.get("etag") != str(etag):
        problems.append(f"ETag {head.get('etag')!r} != {str(etag)!r}")
    if quota is not None and head.get("x-quota-remaining") != str(quota):
        problems.append(f"X-Quota-Remaining {head.get('x-quota-remaining')!r} != {quota}")
    if replayed and head.get("idempotency-replayed") != "true":
        problems.append(f"Idempotency-Replayed {head.get('idempotency-replayed')!r} != 'true'")
    record(label, problems)
    return parsed, head


def check_error(label, method, path, want_status, code, message, details=None, **kwargs):
    status, parsed, text, head = call(method, path, **kwargs)
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
        if not isinstance(envelope.get("requestId"), str) or envelope.get("requestId") == "":
            problems.append("requestId is empty")
        if not matches(envelope.get("details"), details or []):
            problems.append(f"details {envelope.get('details')!r} != {details!r}")
    record(label, problems)
    return parsed, head


def check_shape(label, method, path, want_status, keys, **kwargs):
    """Assert the status and the exact top-level key set, not the values."""
    status, parsed, text, head = call(method, path, **kwargs)
    problems = []
    if status != want_status:
        problems.append(f"status {status} != {want_status}")
    if not isinstance(parsed, dict) or set(parsed) != set(keys):
        problems.append(f"keys {sorted(parsed) if isinstance(parsed, dict) else text!r} "
                        f"!= {sorted(keys)}")
    record(label, problems)
    return parsed, head


def ids_of(parsed, field="items"):
    if not isinstance(parsed, dict) or not isinstance(parsed.get(field), list):
        return None
    return [row.get("id", row.get("seq")) for row in parsed[field]]


def check_ids(label, method, path, want_total, want_ids, **kwargs):
    status, parsed, text, head = call(method, path, **kwargs)
    problems = []
    if status != 200:
        problems.append(f"status {status} != 200")
    elif parsed.get("total") != want_total:
        problems.append(f"total {parsed.get('total')} != {want_total}")
    elif ids_of(parsed) != want_ids:
        problems.append(f"ids {ids_of(parsed)} != {want_ids}")
    record(label, problems)
    return parsed


def detail(field, message):
    return {"field": field, "message": message}


def user(uid, username, role, quota, version, deleted=False):
    return {"id": uid, "username": username, "role": role, "quota": quota,
            "version": version, "deleted": deleted}


def project(pid, name, owner_id, task_count, version, deleted=False):
    return {"id": pid, "name": name, "ownerId": owner_id, "taskCount": task_count,
            "version": version, "deleted": deleted}


def task(tid, project_id, title, priority, status, assignee_id, version, score,
         note=None, deleted=False):
    body = {"id": tid, "projectId": project_id, "title": title, "priority": priority,
            "status": status, "assigneeId": assignee_id}
    if note is not None:
        body["internalNote"] = note
    body["version"] = version
    body["deleted"] = deleted
    body["score"] = score
    return body


def comment(cid, task_id, author_id, body):
    return {"id": cid, "taskId": task_id, "authorId": author_id, "body": body}


UNAUTH = ("unauthorized", "authentication is required")
CREDS = ("invalid_credentials", "the username or password is wrong")
FORBID = ("forbidden", "you may not access this resource")
NOTFOUND = ("not_found", "the resource does not exist")
CONFLICT = ("conflict", "the resource already exists")
TRANSITION = ("invalid_transition", "the status change is not allowed")
INVALID = ("validation_failed", "the request body is not valid")
MALFORMED = ("bad_request", "the request is malformed")
STALE = ("precondition_failed", "the resource has changed")
NEEDMATCH = ("precondition_required", "the If-Match header is required")
NOQUOTA = ("quota_exceeded", "the request quota is exhausted")


def match(version):
    return {"If-Match": str(version)}


# =========================================================== A. health and auth

check("health is empty", "GET", "/health", 200,
      {"status": "ok", "projects": 0, "tasks": 0, "comments": 0})
check_error("me without a token", "GET", "/me", 401, *UNAUTH)
check_error("me with a bad scheme", "GET", "/me", 401, *UNAUTH, token="raw-token")
check_error("login with an empty body", "POST", "/auth/login", 422, *INVALID,
            details=[detail("password", "password is required"),
                     detail("username", "username is required")], body={})
check_error("login with a wrong password", "POST", "/auth/login", 401, *CREDS,
            body={"username": "admin", "password": "wrong"})
check_error("login with a malformed body", "POST", "/auth/login", 400, *MALFORMED,
            raw_body="{not json")

for name, password, uid, role in [("admin", "admin-secret", 1, "admin"),
                                  ("alice", "alice-secret", 2, "user"),
                                  ("bob", "bob-secret", 3, "user"),
                                  ("probe", "probe-secret", 4, "user")]:
    status, parsed, text, _ = call("POST", "/auth/login",
                                   body={"username": name, "password": password})
    problems = []
    if status != 200 or not isinstance(parsed, dict):
        problems.append(f"status {status}, body {text!r}")
    elif set(parsed) != {"token", "userId", "role"}:
        problems.append(f"keys {sorted(parsed)} are wrong")
    elif parsed["userId"] != uid or parsed["role"] != role:
        problems.append(f"userId/role {parsed['userId']}/{parsed['role']!r}")
    elif not isinstance(parsed["token"], str) or len(parsed["token"]) < 16:
        problems.append(f"token {parsed['token']!r} is too short")
    else:
        tokens[name] = parsed["token"]
    record(f"login as {name}", problems)

if len(tokens) < 4:
    print("FAIL login did not return four tokens; the rest cannot run")
    print(f"{passes}/{counter} cases pass")
    sys.exit(1)

check("me as admin", "GET", "/me", 200,
      {"userId": 1, "username": "admin", "role": "admin"}, token="admin")
check("me as alice", "GET", "/me", 200,
      {"userId": 2, "username": "alice", "role": "user"}, token="alice")
_, head = check("health echoes the request id", "GET", "/health", 200,
                {"status": "ok", "projects": 0, "tasks": 0, "comments": 0},
                headers={"X-Request-Id": "probe-1"})
record("X-Request-Id echo",
       [] if head.get("x-request-id") == "probe-1"
       else [f"header {head.get('x-request-id')!r} != 'probe-1'"])

# ================================================================== B. quota

for remaining in [4, 3, 2, 1, 0]:
    check(f"probe request, {remaining} left", "GET", "/me", 200,
          {"userId": 4, "username": "probe", "role": "user"},
          quota=remaining, token="probe")
check_error("probe runs out of quota", "GET", "/me", 429, *NOQUOTA, token="probe")

# =================================================================== C. users

check_error("alice lists users", "GET", "/users", 403, *FORBID, token="alice")
check_ids("admin lists users", "GET", "/users", 4, [1, 2, 3, 4], token="admin")
check_error("alice creates a user", "POST", "/users", 403, *FORBID,
            body={"username": "x", "password": "y"}, token="alice")
check_error("create a user with no fields", "POST", "/users", 422, *INVALID,
            details=[detail("password", "password is required"),
                     detail("username", "username is required")],
            body={}, token="admin")
check("create carol", "POST", "/users", 201, user(5, "carol", "user", 10000, 1),
      etag=1, body={"username": "carol", "password": "carol-secret"}, token="admin")
check_error("carol twice", "POST", "/users", 409, *CONFLICT,
            body={"username": "carol", "password": "again"}, token="admin")
check_error("a bad role", "POST", "/users", 422, *INVALID,
            details=[detail("role", "role is not valid")],
            body={"username": "dave", "password": "y", "role": "boss"}, token="admin")
check_error("a negative quota", "POST", "/users", 422, *INVALID,
            details=[detail("quota", "quota is out of range")],
            body={"username": "dave", "password": "y", "quota": -1}, token="admin")
check("read carol", "GET", "/users/5", 200, user(5, "carol", "user", 10000, 1),
      etag=1, token="admin")
check_error("read an unknown user", "GET", "/users/99", 404, *NOTFOUND, token="admin")
check_error("patch carol with no If-Match", "PATCH", "/users/5", 428, *NEEDMATCH,
            body={"role": "admin"}, token="admin")
check_error("patch carol with a stale If-Match", "PATCH", "/users/5", 412, *STALE,
            body={"role": "admin"}, headers=match(9), token="admin")
check("promote carol", "PATCH", "/users/5", 200, user(5, "carol", "admin", 50, 2),
      etag=2, body={"role": "admin", "quota": 50}, headers=match(1), token="admin")
check_error("admin deletes itself", "DELETE", "/users/1", 409, *CONFLICT,
            headers=match(1), token="admin")
check("delete carol", "DELETE", "/users/5", 200,
      user(5, "carol", "admin", 50, 3, deleted=True), etag=3,
      headers=match(2), token="admin")
check_error("carol is invisible", "GET", "/users/5", 404, *NOTFOUND, token="admin")
check_ids("the user list drops carol", "GET", "/users", 4, [1, 2, 3, 4], token="admin")

# ================================================================ D. projects

check_error("alice creates a project", "POST", "/projects", 403, *FORBID,
            body={"name": "Nope"}, token="alice")
check_error("a project with no name", "POST", "/projects", 422, *INVALID,
            details=[detail("name", "name is required")], body={"name": ""}, token="admin")
check_error("a project with an unknown owner", "POST", "/projects", 422, *INVALID,
            details=[detail("ownerId", "ownerId is not a known user")],
            body={"name": "Apollo", "ownerId": 99}, token="admin")
check("create project 1", "POST", "/projects", 201, project(1, "Apollo", 2, 0, 1),
      etag=1, body={"name": "Apollo", "ownerId": 2}, token="admin")
check_error("the same name and owner", "POST", "/projects", 409, *CONFLICT,
            body={"name": "Apollo", "ownerId": 2}, token="admin")
check("the same name, another owner", "POST", "/projects", 201,
      project(2, "Apollo", 3, 0, 1), body={"name": "Apollo", "ownerId": 3}, token="admin")
check("ownerId defaults to the caller", "POST", "/projects", 201,
      project(3, "Zephyr", 1, 0, 1), body={"name": "Zephyr"}, token="admin")
check("create project 4", "POST", "/projects", 201, project(4, "Borealis", 2, 0, 1),
      body={"name": "Borealis", "ownerId": 2}, token="admin")

check_ids("admin sees every project", "GET", "/projects", 4, [1, 2, 3, 4], token="admin")
check_ids("alice sees her own", "GET", "/projects", 2, [1, 4], token="alice")
check_ids("bob sees his own", "GET", "/projects", 1, [2], token="bob")
check_ids("projects sort by name descending", "GET", "/projects?sort=name&order=desc",
          4, [3, 4, 1, 2], token="admin")
check_error("a user may not includeDeleted", "GET", "/projects?includeDeleted=true",
            403, *FORBID, token="alice")
check("alice reads her project", "GET", "/projects/1", 200, project(1, "Apollo", 2, 0, 1),
      etag=1, token="alice")
check_error("alice reads bob's project", "GET", "/projects/2", 403, *FORBID, token="alice")
check_error("404 beats 403", "GET", "/projects/99", 404, *NOTFOUND, token="alice")
check_error("a non-integer project id", "GET", "/projects/abc", 400, *MALFORMED,
            token="alice")
check_error("patch with no If-Match", "PATCH", "/projects/1", 428, *NEEDMATCH,
            body={"name": "X"}, token="admin")
check("rename project 1", "PATCH", "/projects/1", 200, project(1, "Apollo II", 2, 0, 2),
      etag=2, body={"name": "Apollo II"}, headers=match(1), token="admin")
check_error("rename onto a sibling", "PATCH", "/projects/4", 409, *CONFLICT,
            body={"name": "Apollo II"}, headers=match(1), token="admin")

# =================================================================== E. tasks

check_error("bob writes into project 1", "POST", "/projects/1/tasks", 403, *FORBID,
            body={"title": "No", "priority": 1}, token="bob")
check_error("three broken task fields", "POST", "/projects/1/tasks", 422, *INVALID,
            details=[detail("assigneeId", "assigneeId is not a known user"),
                     detail("priority", "priority is out of range"),
                     detail("title", "title is required")],
            body={"title": "", "priority": 9, "assigneeId": 99}, token="alice")
check_error("a user may not write internalNote", "POST", "/projects/1/tasks", 403, *FORBID,
            body={"title": "Design", "priority": 3, "internalNote": "no"}, token="alice")
check("alice creates task 1", "POST", "/projects/1/tasks", 201,
      task(1, 1, "Design", 3, "todo", None, 1, 30), etag=1,
      body={"title": "Design", "priority": 3}, token="alice")
check("admin creates task 2 with a note", "POST", "/projects/1/tasks", 201,
      task(2, 1, "Build", 5, "todo", 2, 1, 50, note="secret"), etag=1,
      body={"title": "Build", "priority": 5, "assigneeId": 2, "internalNote": "secret"},
      token="admin")
check("admin sees internalNote", "GET", "/tasks/2", 200,
      task(2, 1, "Build", 5, "todo", 2, 1, 50, note="secret"), etag=1, token="admin")
check("alice does not see internalNote", "GET", "/tasks/2", 200,
      task(2, 1, "Build", 5, "todo", 2, 1, 50), etag=1, token="alice")
check("admin creates task 3", "POST", "/projects/1/tasks", 201,
      task(3, 1, "Ship", 1, "todo", None, 1, 10, note=""),
      body={"title": "Ship", "priority": 1}, token="admin")
check("alice creates task 4", "POST", "/projects/4/tasks", 201,
      task(4, 4, "Other", 2, "todo", None, 1, 20),
      body={"title": "Other", "priority": 2}, token="alice")

check_ids("project 1 tasks", "GET", "/projects/1/tasks", 3, [1, 2, 3], token="alice")
check_ids("every reachable task for alice", "GET", "/tasks", 4, [1, 2, 3, 4], token="alice")
check_ids("bob reaches nothing", "GET", "/tasks", 0, [], token="bob")
check_ids("filter by status", "GET", "/tasks?status=todo", 4, [1, 2, 3, 4], token="alice")
check_ids("filter by assignee", "GET", "/tasks?assigneeId=2", 1, [2], token="alice")
check_error("a bad status filter", "GET", "/tasks?status=bogus", 422, *INVALID,
            details=[detail("status", "status is not valid")], token="alice")
check_error("a bad assignee filter", "GET", "/tasks?assigneeId=abc", 422, *INVALID,
            details=[detail("assigneeId", "assigneeId is not a known user")], token="alice")
check_ids("tasks sort by priority descending", "GET",
          "/projects/1/tasks?sort=priority&order=desc", 3, [2, 1, 3], token="alice")

check_error("put with no If-Match", "PUT", "/tasks/1", 428, *NEEDMATCH,
            body={"title": "X", "priority": 2}, token="alice")
check_error("put with a stale If-Match", "PUT", "/tasks/1", 412, *STALE,
            body={"title": "X", "priority": 2}, headers=match(7), token="alice")
check("replace task 1", "PUT", "/tasks/1", 200,
      task(1, 1, "Design v2", 4, "todo", 3, 2, 40), etag=2,
      body={"title": "Design v2", "priority": 4, "assigneeId": 3},
      headers=match(1), token="alice")
check_error("todo to done is blocked", "PATCH", "/tasks/1/status", 409, *TRANSITION,
            body={"status": "done"}, headers=match(2), token="alice")
check_error("an unknown status", "PATCH", "/tasks/1/status", 422, *INVALID,
            details=[detail("status", "status is not valid")],
            body={"status": "bogus"}, headers=match(2), token="alice")
check("todo to in_progress", "PATCH", "/tasks/1/status", 200,
      task(1, 1, "Design v2", 4, "in_progress", 3, 3, 43), etag=3,
      body={"status": "in_progress"}, headers=match(2), token="alice")

check("soft delete task 3", "DELETE", "/tasks/3", 200,
      task(3, 1, "Ship", 1, "todo", None, 2, 10, note="", deleted=True), etag=2,
      headers=match(1), token="admin")
check_error("a deleted task is invisible", "GET", "/tasks/3", 404, *NOTFOUND, token="alice")
check_ids("the list drops the deleted task", "GET", "/projects/1/tasks", 2, [1, 2],
          token="alice")
check("restore task 3", "POST", "/tasks/3/restore", 200,
      task(3, 1, "Ship", 1, "todo", None, 3, 10, note=""), etag=3,
      headers=match(2), token="admin")
check_error("restoring a live task", "POST", "/tasks/3/restore", 409, *CONFLICT,
            headers=match(3), token="admin")

# ================================================================ F. comments

check("alice comments", "POST", "/tasks/1/comments", 201, comment(1, 1, 2, "hi"),
      body={"body": "hi"}, token="alice")
check_error("an empty comment", "POST", "/tasks/1/comments", 422, *INVALID,
            details=[detail("body", "body is required")], body={"body": ""}, token="alice")
check("admin comments", "POST", "/tasks/1/comments", 201, comment(2, 1, 1, "noted"),
      body={"body": "noted"}, token="admin")
check_ids("list the comments", "GET", "/tasks/1/comments", 2, [1, 2], token="alice")
check_error("bob deletes a comment he cannot reach", "DELETE", "/comments/1", 403, *FORBID,
            token="bob")
check_error("alice deletes another author's comment", "DELETE", "/comments/2", 403, *FORBID,
            token="alice")
check("alice deletes her own comment", "DELETE", "/comments/1", 204, None, token="alice")
check_ids("one comment is left", "GET", "/tasks/1/comments", 1, [2], token="alice")
check_error("an unknown comment", "DELETE", "/comments/99", 404, *NOTFOUND, token="admin")

# ============================================================= G. idempotency

check("an idempotent create", "POST", "/projects", 201, project(5, "Idem", 1, 0, 1),
      etag=1, body={"name": "Idem", "ownerId": 1},
      headers={"Idempotency-Key": "key-1"}, token="admin")
check("the same key replays", "POST", "/projects", 201, project(5, "Idem", 1, 0, 1),
      etag=1, replayed=True, body={"name": "Idem", "ownerId": 1},
      headers={"Idempotency-Key": "key-1"}, token="admin")
check("a different key creates again", "POST", "/projects", 201,
      project(6, "Idem2", 1, 0, 1), body={"name": "Idem2", "ownerId": 1},
      headers={"Idempotency-Key": "key-2"}, token="admin")
check_error("an errored key is recorded", "POST", "/projects", 422, *INVALID,
            details=[detail("name", "name is required")], body={"name": ""},
            headers={"Idempotency-Key": "key-3"}, token="admin")
check_error("the error replays", "POST", "/projects", 422, *INVALID,
            details=[detail("name", "name is required")], body={"name": ""},
            headers={"Idempotency-Key": "key-3"}, token="admin")

# ================================================================== H. search

check_error("search with no q", "GET", "/search", 422, *INVALID,
            details=[detail("q", "q is required")], token="admin")
check("search finds both projects", "GET", "/search?q=apollo", 200,
      {"results": [{"type": "project", "id": 1, "label": "Apollo II"},
                   {"type": "project", "id": 2, "label": "Apollo"}], "total": 2},
      token="admin")
check("search is case-insensitive", "GET", "/search?q=APOLLO", 200,
      {"results": [{"type": "project", "id": 1, "label": "Apollo II"},
                   {"type": "project", "id": 2, "label": "Apollo"}], "total": 2},
      token="admin")
check("search matches a task title", "GET", "/search?q=design", 200,
      {"results": [{"type": "task", "id": 1, "label": "Design v2"}], "total": 1},
      token="alice")

# ================================================================= I. reports

check("workload by status", "GET", "/reports/workload", 200, {
    "groupBy": "status",
    "groups": [{"key": "todo", "tasks": 3, "totalScore": 80},
               {"key": "in_progress", "tasks": 1, "totalScore": 43},
               {"key": "done", "tasks": 0, "totalScore": 0},
               {"key": "archived", "tasks": 0, "totalScore": 0}]}, token="alice")
check_shape("workload by assignee", "GET", "/reports/workload?groupBy=assignee", 200,
            ["groupBy", "groups"], token="alice")
check_shape("workload by project", "GET", "/reports/workload?groupBy=project", 200,
            ["groupBy", "groups"], token="alice")
check_error("a bad groupBy", "GET", "/reports/workload?groupBy=bogus", 422, *INVALID,
            details=[detail("groupBy", "groupBy is not valid")], token="alice")

# ================================================ J. audit, outbox, telemetry

check_error("alice reads the audit log", "GET", "/audit", 403, *FORBID, token="alice")
audit_page = check_shape("admin reads the audit log", "GET", "/audit?limit=100", 200,
                         ["items", "total", "limit", "offset"], token="admin")[0]
record("audit entries have the right keys",
       [] if audit_page and audit_page["items"] and set(audit_page["items"][0]) == {
           "seq", "actorId", "action", "resource", "resourceId"}
       else [f"first entry {audit_page['items'][:1] if audit_page else None!r}"])
record("the audit log is ascending by seq",
       [] if audit_page and ids_of(audit_page) == sorted(ids_of(audit_page))
       else ["seq is not ascending"])
filtered = check_shape("filter the audit log by resource", "GET",
                       "/audit?resource=project&limit=100", 200,
                       ["items", "total", "limit", "offset"], token="admin")[0]
record("every filtered entry is a project",
       [] if filtered and all(row["resource"] == "project" for row in filtered["items"])
       else ["a non-project entry leaked through"])
check_shape("filter the audit log by action", "GET", "/audit?action=delete", 200,
            ["items", "total", "limit", "offset"], token="admin")
check_shape("filter the audit log by actor", "GET", "/audit?actorId=2", 200,
            ["items", "total", "limit", "offset"], token="admin")

outbox_page = check_shape("admin reads the outbox", "GET", "/outbox?limit=100", 200,
                          ["items", "total", "limit", "offset"], token="admin")[0]
record("outbox events have the right keys",
       [] if outbox_page and outbox_page["items"] and set(outbox_page["items"][0]) == {
           "seq", "name", "resourceId", "delivered"}
       else [f"first event {outbox_page['items'][:1] if outbox_page else None!r}"])
pending = check_shape("the undelivered outbox", "GET", "/outbox?delivered=false&limit=100",
                      200, ["items", "total", "limit", "offset"], token="admin")[0]
expected_flush = pending["total"] if pending else -1
check("flush the outbox", "POST", "/outbox/flush", 200, {"flushed": expected_flush},
      token="admin")
check("nothing is pending now", "GET", "/outbox?delivered=false", 200,
      {"items": [], "total": 0, "limit": 20, "offset": 0}, token="admin")

metrics = check_shape("metrics", "GET", "/metrics", 200,
                      ["requests", "byStatus", "byRoute", "auditEntries",
                       "outboxPending"], token="admin")[0]
if metrics:
    record("byStatus sums to requests",
           [] if sum(metrics["byStatus"].values()) == metrics["requests"]
           else [f"{sum(metrics['byStatus'].values())} != {metrics['requests']}"])
    record("byRoute holds patterns, not concrete ids",
           [] if all(not any(part.isdigit() for part in row["route"].split("/"))
                     for row in metrics["byRoute"])
           else [f"a concrete id leaked into byRoute: {metrics['byRoute']!r}"])
    record("outboxPending is zero after the flush",
           [] if metrics["outboxPending"] == 0 else [f"{metrics['outboxPending']} != 0"])
else:
    record("byStatus sums to requests", ["no metrics body"])
    record("byRoute holds patterns, not concrete ids", ["no metrics body"])
    record("outboxPending is zero after the flush", ["no metrics body"])

check_error("alice reads stats", "GET", "/stats", 403, *FORBID, token="alice")
check("stats", "GET", "/stats", 200, {
    "projects": 6, "tasks": 4, "users": 4, "sessions": 4, "comments": 1,
    "byStatus": {"todo": 3, "in_progress": 1, "done": 0, "archived": 0},
    "avgScore": 30.75, "topProjectName": "Apollo II",
    "auditEntries": 21, "outboxPending": 0}, token="admin")

# ============================================================ K. bulk writes

check_error("an empty bulk", "POST", "/tasks/bulk", 422, *INVALID,
            details=[detail("operations", "operations is out of range")],
            body={"operations": []}, token="alice")
check("a mixed bulk", "POST", "/tasks/bulk", 200, {"results": [
    {"index": 0, "status": 201, "id": 5, "error": None},
    {"index": 1, "status": 200, "id": 4, "error": None},
    {"index": 2, "status": 412, "id": None, "error": "precondition_failed"},
    {"index": 3, "status": 404, "id": None, "error": "not_found"},
    {"index": 4, "status": 422, "id": None, "error": "validation_failed"},
]}, body={"operations": [
    {"op": "create", "projectId": 1, "title": "Bulk", "priority": 2},
    {"op": "status", "id": 4, "status": "in_progress", "version": 1},
    {"op": "delete", "id": 1, "version": 99},
    {"op": "delete", "id": 999, "version": 1},
    {"op": "bogus"},
]}, token="alice")

# ============================================================ L. cascade, end

check("cascade delete project 1", "DELETE", "/projects/1", 200,
      project(1, "Apollo II", 2, 0, 3, deleted=True), etag=3,
      headers=match(2), token="admin")
check_error("the cascade took task 1", "GET", "/tasks/1", 404, *NOTFOUND, token="admin")
check_error("project 1 is invisible", "GET", "/projects/1", 404, *NOTFOUND, token="admin")
check_ids("includeDeleted shows it again", "GET",
          "/projects?includeDeleted=true&limit=100", 6, [1, 2, 3, 4, 5, 6], token="admin")
check("restore project 1", "POST", "/projects/1/restore", 200,
      project(1, "Apollo II", 2, 0, 4), etag=4, headers=match(3), token="admin")
check_error("restoring a live project", "POST", "/projects/1/restore", 409, *CONFLICT,
            headers=match(4), token="admin")
check_ids("the restored project has no live tasks", "GET", "/projects/1/tasks", 0, [],
          token="admin")

check("bob logs out", "POST", "/auth/logout", 204, None, token="bob")
check_error("bob's token is dead", "GET", "/me", 401, *UNAUTH, token="bob")
check_error("an unknown path", "GET", "/nope", 404, *NOTFOUND, token="admin")
check_error("an unknown nested path", "GET", "/projects/2/nope", 404, *NOTFOUND,
            token="admin")

for line in failures:
    print("FAIL " + line)
print(f"{passes}/{counter} cases pass")
sys.exit(1 if failures else 0)
