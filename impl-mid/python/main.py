"""Task Service, mid tier — FastAPI implementation."""

import json
import sys
import time
import uuid
from dataclasses import dataclass

from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse

MAX_TITLE_LENGTH = 80
MAX_NAME_LENGTH = 60
MIN_PRIORITY = 1
MAX_PRIORITY = 5
DEFAULT_LIMIT = 20
MAX_LIMIT = 100
PORT = 8080

STATUS_BONUS = {"todo": 0, "in_progress": 3, "done": 5, "archived": 0}
TRANSITIONS = {
    ("todo", "in_progress"), ("todo", "archived"), ("in_progress", "todo"),
    ("in_progress", "done"), ("done", "archived"),
}
PROJECT_SORTS = ("id", "name", "taskCount")
TASK_SORTS = ("id", "title", "priority", "score", "status")


@dataclass
class User:
    id: int
    username: str
    password: str
    role: str


@dataclass
class Project:
    id: int
    name: str
    ownerId: int


@dataclass
class Task:
    id: int
    projectId: int
    title: str
    priority: int
    status: str
    assigneeId: int | None
    score: int


class AppError(Exception):
    def __init__(self, status: int, code: str, message: str, details: list | None = None):
        self.status = status
        self.code = code
        self.message = message
        self.details = details or []


users: dict[int, User] = {
    1: User(1, "admin", "admin-secret", "admin"),
    2: User(2, "alice", "alice-secret", "user"),
    3: User(3, "bob", "bob-secret", "user"),
}
sessions: dict[str, int] = {}
projects: dict[int, Project] = {}
tasks: dict[int, Task] = {}
next_project_id = 1
next_task_id = 1

app = FastAPI()


def compute_score(priority: int, status: str) -> int:
    base_score = priority * 10
    return base_score + STATUS_BONUS[status]


def task_count(project_id: int) -> int:
    return sum(1 for task in tasks.values() if task.projectId == project_id)


def serialize_project(project: Project) -> dict:
    return {"id": project.id, "name": project.name, "ownerId": project.ownerId,
            "taskCount": task_count(project.id)}


def serialize_task(task: Task) -> dict:
    return {"id": task.id, "projectId": task.projectId, "title": task.title,
            "priority": task.priority, "status": task.status,
            "assigneeId": task.assigneeId, "score": task.score}


def bad_request() -> AppError:
    return AppError(400, "bad_request", "the request is malformed")


def not_found() -> AppError:
    return AppError(404, "not_found", "the resource does not exist")


def forbidden() -> AppError:
    return AppError(403, "forbidden", "you may not access this resource")


def conflict() -> AppError:
    return AppError(409, "conflict", "the resource already exists")


def invalid(details: list) -> AppError:
    details.sort(key=lambda entry: (entry["field"], entry["message"]))
    return AppError(422, "validation_failed", "the request body is not valid", details)


def fail(field: str, message: str) -> dict:
    return {"field": field, "message": message}


async def read_body(request: Request) -> dict:
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


def read_int(body: dict, field: str, default):
    value = body.get(field, default)
    if value is None or (isinstance(value, int) and not isinstance(value, bool)):
        return value
    raise bad_request()


def read_string(body: dict, field: str, errors: list, max_length: int, required: bool) -> str:
    value = body.get(field, "")
    if not isinstance(value, str):
        raise bad_request()
    if value == "":
        if required:
            errors.append(fail(field, f"{field} is required"))
    elif len(value) > max_length:
        errors.append(fail(field, f"{field} is too long"))
    return value


def read_priority(body: dict, errors: list) -> int:
    value = read_int(body, "priority", 0)
    if value is None or value < MIN_PRIORITY or value > MAX_PRIORITY:
        errors.append(fail("priority", "priority is out of range"))
    return value or 0


def read_user_ref(body: dict, field: str, errors: list, default, nullable: bool):
    value = read_int(body, field, default)
    if value is None and nullable:
        return None
    if value not in users:
        errors.append(fail(field, f"{field} is not a known user"))
    return value


def parse_id(raw: str) -> int:
    try:
        return int(raw)
    except ValueError:
        raise bad_request()


def read_page(request: Request, allowed: tuple) -> tuple[int, int, str, str]:
    query = request.query_params
    errors: list = []
    limit, offset = DEFAULT_LIMIT, 0
    sort = query.get("sort", "id")
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


def paginate(rows: list, limit: int, offset: int, sort: str, order: str) -> dict:
    rows.sort(key=lambda row: row["id"])
    rows.sort(key=lambda row: row[sort], reverse=order == "desc")
    return {"items": rows[offset:offset + limit], "total": len(rows),
            "limit": limit, "offset": offset}


def authenticate(request: Request) -> User:
    header = request.headers.get("authorization", "")
    session = sessions.get(header[7:]) if header.startswith("Bearer ") else None
    if session is None:
        raise AppError(401, "unauthorized", "authentication is required")
    request.state.user_id = session
    return users[session]


def require_admin(user: User) -> None:
    if user.role != "admin":
        raise forbidden()


def reachable_project(project_id: int, user: User) -> Project:
    project = projects.get(project_id)
    if project is None:
        raise not_found()
    if user.role != "admin" and project.ownerId != user.id:
        raise forbidden()
    return project


def reachable_task(task_id: int, user: User) -> Task:
    task = tasks.get(task_id)
    if task is None:
        raise not_found()
    reachable_project(task.projectId, user)
    return task


@app.middleware("http")
async def observe(request: Request, call_next):
    request.state.request_id = request.headers.get("x-request-id") or uuid.uuid4().hex[:12]
    request.state.user_id = None
    started = time.perf_counter()
    response = await call_next(request)
    status = response.status_code
    sys.stdout.write(json.dumps({
        "level": "error" if status >= 500 else "warn" if status >= 400 else "info",
        "requestId": request.state.request_id,
        "method": request.method,
        "path": request.url.path,
        "status": status,
        "durationMs": int((time.perf_counter() - started) * 1000),
        "userId": request.state.user_id,
    }) + "\n")
    sys.stdout.flush()
    response.headers["X-Request-Id"] = request.state.request_id
    return response


@app.exception_handler(AppError)
async def on_app_error(request: Request, error: AppError) -> JSONResponse:
    return JSONResponse(status_code=error.status, content={"error": {
        "code": error.code, "message": error.message,
        "requestId": request.state.request_id, "details": error.details,
    }})


@app.exception_handler(404)
async def on_missing_route(request: Request, error) -> JSONResponse:
    return await on_app_error(request, not_found())


@app.get("/health")
def get_health() -> dict:
    return {"status": "ok", "projects": len(projects), "tasks": len(tasks)}


@app.post("/auth/login")
async def login(request: Request) -> JSONResponse:
    body = await read_body(request)
    errors: list = []
    username = read_string(body, "username", errors, MAX_NAME_LENGTH, True)
    password = read_string(body, "password", errors, MAX_NAME_LENGTH, True)
    if errors:
        raise invalid(errors)
    user = next((u for u in users.values()
                 if u.username == username and u.password == password), None)
    if user is None:
        raise AppError(401, "invalid_credentials", "the username or password is wrong")
    token = uuid.uuid4().hex
    sessions[token] = user.id
    return JSONResponse({"token": token, "userId": user.id, "role": user.role})


@app.post("/auth/logout")
def logout(request: Request) -> Response:
    authenticate(request)
    del sessions[request.headers["authorization"][7:]]
    return Response(status_code=204)


@app.get("/me")
def get_me(request: Request) -> dict:
    user = authenticate(request)
    return {"userId": user.id, "username": user.username, "role": user.role}


@app.get("/projects")
def list_projects(request: Request) -> dict:
    user = authenticate(request)
    limit, offset, sort, order = read_page(request, PROJECT_SORTS)
    rows = [serialize_project(p) for p in projects.values()
            if user.role == "admin" or p.ownerId == user.id]
    return paginate(rows, limit, offset, sort, order)


@app.post("/projects")
async def create_project(request: Request) -> JSONResponse:
    global next_project_id
    user = authenticate(request)
    require_admin(user)
    body = await read_body(request)
    errors: list = []
    name = read_string(body, "name", errors, MAX_NAME_LENGTH, True)
    owner_id = read_user_ref(body, "ownerId", errors, user.id, False)
    if errors:
        raise invalid(errors)
    if any(p.ownerId == owner_id and p.name == name for p in projects.values()):
        raise conflict()
    project = Project(next_project_id, name, owner_id)
    projects[next_project_id] = project
    next_project_id += 1
    return JSONResponse(serialize_project(project), status_code=201)


@app.get("/projects/{raw_id}")
def get_project(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    return serialize_project(reachable_project(parse_id(raw_id), user))


@app.patch("/projects/{raw_id}")
async def update_project(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    require_admin(user)
    project = reachable_project(parse_id(raw_id), user)
    body = await read_body(request)
    if "name" not in body:
        return serialize_project(project)
    errors: list = []
    name = read_string(body, "name", errors, MAX_NAME_LENGTH, True)
    if errors:
        raise invalid(errors)
    if any(p.ownerId == project.ownerId and p.name == name and p.id != project.id
           for p in projects.values()):
        raise conflict()
    project.name = name
    return serialize_project(project)


@app.delete("/projects/{raw_id}")
def delete_project(request: Request, raw_id: str) -> Response:
    user = authenticate(request)
    require_admin(user)
    project = reachable_project(parse_id(raw_id), user)
    for task_id in [t.id for t in tasks.values() if t.projectId == project.id]:
        del tasks[task_id]
    del projects[project.id]
    return Response(status_code=204)


@app.get("/projects/{raw_id}/tasks")
def list_tasks(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    project = reachable_project(parse_id(raw_id), user)
    limit, offset, sort, order = read_page(request, TASK_SORTS)
    rows = [serialize_task(t) for t in tasks.values() if t.projectId == project.id]
    return paginate(rows, limit, offset, sort, order)


@app.post("/projects/{raw_id}/tasks")
async def create_task(request: Request, raw_id: str) -> JSONResponse:
    global next_task_id
    user = authenticate(request)
    project = reachable_project(parse_id(raw_id), user)
    body = await read_body(request)
    errors: list = []
    title = read_string(body, "title", errors, MAX_TITLE_LENGTH, True)
    priority = read_priority(body, errors)
    assignee_id = read_user_ref(body, "assigneeId", errors, None, True)
    if errors:
        raise invalid(errors)
    task = Task(next_task_id, project.id, title, priority, "todo", assignee_id,
                compute_score(priority, "todo"))
    tasks[next_task_id] = task
    next_task_id += 1
    return JSONResponse(serialize_task(task), status_code=201)


@app.get("/tasks/{raw_id}")
def get_task(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    return serialize_task(reachable_task(parse_id(raw_id), user))


@app.put("/tasks/{raw_id}")
async def replace_task(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    task = reachable_task(parse_id(raw_id), user)
    body = await read_body(request)
    errors: list = []
    title = read_string(body, "title", errors, MAX_TITLE_LENGTH, True)
    priority = read_priority(body, errors)
    assignee_id = read_user_ref(body, "assigneeId", errors, None, True)
    if errors:
        raise invalid(errors)
    task.title = title
    task.priority = priority
    task.assigneeId = assignee_id
    task.score = compute_score(priority, task.status)
    return serialize_task(task)


@app.patch("/tasks/{raw_id}/status")
async def update_status(request: Request, raw_id: str) -> dict:
    user = authenticate(request)
    task = reachable_task(parse_id(raw_id), user)
    body = await read_body(request)
    status = body.get("status")
    if not isinstance(status, str) or status not in STATUS_BONUS:
        raise invalid([fail("status", "status is not valid")])
    if (task.status, status) not in TRANSITIONS:
        raise AppError(409, "invalid_transition", "the status change is not allowed")
    task.status = status
    task.score = compute_score(task.priority, status)
    return serialize_task(task)


@app.delete("/tasks/{raw_id}")
def delete_task(request: Request, raw_id: str) -> Response:
    user = authenticate(request)
    task = reachable_task(parse_id(raw_id), user)
    del tasks[task.id]
    return Response(status_code=204)


@app.get("/stats")
def get_stats(request: Request) -> dict:
    user = authenticate(request)
    require_admin(user)
    by_status = {name: 0 for name in STATUS_BONUS}
    for task in tasks.values():
        by_status[task.status] += 1
    total = len(tasks)
    avg_score = round(sum(t.score for t in tasks.values()) / total, 2) if total else 0.0
    best = None
    for project in projects.values():
        if best is None or task_count(project.id) > task_count(best.id):
            best = project
    return {"projects": len(projects), "tasks": total, "users": len(users),
            "sessions": len(sessions), "byStatus": by_status, "avgScore": avg_score,
            "topProjectName": best.name if best else None}


@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
def fallback(path: str) -> JSONResponse:
    raise not_found()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=PORT, log_config=None, access_log=False)
