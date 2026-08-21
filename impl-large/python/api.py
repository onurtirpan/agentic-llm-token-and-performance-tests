"""Task Service, large tier — HTTP routing, middleware and the entry point."""

import json
import sys
import time
import uuid

from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse

import service
import store
from domain import (
    COMMENT_SORTS, DEFAULT_LIMIT, DEFAULT_QUOTA, GROUP_BYS, MAX_LIMIT, PORT, PROJECT_SORTS,
    SEQ_SORTS, STATUSES, TASK_SORTS, USER_SORTS, AppError, bad_request, fail, invalid,
    not_found,
)

app = FastAPI()
store.seed()


# ------------------------------------------------------------------- middleware


@app.middleware("http")
async def observe(request: Request, call_next):
    request.state.request_id = request.headers.get("x-request-id") or uuid.uuid4().hex[:12]
    request.state.user_id = None
    request.state.quota_remaining = None
    request.state.replayed = False
    before = len(store.audit)
    started = time.perf_counter()
    response = await call_next(request)
    route = request.scope.get("route")
    label = f"{request.method} {route.path.replace('raw_id', 'id')}" if route else "unmatched"
    store.count_request(label, response.status_code)
    sys.stdout.write(json.dumps({
        "level": "error" if response.status_code >= 500
        else "warn" if response.status_code >= 400 else "info",
        "requestId": request.state.request_id,
        "method": request.method,
        "path": request.url.path,
        "status": response.status_code,
        "durationMs": int((time.perf_counter() - started) * 1000),
        "userId": request.state.user_id,
        "quotaRemaining": request.state.quota_remaining,
        "auditSeq": len(store.audit) - before,
    }) + "\n")
    sys.stdout.flush()
    response.headers["X-Request-Id"] = request.state.request_id
    if request.state.quota_remaining is not None:
        response.headers["X-Quota-Remaining"] = str(request.state.quota_remaining)
    if request.state.replayed:
        response.headers["Idempotency-Replayed"] = "true"
    return response


def envelope(request: Request, error: AppError) -> dict:
    return {"error": {"code": error.code, "message": error.message,
                      "requestId": request.state.request_id, "details": error.details}}


@app.exception_handler(AppError)
async def on_app_error(request: Request, error: AppError) -> JSONResponse:
    return JSONResponse(envelope(request, error), status_code=error.status)


@app.exception_handler(404)
async def on_missing_route(request: Request, error) -> JSONResponse:
    return await on_app_error(request, not_found())


# ---------------------------------------------------------------------- helpers


def begin(request: Request, admin: bool = False):
    """Authenticate, charge the quota, then check the role. This order is fixed."""
    user, session = service.authenticate(request.headers.get("authorization", ""))
    request.state.user_id = user.id
    request.state.quota_remaining = service.charge_quota(user, session)
    if admin:
        service.require_admin(user)
    return user, session


async def body_of(request: Request) -> dict:
    raw = await request.body()
    if raw.strip() == b"":
        return {}
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        raise bad_request()
    if not isinstance(parsed, dict):
        raise bad_request()
    return parsed


def whole(body: dict, field: str, default):
    value = body.get(field, default)
    if value is None or (isinstance(value, int) and not isinstance(value, bool)):
        return value
    raise bad_request()


def text(body: dict, field: str, default: str = "") -> str:
    value = body.get(field, default)
    if not isinstance(value, str):
        raise bad_request()
    return value


def parse_id(raw: str) -> int:
    try:
        return int(raw)
    except ValueError:
        raise bad_request()


def read_page(request: Request, allowed: tuple):
    query = request.query_params
    errors: list = []
    limit, offset = DEFAULT_LIMIT, 0
    sort = query.get("sort", allowed[0])
    order = query.get("order", "asc")
    if "limit" in query:
        limit = int(query["limit"]) if query["limit"].lstrip("-").isdigit() else -1
        if limit < 1 or limit > MAX_LIMIT:
            errors.append(fail("limit", "limit is out of range"))
    if "offset" in query:
        offset = int(query["offset"]) if query["offset"].lstrip("-").isdigit() else -1
        if offset < 0:
            errors.append(fail("offset", "offset is out of range"))
    if sort not in allowed:
        errors.append(fail("sort", "sort is not a valid field"))
    if order not in ("asc", "desc"):
        errors.append(fail("order", "order must be asc or desc"))
    if errors:
        raise invalid(errors)
    return limit, offset, sort, order


def tagged(body: dict, version: int) -> JSONResponse:
    return JSONResponse(body, headers={"ETag": str(version)})


def if_match(request: Request, version: int) -> None:
    service.check_if_match(request.headers.get("if-match"), version)


def responded(status: int, body: dict) -> JSONResponse:
    """A single-resource body carries its version, so the ETag comes for free."""
    version = body.get("version") if isinstance(body, dict) else None
    headers = {"ETag": str(version)} if version is not None else None
    return JSONResponse(body, status_code=status, headers=headers)


def idempotent(request: Request, session, produce):
    """Run produce once per Idempotency-Key, then replay the recorded outcome."""
    key = request.headers.get("idempotency-key")
    if key is None:
        return responded(*produce())
    slot = (session.token, key)
    if slot in store.idempotency:
        request.state.replayed = True
        return responded(*store.idempotency[slot])
    try:
        status, body = produce()
    except AppError as error:
        store.idempotency[slot] = (error.status, envelope(request, error))
        raise
    store.idempotency[slot] = (status, body)
    return responded(status, body)


# ------------------------------------------------------------------ health, auth


@app.get("/health")
def get_health() -> dict:
    return {"status": "ok",
            "projects": sum(1 for p in store.projects.values() if not p.deleted),
            "tasks": sum(1 for t in store.tasks.values() if not t.deleted),
            "comments": len(store.comments)}


@app.post("/auth/login")
async def login(request: Request) -> JSONResponse:
    body = await body_of(request)
    errors: list = []
    username = text(body, "username")
    password = text(body, "password")
    if username == "":
        errors.append(fail("username", "username is required"))
    if password == "":
        errors.append(fail("password", "password is required"))
    if errors:
        raise invalid(errors)
    token = uuid.uuid4().hex
    user = service.login(username, password, token)
    return JSONResponse({"token": token, "userId": user.id, "role": user.role})


@app.post("/auth/logout")
def logout(request: Request) -> Response:
    user, session = begin(request)
    del store.sessions[session.token]
    return Response(status_code=204)


@app.get("/me")
def get_me(request: Request) -> dict:
    user, session = begin(request)
    return {"userId": user.id, "username": user.username, "role": user.role}


# ------------------------------------------------------------------------ users


@app.get("/users")
def list_users(request: Request) -> dict:
    begin(request, admin=True)
    limit, offset, sort, order = read_page(request, USER_SORTS)
    rows = [service.serialize_user(u) for u in store.users.values() if not u.deleted]
    return service.paginate(rows, limit, offset, sort, order)


@app.post("/users")
async def create_user(request: Request) -> JSONResponse:
    actor, session = begin(request, admin=True)
    body = await body_of(request)

    def produce():
        user = service.create_user(actor, text(body, "username"), text(body, "password"),
                                  body.get("role", "user"), body.get("quota", DEFAULT_QUOTA))
        return 201, service.serialize_user(user)

    return idempotent(request, session, produce)


@app.get("/users/{raw_id}")
def get_user(request: Request, raw_id: str) -> JSONResponse:
    begin(request, admin=True)
    user = store.find_user(parse_id(raw_id))
    if user is None:
        raise not_found()
    return tagged(service.serialize_user(user), user.version)


@app.patch("/users/{raw_id}")
async def update_user(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request, admin=True)
    user = store.find_user(parse_id(raw_id))
    if user is None:
        raise not_found()
    if_match(request, user.version)
    service.update_user(actor, user, await body_of(request))
    return tagged(service.serialize_user(user), user.version)


@app.delete("/users/{raw_id}")
def delete_user(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request, admin=True)
    user = store.find_user(parse_id(raw_id))
    if user is None:
        raise not_found()
    if_match(request, user.version)
    service.delete_user(actor, user)
    return tagged(service.serialize_user(user), user.version)


# --------------------------------------------------------------------- projects


@app.get("/projects")
def list_projects(request: Request) -> dict:
    user, session = begin(request)
    include = service.check_include_deleted(request.query_params.get("includeDeleted"), user)
    limit, offset, sort, order = read_page(request, PROJECT_SORTS)
    rows = [service.serialize_project(p) for p in service.visible_projects(user, include)]
    return service.paginate(rows, limit, offset, sort, order)


@app.post("/projects")
async def create_project(request: Request) -> JSONResponse:
    actor, session = begin(request, admin=True)
    body = await body_of(request)

    def produce():
        project = service.create_project(actor, text(body, "name"),
                                         whole(body, "ownerId", actor.id))
        return 201, service.serialize_project(project)

    return idempotent(request, session, produce)


@app.get("/projects/{raw_id}")
def get_project(request: Request, raw_id: str) -> JSONResponse:
    user, session = begin(request)
    project = service.reachable_project(parse_id(raw_id), user)
    return tagged(service.serialize_project(project), project.version)


@app.patch("/projects/{raw_id}")
async def update_project(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request, admin=True)
    project = service.reachable_project(parse_id(raw_id), actor)
    if_match(request, project.version)
    body = await body_of(request)
    if "name" in body:
        service.rename_project(actor, project, text(body, "name"))
    return tagged(service.serialize_project(project), project.version)


@app.delete("/projects/{raw_id}")
def delete_project(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request, admin=True)
    project = service.reachable_project(parse_id(raw_id), actor)
    if_match(request, project.version)
    service.delete_project(actor, project)
    return tagged(service.serialize_project(project), project.version)


@app.post("/projects/{raw_id}/restore")
def restore_project(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request, admin=True)
    project = service.reachable_project(parse_id(raw_id), actor, True)
    if_match(request, project.version)
    service.restore_project(actor, project)
    return tagged(service.serialize_project(project), project.version)


# ------------------------------------------------------------------------ tasks


def task_filters(request: Request, rows: list) -> list:
    query = request.query_params
    errors: list = []
    status = query.get("status")
    assignee = query.get("assigneeId")
    if status is not None and status not in STATUSES:
        errors.append(fail("status", "status is not valid"))
    if assignee is not None and not assignee.lstrip("-").isdigit():
        errors.append(fail("assigneeId", "assigneeId is not a known user"))
    if errors:
        raise invalid(errors)
    if status is not None:
        rows = [task for task in rows if task.status == status]
    if assignee is not None:
        rows = [task for task in rows if task.assigneeId == int(assignee)]
    return rows


@app.get("/tasks")
def list_all_tasks(request: Request) -> dict:
    user, session = begin(request)
    include = service.check_include_deleted(request.query_params.get("includeDeleted"), user)
    limit, offset, sort, order = read_page(request, TASK_SORTS)
    rows = task_filters(request, service.visible_tasks(user, include))
    return service.paginate([service.serialize_task(t, user.role) for t in rows],
                            limit, offset, sort, order)


@app.get("/projects/{raw_id}/tasks")
def list_tasks(request: Request, raw_id: str) -> dict:
    user, session = begin(request)
    project = service.reachable_project(parse_id(raw_id), user)
    limit, offset, sort, order = read_page(request, TASK_SORTS)
    rows = [t for t in store.tasks.values() if t.projectId == project.id and not t.deleted]
    return service.paginate([service.serialize_task(t, user.role) for t in rows],
                            limit, offset, sort, order)


@app.post("/projects/{raw_id}/tasks")
async def create_task(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    project = service.reachable_project(parse_id(raw_id), actor)
    body = await body_of(request)

    def produce():
        errors: list = []
        note = service.read_note(actor, body, errors, "")
        task = service.create_task(actor, project, text(body, "title"),
                                   whole(body, "priority", 0),
                                   whole(body, "assigneeId", None), note, errors)
        return 201, service.serialize_task(task, actor.role)

    return idempotent(request, session, produce)


@app.get("/tasks/{raw_id}")
def get_task(request: Request, raw_id: str) -> JSONResponse:
    user, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), user)
    return tagged(service.serialize_task(task, user.role), task.version)


@app.put("/tasks/{raw_id}")
async def replace_task(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), actor)
    if_match(request, task.version)
    body = await body_of(request)
    errors: list = []
    note = service.read_note(actor, body, errors, task.internalNote)
    service.replace_task(actor, task, text(body, "title"), whole(body, "priority", 0),
                         whole(body, "assigneeId", None), note, errors)
    return tagged(service.serialize_task(task, actor.role), task.version)


@app.patch("/tasks/{raw_id}/status")
async def update_status(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), actor)
    if_match(request, task.version)
    body = await body_of(request)
    service.move_status(actor, task, body.get("status"))
    return tagged(service.serialize_task(task, actor.role), task.version)


@app.delete("/tasks/{raw_id}")
def delete_task(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), actor)
    if_match(request, task.version)
    service.delete_task(actor, task)
    return tagged(service.serialize_task(task, actor.role), task.version)


@app.post("/tasks/{raw_id}/restore")
def restore_task(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), actor, True)
    if_match(request, task.version)
    service.restore_task(actor, task)
    return tagged(service.serialize_task(task, actor.role), task.version)


@app.post("/tasks/bulk")
async def bulk_tasks(request: Request) -> dict:
    actor, session = begin(request)
    body = await body_of(request)
    operations = body.get("operations")
    service.check_bulk_size(operations)
    results = []
    for index, item in enumerate(operations):
        try:
            if not isinstance(item, dict):
                raise bad_request()
            results.append({"index": index, **apply_bulk(actor, item)})
        except AppError as error:
            results.append({"index": index, "status": error.status,
                            "id": None, "error": error.code})
    return {"results": results}


def apply_bulk(actor, item: dict) -> dict:
    operation = item.get("op")
    if operation == "create":
        project = service.reachable_project(whole(item, "projectId", 0), actor)
        task = service.create_task(actor, project, text(item, "title"),
                                   whole(item, "priority", 0), None, "", [])
        return {"status": 201, "id": task.id, "error": None}
    if operation == "status":
        task = service.reachable_task(whole(item, "id", 0), actor)
        service.check_if_match(str(item.get("version")), task.version)
        service.move_status(actor, task, item.get("status"))
        return {"status": 200, "id": task.id, "error": None}
    if operation == "delete":
        task = service.reachable_task(whole(item, "id", 0), actor)
        service.check_if_match(str(item.get("version")), task.version)
        service.delete_task(actor, task)
        return {"status": 200, "id": task.id, "error": None}
    raise invalid([fail("op", "op is not valid")])


# --------------------------------------------------------------------- comments


@app.get("/tasks/{raw_id}/comments")
def list_comments(request: Request, raw_id: str) -> dict:
    user, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), user)
    limit, offset, sort, order = read_page(request, COMMENT_SORTS)
    rows = [service.serialize_comment(c) for c in store.comments.values()
            if c.taskId == task.id]
    return service.paginate(rows, limit, offset, sort, order)


@app.post("/tasks/{raw_id}/comments")
async def create_comment(request: Request, raw_id: str) -> JSONResponse:
    actor, session = begin(request)
    task = service.reachable_task(parse_id(raw_id), actor)
    body = await body_of(request)

    def produce():
        comment = service.create_comment(actor, task, text(body, "body"))
        return 201, service.serialize_comment(comment)

    return idempotent(request, session, produce)


@app.delete("/comments/{raw_id}")
def delete_comment(request: Request, raw_id: str) -> Response:
    actor, session = begin(request)
    comment = store.find_comment(parse_id(raw_id))
    if comment is None:
        raise not_found()
    service.reachable_task(comment.taskId, actor, True)
    service.remove_comment(actor, comment)
    return Response(status_code=204)


# --------------------------------------------------- search, reports, telemetry


@app.get("/search")
def search(request: Request) -> dict:
    user, session = begin(request)
    query = request.query_params.get("q", "")
    if query == "":
        raise invalid([fail("q", "q is required")])
    return service.search(user, query)


@app.get("/reports/workload")
def workload(request: Request) -> dict:
    user, session = begin(request)
    group_by = request.query_params.get("groupBy", "status")
    if group_by not in GROUP_BYS:
        raise invalid([fail("groupBy", "groupBy is not valid")])
    return service.workload(user, group_by)


@app.get("/audit")
def list_audit(request: Request) -> dict:
    begin(request, admin=True)
    limit, offset, sort, order = read_page(request, SEQ_SORTS)
    query = request.query_params
    rows = [service.serialize_audit(entry) for entry in store.audit
            if (query.get("actorId") is None or str(entry.actorId) == query["actorId"])
            and (query.get("resource") is None or entry.resource == query["resource"])
            and (query.get("action") is None or entry.action == query["action"])]
    return service.paginate(rows, limit, offset, sort, order)


@app.get("/outbox")
def list_outbox(request: Request) -> dict:
    begin(request, admin=True)
    limit, offset, sort, order = read_page(request, SEQ_SORTS)
    wanted = request.query_params.get("delivered")
    rows = [service.serialize_outbox(event) for event in store.outbox
            if wanted is None or event.delivered == (wanted == "true")]
    return service.paginate(rows, limit, offset, sort, order)


@app.post("/outbox/flush")
def flush_outbox(request: Request) -> dict:
    begin(request, admin=True)
    return {"flushed": service.flush_outbox()}


@app.get("/metrics")
def get_metrics(request: Request) -> dict:
    begin(request, admin=True)
    return service.metrics()


@app.get("/stats")
def get_stats(request: Request) -> dict:
    begin(request, admin=True)
    return service.stats()


@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
def fallback(path: str) -> JSONResponse:
    raise not_found()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=PORT, log_config=None, access_log=False)
