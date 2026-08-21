"""Task Service, large tier — domain types, constants and pure rules."""

from dataclasses import dataclass

MAX_TITLE_LENGTH = 80
MAX_NAME_LENGTH = 60
MAX_COMMENT_LENGTH = 200
MAX_BULK_ITEMS = 20
MIN_PRIORITY = 1
MAX_PRIORITY = 5
DEFAULT_LIMIT = 20
MAX_LIMIT = 100
DEFAULT_QUOTA = 10000
PROBE_QUOTA = 5
PORT = 8080

ROLES = ("admin", "user")
STATUSES = ("todo", "in_progress", "done", "archived")
STATUS_BONUS = {"todo": 0, "in_progress": 3, "done": 5, "archived": 0}
TRANSITIONS = {
    ("todo", "in_progress"), ("todo", "archived"), ("in_progress", "todo"),
    ("in_progress", "done"), ("done", "archived"),
}
ACTIONS = ("create", "update", "delete", "restore")
PROJECT_SORTS = ("id", "name", "taskCount")
TASK_SORTS = ("id", "title", "priority", "score", "status")
USER_SORTS = ("id", "username", "role")
COMMENT_SORTS = ("id", "authorId")
SEQ_SORTS = ("seq",)
GROUP_BYS = ("assignee", "status", "project")


@dataclass
class User:
    id: int
    username: str
    password: str
    role: str
    quota: int
    version: int = 1
    deleted: bool = False


@dataclass
class Session:
    token: str
    userId: int
    used: int = 0


@dataclass
class Project:
    id: int
    name: str
    ownerId: int
    version: int = 1
    deleted: bool = False


@dataclass
class Task:
    id: int
    projectId: int
    title: str
    priority: int
    status: str
    assigneeId: int | None
    internalNote: str = ""
    version: int = 1
    deleted: bool = False


@dataclass
class Comment:
    id: int
    taskId: int
    authorId: int
    body: str


@dataclass
class AuditEntry:
    seq: int
    actorId: int
    action: str
    resource: str
    resourceId: int


@dataclass
class OutboxEvent:
    seq: int
    name: str
    resourceId: int
    delivered: bool = False


class AppError(Exception):
    """Every failure path raises this. The api layer turns it into the envelope."""

    def __init__(self, status: int, code: str, message: str, details: list | None = None):
        self.status = status
        self.code = code
        self.message = message
        self.details = details or []


def bad_request() -> AppError:
    return AppError(400, "bad_request", "the request is malformed")


def unauthorized() -> AppError:
    return AppError(401, "unauthorized", "authentication is required")


def invalid_credentials() -> AppError:
    return AppError(401, "invalid_credentials", "the username or password is wrong")


def forbidden() -> AppError:
    return AppError(403, "forbidden", "you may not access this resource")


def not_found() -> AppError:
    return AppError(404, "not_found", "the resource does not exist")


def conflict() -> AppError:
    return AppError(409, "conflict", "the resource already exists")


def invalid_transition() -> AppError:
    return AppError(409, "invalid_transition", "the status change is not allowed")


def precondition_failed() -> AppError:
    return AppError(412, "precondition_failed", "the resource has changed")


def precondition_required() -> AppError:
    return AppError(428, "precondition_required", "the If-Match header is required")


def quota_exceeded() -> AppError:
    return AppError(429, "quota_exceeded", "the request quota is exhausted")


def invalid(details: list) -> AppError:
    details.sort(key=lambda entry: (entry["field"], entry["message"]))
    return AppError(422, "validation_failed", "the request body is not valid", details)


def fail(field: str, message: str) -> dict:
    return {"field": field, "message": message}


def compute_score(priority: int, status: str) -> int:
    base_score = priority * 10
    return base_score + STATUS_BONUS[status]


def check_string(value: str, field_name: str, max_length: int, errors: list) -> None:
    if value == "":
        errors.append(fail(field_name, f"{field_name} is required"))
    elif len(value) > max_length:
        errors.append(fail(field_name, f"{field_name} is too long"))


def check_priority(value: int | None, errors: list) -> None:
    if value is None or value < MIN_PRIORITY or value > MAX_PRIORITY:
        errors.append(fail("priority", "priority is out of range"))


def check_status(value, errors: list) -> None:
    if not isinstance(value, str) or value not in STATUSES:
        errors.append(fail("status", "status is not valid"))


def check_role(value, errors: list) -> None:
    if not isinstance(value, str) or value not in ROLES:
        errors.append(fail("role", "role is not valid"))


def check_quota(value, errors: list) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        errors.append(fail("quota", "quota is out of range"))
