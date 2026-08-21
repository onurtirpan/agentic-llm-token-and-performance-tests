"""Task Service, large tier — the in-memory state and its repositories."""

from domain import (
    DEFAULT_QUOTA, PROBE_QUOTA, AuditEntry, Comment, OutboxEvent, Project, Session, Task, User,
)

users: dict[int, User] = {}
sessions: dict[str, Session] = {}
projects: dict[int, Project] = {}
tasks: dict[int, Task] = {}
comments: dict[int, Comment] = {}
audit: list[AuditEntry] = []
outbox: list[OutboxEvent] = []
idempotency: dict[tuple[str, str], tuple[int, dict]] = {}
by_status: dict[int, int] = {}
by_route: dict[str, int] = {}

requests = 0
next_project_id = 1
next_task_id = 1
next_comment_id = 1
next_user_id = 5
next_seq = 1


def seed() -> None:
    users.update({
        1: User(1, "admin", "admin-secret", "admin", DEFAULT_QUOTA),
        2: User(2, "alice", "alice-secret", "user", DEFAULT_QUOTA),
        3: User(3, "bob", "bob-secret", "user", DEFAULT_QUOTA),
        4: User(4, "probe", "probe-secret", "user", PROBE_QUOTA),
    })


def take_seq() -> int:
    global next_seq
    value = next_seq
    next_seq += 1
    return value


def record(actor_id: int, action: str, resource: str, resource_id: int) -> None:
    """Append one audit entry and one outbox event for a successful write."""
    audit.append(AuditEntry(take_seq(), actor_id, action, resource, resource_id))
    outbox.append(OutboxEvent(take_seq(), f"{resource}.{action}", resource_id))


def count_request(route: str, status: int) -> None:
    global requests
    requests += 1
    by_route[route] = by_route.get(route, 0) + 1
    by_status[status] = by_status.get(status, 0) + 1


def find_user(user_id: int, include_deleted: bool = False) -> User | None:
    user = users.get(user_id)
    if user is None or (user.deleted and not include_deleted):
        return None
    return user


def find_by_username(username: str) -> User | None:
    return next((u for u in users.values() if u.username == username and not u.deleted), None)


def insert_user(username: str, password: str, role: str, quota: int) -> User:
    global next_user_id
    user = User(next_user_id, username, password, role, quota)
    users[user.id] = user
    next_user_id += 1
    return user


def find_project(project_id: int, include_deleted: bool = False) -> Project | None:
    project = projects.get(project_id)
    if project is None or (project.deleted and not include_deleted):
        return None
    return project


def insert_project(name: str, owner_id: int) -> Project:
    global next_project_id
    project = Project(next_project_id, name, owner_id)
    projects[project.id] = project
    next_project_id += 1
    return project


def find_task(task_id: int, include_deleted: bool = False) -> Task | None:
    task = tasks.get(task_id)
    if task is None or (task.deleted and not include_deleted):
        return None
    return task


def insert_task(project_id: int, title: str, priority: int, assignee_id: int | None,
                internal_note: str) -> Task:
    global next_task_id
    task = Task(next_task_id, project_id, title, priority, "todo", assignee_id, internal_note)
    tasks[task.id] = task
    next_task_id += 1
    return task


def find_comment(comment_id: int) -> Comment | None:
    return comments.get(comment_id)


def insert_comment(task_id: int, author_id: int, body: str) -> Comment:
    global next_comment_id
    comment = Comment(next_comment_id, task_id, author_id, body)
    comments[comment.id] = comment
    next_comment_id += 1
    return comment


def live_tasks_of(project_id: int) -> list[Task]:
    return [t for t in tasks.values() if t.projectId == project_id and not t.deleted]


def task_count(project_id: int) -> int:
    return len(live_tasks_of(project_id))


def outbox_pending() -> int:
    return sum(1 for event in outbox if not event.delivered)
