"""Task Service, large tier — business rules, authorization and audit emission."""

import store
from domain import (
    MAX_BULK_ITEMS, MAX_COMMENT_LENGTH, MAX_NAME_LENGTH, MAX_TITLE_LENGTH, STATUSES,
    TRANSITIONS, Comment, Project, Task, User, bad_request, check_priority, check_quota,
    check_role, check_status, check_string, compute_score, conflict, fail, forbidden, invalid,
    invalid_credentials, invalid_transition, not_found, precondition_failed,
    precondition_required, quota_exceeded, unauthorized,
)

# ------------------------------------------------------------------ serializers


def serialize_user(user: User) -> dict:
    return {"id": user.id, "username": user.username, "role": user.role,
            "quota": user.quota, "version": user.version, "deleted": user.deleted}


def serialize_project(project: Project) -> dict:
    return {"id": project.id, "name": project.name, "ownerId": project.ownerId,
            "taskCount": store.task_count(project.id), "version": project.version,
            "deleted": project.deleted}


def serialize_task(task: Task, role: str) -> dict:
    body = {"id": task.id, "projectId": task.projectId, "title": task.title,
            "priority": task.priority, "status": task.status, "assigneeId": task.assigneeId}
    if role == "admin":
        body["internalNote"] = task.internalNote
    body["version"] = task.version
    body["deleted"] = task.deleted
    body["score"] = compute_score(task.priority, task.status)
    return body


def serialize_comment(comment: Comment) -> dict:
    return {"id": comment.id, "taskId": comment.taskId,
            "authorId": comment.authorId, "body": comment.body}


def serialize_audit(entry) -> dict:
    return {"seq": entry.seq, "actorId": entry.actorId, "action": entry.action,
            "resource": entry.resource, "resourceId": entry.resourceId}


def serialize_outbox(event) -> dict:
    return {"seq": event.seq, "name": event.name, "resourceId": event.resourceId,
            "delivered": event.delivered}


# ----------------------------------------------------------------- access rules


def authenticate(header: str) -> tuple[User, object]:
    token = header[7:] if header.startswith("Bearer ") else ""
    session = store.sessions.get(token)
    if session is None:
        raise unauthorized()
    user = store.find_user(session.userId)
    if user is None:
        raise unauthorized()
    return user, session


def charge_quota(user: User, session) -> int:
    if session.used >= user.quota:
        raise quota_exceeded()
    session.used += 1
    return max(user.quota - session.used, 0)


def require_admin(user: User) -> None:
    if user.role != "admin":
        raise forbidden()


def reachable_project(project_id: int, user: User, include_deleted: bool = False) -> Project:
    project = store.find_project(project_id, include_deleted)
    if project is None:
        raise not_found()
    if user.role != "admin" and project.ownerId != user.id:
        raise forbidden()
    return project


def reachable_task(task_id: int, user: User, include_deleted: bool = False) -> Task:
    task = store.find_task(task_id, include_deleted)
    if task is None:
        raise not_found()
    reachable_project(task.projectId, user, True)
    return task


def check_if_match(header: str | None, version: int) -> None:
    if header is None or header == "":
        raise precondition_required()
    if header != str(version):
        raise precondition_failed()


def check_include_deleted(raw: str | None, user: User) -> bool:
    if raw is None:
        return False
    if user.role != "admin":
        raise forbidden()
    return raw == "true"


# ------------------------------------------------------------------- pagination


def paginate(rows: list, limit: int, offset: int, sort: str, order: str) -> dict:
    """Sort by the tiebreak first, then stably by the requested field."""
    tiebreak = "seq" if rows and "seq" in rows[0] else "id"
    rows.sort(key=lambda row: row[tiebreak])
    rows.sort(key=lambda row: row[sort], reverse=order == "desc")
    return {"items": rows[offset:offset + limit], "total": len(rows),
            "limit": limit, "offset": offset}


# ------------------------------------------------------------------------- auth


def login(username: str, password: str, token: str):
    user = store.find_by_username(username)
    if user is None or user.password != password:
        raise invalid_credentials()
    store.sessions[token] = store.Session(token, user.id)
    return user


# --------------------------------------------------------------------- projects


def create_project(actor: User, name: str, owner_id) -> Project:
    errors: list = []
    check_string(name, "name", MAX_NAME_LENGTH, errors)
    if store.find_user(owner_id) is None:
        errors.append(fail("ownerId", "ownerId is not a known user"))
    if errors:
        raise invalid(errors)
    if any(p.ownerId == owner_id and p.name == name and not p.deleted
           for p in store.projects.values()):
        raise conflict()
    project = store.insert_project(name, owner_id)
    store.record(actor.id, "create", "project", project.id)
    return project


def rename_project(actor: User, project: Project, name: str) -> Project:
    errors: list = []
    check_string(name, "name", MAX_NAME_LENGTH, errors)
    if errors:
        raise invalid(errors)
    if any(p.ownerId == project.ownerId and p.name == name and p.id != project.id
           and not p.deleted for p in store.projects.values()):
        raise conflict()
    project.name = name
    project.version += 1
    store.record(actor.id, "update", "project", project.id)
    return project


def delete_project(actor: User, project: Project) -> Project:
    project.deleted = True
    project.version += 1
    store.record(actor.id, "delete", "project", project.id)
    for task in store.live_tasks_of(project.id):
        task.deleted = True
        task.version += 1
        store.record(actor.id, "delete", "task", task.id)
    return project


def restore_project(actor: User, project: Project) -> Project:
    if not project.deleted:
        raise conflict()
    project.deleted = False
    project.version += 1
    store.record(actor.id, "restore", "project", project.id)
    return project


# ------------------------------------------------------------------------ tasks


def read_note(actor: User, body: dict, errors: list, current: str) -> str:
    if "internalNote" not in body:
        return current
    if actor.role != "admin":
        raise forbidden()
    note = body["internalNote"]
    if not isinstance(note, str):
        raise bad_request()
    if len(note) > MAX_TITLE_LENGTH:
        errors.append(fail("internalNote", "internalNote is too long"))
    return note


def create_task(actor: User, project: Project, title: str, priority, assignee_id,
                note: str, errors: list) -> Task:
    check_string(title, "title", MAX_TITLE_LENGTH, errors)
    check_priority(priority, errors)
    if assignee_id is not None and store.find_user(assignee_id) is None:
        errors.append(fail("assigneeId", "assigneeId is not a known user"))
    if errors:
        raise invalid(errors)
    task = store.insert_task(project.id, title, priority, assignee_id, note)
    store.record(actor.id, "create", "task", task.id)
    return task


def replace_task(actor: User, task: Task, title: str, priority, assignee_id,
                 note: str, errors: list) -> Task:
    check_string(title, "title", MAX_TITLE_LENGTH, errors)
    check_priority(priority, errors)
    if assignee_id is not None and store.find_user(assignee_id) is None:
        errors.append(fail("assigneeId", "assigneeId is not a known user"))
    if errors:
        raise invalid(errors)
    task.title = title
    task.priority = priority
    task.assigneeId = assignee_id
    task.internalNote = note
    task.version += 1
    store.record(actor.id, "update", "task", task.id)
    return task


def move_status(actor: User, task: Task, status) -> Task:
    errors: list = []
    check_status(status, errors)
    if errors:
        raise invalid(errors)
    if (task.status, status) not in TRANSITIONS:
        raise invalid_transition()
    task.status = status
    task.version += 1
    store.record(actor.id, "update", "task", task.id)
    return task


def delete_task(actor: User, task: Task) -> Task:
    task.deleted = True
    task.version += 1
    store.record(actor.id, "delete", "task", task.id)
    return task


def restore_task(actor: User, task: Task) -> Task:
    if not task.deleted:
        raise conflict()
    task.deleted = False
    task.version += 1
    store.record(actor.id, "restore", "task", task.id)
    return task


# --------------------------------------------------------------------- comments


def create_comment(actor: User, task: Task, body: str) -> Comment:
    errors: list = []
    check_string(body, "body", MAX_COMMENT_LENGTH, errors)
    if errors:
        raise invalid(errors)
    comment = store.insert_comment(task.id, actor.id, body)
    store.record(actor.id, "create", "comment", comment.id)
    return comment


def remove_comment(actor: User, comment: Comment) -> None:
    if actor.role != "admin" and comment.authorId != actor.id:
        raise forbidden()
    del store.comments[comment.id]
    store.record(actor.id, "delete", "comment", comment.id)


# ------------------------------------------------------------------------ users


def create_user(actor: User, username: str, password: str, role, quota) -> User:
    errors: list = []
    check_string(username, "username", MAX_NAME_LENGTH, errors)
    check_string(password, "password", MAX_NAME_LENGTH, errors)
    check_role(role, errors)
    check_quota(quota, errors)
    if errors:
        raise invalid(errors)
    if store.find_by_username(username) is not None:
        raise conflict()
    user = store.insert_user(username, password, role, quota)
    store.record(actor.id, "create", "user", user.id)
    return user


def update_user(actor: User, user: User, body: dict) -> User:
    errors: list = []
    if "role" in body:
        check_role(body["role"], errors)
    if "quota" in body:
        check_quota(body["quota"], errors)
    if errors:
        raise invalid(errors)
    if "role" in body:
        user.role = body["role"]
    if "quota" in body:
        user.quota = body["quota"]
    user.version += 1
    store.record(actor.id, "update", "user", user.id)
    return user


def delete_user(actor: User, user: User) -> User:
    if user.id == actor.id:
        raise conflict()
    user.deleted = True
    user.version += 1
    store.record(actor.id, "delete", "user", user.id)
    return user


# --------------------------------------------------------- queries and reports


def visible_projects(user: User, include_deleted: bool) -> list[Project]:
    return [p for p in store.projects.values()
            if (include_deleted or not p.deleted)
            and (user.role == "admin" or p.ownerId == user.id)]


def visible_tasks(user: User, include_deleted: bool) -> list[Task]:
    allowed = {p.id for p in visible_projects(user, True)}
    return [t for t in store.tasks.values()
            if t.projectId in allowed and (include_deleted or not t.deleted)]


def search(user: User, query: str) -> dict:
    needle = query.lower()
    results = [{"type": "project", "id": p.id, "label": p.name}
               for p in visible_projects(user, False) if needle in p.name.lower()]
    results += [{"type": "task", "id": t.id, "label": t.title}
                for t in visible_tasks(user, False) if needle in t.title.lower()]
    return {"results": results, "total": len(results)}


def workload(user: User, group_by: str) -> dict:
    rows = visible_tasks(user, False)
    groups: list = []
    if group_by == "status":
        for status in STATUSES:
            picked = [t for t in rows if t.status == status]
            groups.append({"key": status, "tasks": len(picked),
                           "totalScore": sum(compute_score(t.priority, t.status)
                                             for t in picked)})
    elif group_by == "assignee":
        named = sorted({t.assigneeId for t in rows if t.assigneeId is not None})
        for assignee in named:
            picked = [t for t in rows if t.assigneeId == assignee]
            groups.append({"key": str(assignee), "tasks": len(picked),
                           "totalScore": sum(compute_score(t.priority, t.status)
                                             for t in picked)})
        loose = [t for t in rows if t.assigneeId is None]
        if loose:
            groups.append({"key": "unassigned", "tasks": len(loose),
                           "totalScore": sum(compute_score(t.priority, t.status)
                                             for t in loose)})
    else:
        for project in sorted(visible_projects(user, False), key=lambda p: p.id):
            picked = [t for t in rows if t.projectId == project.id]
            groups.append({"key": project.name, "tasks": len(picked),
                           "totalScore": sum(compute_score(t.priority, t.status)
                                             for t in picked)})
    return {"groupBy": group_by, "groups": groups}


def flush_outbox() -> int:
    pending = [event for event in store.outbox if not event.delivered]
    for event in pending:
        event.delivered = True
    return len(pending)


def metrics() -> dict:
    return {
        "requests": store.requests,
        "byStatus": {str(code): store.by_status[code] for code in sorted(store.by_status)},
        "byRoute": [{"route": route, "count": store.by_route[route]}
                    for route in sorted(store.by_route)],
        "auditEntries": len(store.audit),
        "outboxPending": store.outbox_pending(),
    }


def stats() -> dict:
    live = [t for t in store.tasks.values() if not t.deleted]
    counts = {status: 0 for status in STATUSES}
    for task in live:
        counts[task.status] += 1
    total = len(live)
    scores = [compute_score(t.priority, t.status) for t in live]
    best = None
    for project in store.projects.values():
        if project.deleted:
            continue
        if best is None or store.task_count(project.id) > store.task_count(best.id):
            best = project
    return {
        "projects": sum(1 for p in store.projects.values() if not p.deleted),
        "tasks": total,
        "users": sum(1 for u in store.users.values() if not u.deleted),
        "sessions": len(store.sessions),
        "comments": len(store.comments),
        "byStatus": counts,
        "avgScore": round(sum(scores) / total, 2) if total else 0.0,
        "topProjectName": best.name if best else None,
        "auditEntries": len(store.audit),
        "outboxPending": store.outbox_pending(),
    }


def check_bulk_size(operations) -> None:
    if not isinstance(operations, list) or not 1 <= len(operations) <= MAX_BULK_ITEMS:
        raise invalid([fail("operations", "operations is out of range")])
