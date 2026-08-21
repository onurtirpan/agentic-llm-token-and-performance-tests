"""Task Service — FastAPI implementation."""

from fastapi import FastAPI, Request, Response
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel

MAX_TITLE_LENGTH = 80
MIN_PRIORITY = 1
MAX_PRIORITY = 5
PORT = 8080


class Task(BaseModel):
    id: int
    title: str
    priority: int
    done: bool
    score: int


class TaskInput(BaseModel):
    title: str = ""
    priority: int = 0
    done: bool = False


tasks: dict[int, Task] = {}
next_id = 1

app = FastAPI()


def compute_score(priority: int, done: bool) -> int:
    base_score = priority * 10
    return base_score if done else base_score + 5


def validate(title: str, priority: int) -> str | None:
    if not title:
        return "title is required"
    if len(title) > MAX_TITLE_LENGTH:
        return "title is too long"
    if priority < MIN_PRIORITY or priority > MAX_PRIORITY:
        return "priority is out of range"
    return None


def fail(status: int, message: str) -> JSONResponse:
    return JSONResponse(status_code=status, content={"error": message})


@app.exception_handler(RequestValidationError)
async def on_bad_request(request: Request, error: RequestValidationError) -> JSONResponse:
    where = error.errors()[0]["loc"][0]
    return fail(400, "invalid id" if where == "path" else "invalid json")


@app.get("/health")
def get_health() -> dict:
    return {"status": "ok", "count": len(tasks)}


@app.get("/tasks")
def list_tasks(done: str | None = None):
    if done is not None and done not in ("true", "false"):
        return fail(400, "done must be true or false")
    selected = [t for t in tasks.values() if done is None or t.done == (done == "true")]
    selected.sort(key=lambda t: (-t.score, t.id))
    return {"tasks": selected, "total": len(selected)}


@app.get("/tasks/{task_id}")
def get_task(task_id: int):
    task = tasks.get(task_id)
    if task is None:
        return fail(404, "task not found")
    return task


@app.post("/tasks", status_code=201)
def create_task(body: TaskInput):
    global next_id
    error = validate(body.title, body.priority)
    if error:
        return fail(400, error)
    task = Task(id=next_id, title=body.title, priority=body.priority, done=False,
                score=compute_score(body.priority, False))
    tasks[next_id] = task
    next_id += 1
    return task


@app.put("/tasks/{task_id}")
def update_task(task_id: int, body: TaskInput):
    task = tasks.get(task_id)
    if task is None:
        return fail(404, "task not found")
    error = validate(body.title, body.priority)
    if error:
        return fail(400, error)
    task.title = body.title
    task.priority = body.priority
    task.done = body.done
    task.score = compute_score(body.priority, body.done)
    return task


@app.delete("/tasks/{task_id}")
def delete_task(task_id: int):
    if task_id not in tasks:
        return fail(404, "task not found")
    del tasks[task_id]
    return Response(status_code=204)


@app.get("/stats")
def get_stats() -> dict:
    total = len(tasks)
    done_count = sum(1 for t in tasks.values() if t.done)
    avg_score = round(sum(t.score for t in tasks.values()) / total, 2) if total else 0.0
    best: Task | None = None
    for task in tasks.values():
        if not task.done and (best is None or task.priority > best.priority):
            best = task
    return {"total": total, "doneCount": done_count, "openCount": total - done_count,
            "avgScore": avg_score, "topOpenTitle": best.title if best else None}


@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH"])
def fallback(path: str) -> JSONResponse:
    return fail(404, "not found")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=PORT, log_level="warning")
