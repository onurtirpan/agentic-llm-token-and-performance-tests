// Task Service, large tier — business rules, authorization and audit emission.

use crate::domain::{
    bad_request, check_priority, check_quota, check_role, check_status, check_string,
    compute_score, conflict, fail, forbidden, invalid, invalid_credentials, invalid_transition,
    not_found, precondition_failed, precondition_required, quota_exceeded, unauthorized,
    AppError, AuditEntry, Comment, Ctx, OutboxEvent, Project, Session, Task, User,
    DEFAULT_QUOTA, MAX_BULK_ITEMS, MAX_COMMENT_LENGTH, MAX_NAME_LENGTH, MAX_TITLE_LENGTH,
    STATUSES, TRANSITIONS,
};
use crate::store::Store;
use serde_json::{json, Map, Value};

// ------------------------------------------------------------------ serializers

pub fn serialize_user(user: &User) -> Value {
    json!({"id": user.id, "username": user.username, "role": user.role,
           "quota": user.quota, "version": user.version, "deleted": user.deleted})
}

pub fn serialize_project(project: &Project, store: &Store) -> Value {
    json!({"id": project.id, "name": project.name, "ownerId": project.owner_id,
           "taskCount": store.task_count(project.id), "version": project.version,
           "deleted": project.deleted})
}

/// `internalNote` is absent for a `user` role, so the object is built key by key.
pub fn serialize_task(task: &Task, role: &str) -> Value {
    let mut body = Map::new();
    body.insert("id".to_string(), json!(task.id));
    body.insert("projectId".to_string(), json!(task.project_id));
    body.insert("title".to_string(), json!(task.title));
    body.insert("priority".to_string(), json!(task.priority));
    body.insert("status".to_string(), json!(task.status));
    body.insert("assigneeId".to_string(), json!(task.assignee_id));
    if role == "admin" {
        body.insert("internalNote".to_string(), json!(task.internal_note));
    }
    body.insert("version".to_string(), json!(task.version));
    body.insert("deleted".to_string(), json!(task.deleted));
    body.insert(
        "score".to_string(),
        json!(compute_score(task.priority, &task.status)),
    );
    Value::Object(body)
}

pub fn serialize_comment(comment: &Comment) -> Value {
    json!({"id": comment.id, "taskId": comment.task_id,
           "authorId": comment.author_id, "body": comment.body})
}

pub fn serialize_audit(entry: &AuditEntry) -> Value {
    json!({"seq": entry.seq, "actorId": entry.actor_id, "action": entry.action,
           "resource": entry.resource, "resourceId": entry.resource_id})
}

pub fn serialize_outbox(event: &OutboxEvent) -> Value {
    json!({"seq": event.seq, "name": event.name, "resourceId": event.resource_id,
           "delivered": event.delivered})
}

// ----------------------------------------------------------------- access rules

pub fn authenticate(ctx: &Ctx, store: &Store) -> Result<User, AppError> {
    let Some(session) = store.sessions.get(&ctx.token) else {
        return Err(unauthorized(ctx));
    };
    let Some(user) = store.find_user(session.user_id, false) else {
        return Err(unauthorized(ctx));
    };
    ctx.out.lock().unwrap().user_id = Some(user.id);
    Ok(user)
}

pub fn charge_quota(ctx: &Ctx, store: &mut Store, user: &User) -> Result<(), AppError> {
    let session = store.sessions.get_mut(&ctx.token).unwrap();
    if session.used >= user.quota {
        return Err(quota_exceeded(ctx));
    }
    session.used += 1;
    ctx.out.lock().unwrap().quota_remaining = Some((user.quota - session.used).max(0));
    Ok(())
}

pub fn require_admin(ctx: &Ctx, user: &User) -> Result<(), AppError> {
    if user.role != "admin" {
        return Err(forbidden(ctx));
    }
    Ok(())
}

pub fn reachable_project(
    ctx: &Ctx,
    store: &Store,
    project_id: i64,
    user: &User,
    include_deleted: bool,
) -> Result<Project, AppError> {
    let Some(project) = store.find_project(project_id, include_deleted) else {
        return Err(not_found(ctx));
    };
    if user.role != "admin" && project.owner_id != user.id {
        return Err(forbidden(ctx));
    }
    Ok(project)
}

pub fn reachable_task(
    ctx: &Ctx,
    store: &Store,
    task_id: i64,
    user: &User,
    include_deleted: bool,
) -> Result<Task, AppError> {
    let Some(task) = store.find_task(task_id, include_deleted) else {
        return Err(not_found(ctx));
    };
    reachable_project(ctx, store, task.project_id, user, true)?;
    Ok(task)
}

pub fn check_if_match(ctx: &Ctx, given: Option<&str>, version: i64) -> Result<(), AppError> {
    match given {
        None | Some("") => Err(precondition_required(ctx)),
        Some(value) if value == version.to_string() => Ok(()),
        _ => Err(precondition_failed(ctx)),
    }
}

pub fn check_include_deleted(
    ctx: &Ctx,
    raw: Option<&String>,
    user: &User,
) -> Result<bool, AppError> {
    let Some(raw) = raw else { return Ok(false) };
    if user.role != "admin" {
        return Err(forbidden(ctx));
    }
    Ok(raw == "true")
}

// ------------------------------------------------------------------- pagination

/// Sort by the tiebreak first, then stably by the requested field.
pub fn paginate(mut rows: Vec<Value>, limit: i64, offset: i64, sort: &str, order: &str) -> Value {
    let tiebreak = if rows.first().is_some_and(|row| row.get("seq").is_some()) {
        "seq"
    } else {
        "id"
    };
    rows.sort_by_key(|row| row[tiebreak].as_i64());
    rows.sort_by(|left, right| {
        let ordering = match (&left[sort], &right[sort]) {
            (Value::String(one), Value::String(other)) => one.cmp(other),
            (one, other) => one.as_i64().cmp(&other.as_i64()),
        };
        if order == "desc" {
            ordering.reverse()
        } else {
            ordering
        }
    });
    let total = rows.len();
    let items: Vec<Value> = rows
        .into_iter()
        .skip(offset as usize)
        .take(limit as usize)
        .collect();
    json!({"items": items, "total": total, "limit": limit, "offset": offset})
}

// ------------------------------------------------------------------------- auth

pub fn login(
    ctx: &Ctx,
    store: &mut Store,
    username: &str,
    password: &str,
    token: &str,
) -> Result<User, AppError> {
    let user = store
        .find_by_username(username)
        .filter(|user| user.password == password)
        .ok_or_else(|| invalid_credentials(ctx))?;
    store.sessions.insert(
        token.to_string(),
        Session {
            user_id: user.id,
            used: 0,
        },
    );
    Ok(user)
}

// --------------------------------------------------------------------- projects

pub fn create_project(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    name: &str,
    owner_id: Option<i64>,
) -> Result<Project, AppError> {
    let mut errors: Vec<Value> = vec![];
    check_string(name, "name", MAX_NAME_LENGTH, &mut errors);
    if !owner_id.is_some_and(|id| store.find_user(id, false).is_some()) {
        errors.push(fail("ownerId", "ownerId is not a known user"));
    }
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    let owner_id = owner_id.unwrap_or_default();
    if store
        .projects
        .values()
        .any(|other| other.owner_id == owner_id && other.name == name && !other.deleted)
    {
        return Err(conflict(ctx));
    }
    let project = store.insert_project(name, owner_id);
    store.record(actor.id, "create", "project", project.id);
    Ok(project)
}

pub fn rename_project(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    project: &Project,
    name: &str,
) -> Result<Project, AppError> {
    let mut errors: Vec<Value> = vec![];
    check_string(name, "name", MAX_NAME_LENGTH, &mut errors);
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    if store.projects.values().any(|other| {
        other.owner_id == project.owner_id
            && other.name == name
            && other.id != project.id
            && !other.deleted
    }) {
        return Err(conflict(ctx));
    }
    let row = store.projects.get_mut(&project.id).unwrap();
    row.name = name.to_string();
    row.version += 1;
    let renamed = row.clone();
    store.record(actor.id, "update", "project", renamed.id);
    Ok(renamed)
}

pub fn delete_project(store: &mut Store, actor: &User, project_id: i64) -> Project {
    let row = store.projects.get_mut(&project_id).unwrap();
    row.deleted = true;
    row.version += 1;
    let project = row.clone();
    store.record(actor.id, "delete", "project", project.id);
    for task in store.live_tasks_of(project.id) {
        let row = store.tasks.get_mut(&task.id).unwrap();
        row.deleted = true;
        row.version += 1;
        store.record(actor.id, "delete", "task", task.id);
    }
    project
}

pub fn restore_project(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    project: &Project,
) -> Result<Project, AppError> {
    if !project.deleted {
        return Err(conflict(ctx));
    }
    let row = store.projects.get_mut(&project.id).unwrap();
    row.deleted = false;
    row.version += 1;
    let restored = row.clone();
    store.record(actor.id, "restore", "project", restored.id);
    Ok(restored)
}

// ------------------------------------------------------------------------ tasks

pub fn read_note(
    ctx: &Ctx,
    actor: &User,
    body: &Value,
    errors: &mut Vec<Value>,
    current: &str,
) -> Result<String, AppError> {
    let Some(note) = body.get("internalNote") else {
        return Ok(current.to_string());
    };
    if actor.role != "admin" {
        return Err(forbidden(ctx));
    }
    let Some(note) = note.as_str() else {
        return Err(bad_request(ctx));
    };
    if note.chars().count() > MAX_TITLE_LENGTH {
        errors.push(fail("internalNote", "internalNote is too long"));
    }
    Ok(note.to_string())
}

fn check_task_fields(
    ctx: &Ctx,
    store: &Store,
    title: &str,
    priority: Option<i64>,
    assignee_id: Option<i64>,
    mut errors: Vec<Value>,
) -> Result<(), AppError> {
    check_string(title, "title", MAX_TITLE_LENGTH, &mut errors);
    check_priority(priority, &mut errors);
    if assignee_id.is_some_and(|id| store.find_user(id, false).is_none()) {
        errors.push(fail("assigneeId", "assigneeId is not a known user"));
    }
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub fn create_task(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    project_id: i64,
    title: &str,
    priority: Option<i64>,
    assignee_id: Option<i64>,
    note: &str,
    errors: Vec<Value>,
) -> Result<Task, AppError> {
    check_task_fields(ctx, store, title, priority, assignee_id, errors)?;
    let task = store.insert_task(
        project_id,
        title,
        priority.unwrap_or_default(),
        assignee_id,
        note,
    );
    store.record(actor.id, "create", "task", task.id);
    Ok(task)
}

#[allow(clippy::too_many_arguments)]
pub fn replace_task(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    task_id: i64,
    title: &str,
    priority: Option<i64>,
    assignee_id: Option<i64>,
    note: &str,
    errors: Vec<Value>,
) -> Result<Task, AppError> {
    check_task_fields(ctx, store, title, priority, assignee_id, errors)?;
    let row = store.tasks.get_mut(&task_id).unwrap();
    row.title = title.to_string();
    row.priority = priority.unwrap_or_default();
    row.assignee_id = assignee_id;
    row.internal_note = note.to_string();
    row.version += 1;
    let task = row.clone();
    store.record(actor.id, "update", "task", task.id);
    Ok(task)
}

pub fn move_status(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    task: &Task,
    status: Option<&Value>,
) -> Result<Task, AppError> {
    let mut errors: Vec<Value> = vec![];
    check_status(status, &mut errors);
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    let status = status.and_then(Value::as_str).unwrap_or_default();
    if !TRANSITIONS.contains(&(task.status.as_str(), status)) {
        return Err(invalid_transition(ctx));
    }
    let row = store.tasks.get_mut(&task.id).unwrap();
    row.status = status.to_string();
    row.version += 1;
    let moved = row.clone();
    store.record(actor.id, "update", "task", moved.id);
    Ok(moved)
}

pub fn delete_task(store: &mut Store, actor: &User, task_id: i64) -> Task {
    let row = store.tasks.get_mut(&task_id).unwrap();
    row.deleted = true;
    row.version += 1;
    let task = row.clone();
    store.record(actor.id, "delete", "task", task.id);
    task
}

pub fn restore_task(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    task: &Task,
) -> Result<Task, AppError> {
    if !task.deleted {
        return Err(conflict(ctx));
    }
    let row = store.tasks.get_mut(&task.id).unwrap();
    row.deleted = false;
    row.version += 1;
    let restored = row.clone();
    store.record(actor.id, "restore", "task", restored.id);
    Ok(restored)
}

// --------------------------------------------------------------------- comments

pub fn create_comment(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    task_id: i64,
    body: &str,
) -> Result<Comment, AppError> {
    let mut errors: Vec<Value> = vec![];
    check_string(body, "body", MAX_COMMENT_LENGTH, &mut errors);
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    let comment = store.insert_comment(task_id, actor.id, body);
    store.record(actor.id, "create", "comment", comment.id);
    Ok(comment)
}

pub fn remove_comment(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    comment: &Comment,
) -> Result<(), AppError> {
    if actor.role != "admin" && comment.author_id != actor.id {
        return Err(forbidden(ctx));
    }
    store.comments.remove(&comment.id);
    store.record(actor.id, "delete", "comment", comment.id);
    Ok(())
}

// ------------------------------------------------------------------------ users

pub fn create_user(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    username: &str,
    password: &str,
    body: &Value,
) -> Result<User, AppError> {
    let mut errors: Vec<Value> = vec![];
    check_string(username, "username", MAX_NAME_LENGTH, &mut errors);
    check_string(password, "password", MAX_NAME_LENGTH, &mut errors);
    let role = body.get("role").cloned().unwrap_or(json!("user"));
    let quota = body.get("quota").cloned().unwrap_or(json!(DEFAULT_QUOTA));
    check_role(&role, &mut errors);
    check_quota(&quota, &mut errors);
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    if store.find_by_username(username).is_some() {
        return Err(conflict(ctx));
    }
    let user = store.insert_user(
        username,
        password,
        role.as_str().unwrap_or_default(),
        quota.as_i64().unwrap_or_default(),
    );
    store.record(actor.id, "create", "user", user.id);
    Ok(user)
}

pub fn update_user(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    user_id: i64,
    body: &Value,
) -> Result<User, AppError> {
    let mut errors: Vec<Value> = vec![];
    if let Some(role) = body.get("role") {
        check_role(role, &mut errors);
    }
    if let Some(quota) = body.get("quota") {
        check_quota(quota, &mut errors);
    }
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    let row = store.users.get_mut(&user_id).unwrap();
    if let Some(role) = body.get("role").and_then(Value::as_str) {
        row.role = role.to_string();
    }
    if let Some(quota) = body.get("quota").and_then(Value::as_i64) {
        row.quota = quota;
    }
    row.version += 1;
    let user = row.clone();
    store.record(actor.id, "update", "user", user.id);
    Ok(user)
}

pub fn delete_user(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    user_id: i64,
) -> Result<User, AppError> {
    if user_id == actor.id {
        return Err(conflict(ctx));
    }
    let row = store.users.get_mut(&user_id).unwrap();
    row.deleted = true;
    row.version += 1;
    let user = row.clone();
    store.record(actor.id, "delete", "user", user.id);
    Ok(user)
}

// --------------------------------------------------------- queries and reports

pub fn visible_projects(store: &Store, user: &User, include_deleted: bool) -> Vec<Project> {
    store
        .projects
        .values()
        .filter(|project| include_deleted || !project.deleted)
        .filter(|project| user.role == "admin" || project.owner_id == user.id)
        .cloned()
        .collect()
}

pub fn visible_tasks(store: &Store, user: &User, include_deleted: bool) -> Vec<Task> {
    let allowed: Vec<i64> = visible_projects(store, user, true)
        .iter()
        .map(|project| project.id)
        .collect();
    store
        .tasks
        .values()
        .filter(|task| allowed.contains(&task.project_id) && (include_deleted || !task.deleted))
        .cloned()
        .collect()
}

pub fn search(store: &Store, user: &User, query: &str) -> Value {
    let needle = query.to_lowercase();
    let mut results: Vec<Value> = visible_projects(store, user, false)
        .iter()
        .filter(|project| project.name.to_lowercase().contains(&needle))
        .map(|project| json!({"type": "project", "id": project.id, "label": project.name}))
        .collect();
    results.extend(
        visible_tasks(store, user, false)
            .iter()
            .filter(|task| task.title.to_lowercase().contains(&needle))
            .map(|task| json!({"type": "task", "id": task.id, "label": task.title})),
    );
    let total = results.len();
    json!({"results": results, "total": total})
}

fn group(key: &str, picked: Vec<&Task>) -> Value {
    json!({"key": key, "tasks": picked.len(),
           "totalScore": picked.iter()
               .map(|task| compute_score(task.priority, &task.status)).sum::<i64>()})
}

pub fn workload(store: &Store, user: &User, group_by: &str) -> Value {
    let rows = visible_tasks(store, user, false);
    let mut groups: Vec<Value> = vec![];
    if group_by == "status" {
        for status in STATUSES {
            groups.push(group(
                status,
                rows.iter().filter(|task| task.status == status).collect(),
            ));
        }
    } else if group_by == "assignee" {
        let mut named: Vec<i64> = rows.iter().filter_map(|task| task.assignee_id).collect();
        named.sort_unstable();
        named.dedup();
        for assignee in named {
            groups.push(group(
                &assignee.to_string(),
                rows.iter()
                    .filter(|task| task.assignee_id == Some(assignee))
                    .collect(),
            ));
        }
        let loose: Vec<&Task> = rows
            .iter()
            .filter(|task| task.assignee_id.is_none())
            .collect();
        if !loose.is_empty() {
            groups.push(group("unassigned", loose));
        }
    } else {
        for project in visible_projects(store, user, false) {
            groups.push(group(
                &project.name,
                rows.iter()
                    .filter(|task| task.project_id == project.id)
                    .collect(),
            ));
        }
    }
    json!({"groupBy": group_by, "groups": groups})
}

pub fn flush_outbox(store: &mut Store) -> usize {
    let mut flushed = 0;
    for event in store.outbox.iter_mut().filter(|event| !event.delivered) {
        event.delivered = true;
        flushed += 1;
    }
    flushed
}

pub fn metrics(store: &Store) -> Value {
    let mut by_status = Map::new();
    for (code, count) in &store.by_status {
        by_status.insert(code.to_string(), json!(count));
    }
    let by_route: Vec<Value> = store
        .by_route
        .iter()
        .map(|(route, count)| json!({"route": route, "count": count}))
        .collect();
    json!({"requests": store.requests, "byStatus": by_status, "byRoute": by_route,
           "auditEntries": store.audit.len(), "outboxPending": store.outbox_pending()})
}

pub fn stats(store: &Store) -> Value {
    let live: Vec<&Task> = store.tasks.values().filter(|task| !task.deleted).collect();
    let mut by_status = Map::new();
    for status in STATUSES {
        by_status.insert(
            status.to_string(),
            json!(live.iter().filter(|task| task.status == status).count()),
        );
    }
    let scores: i64 = live
        .iter()
        .map(|task| compute_score(task.priority, &task.status))
        .sum();
    let avg_score = if live.is_empty() {
        0.0
    } else {
        (scores as f64 / live.len() as f64 * 100.0).round() / 100.0
    };
    let mut best: Option<&Project> = None;
    for project in store.projects.values().filter(|project| !project.deleted) {
        if best.is_none_or(|found| store.task_count(project.id) > store.task_count(found.id)) {
            best = Some(project);
        }
    }
    json!({
        "projects": store.projects.values().filter(|project| !project.deleted).count(),
        "tasks": live.len(),
        "users": store.users.values().filter(|user| !user.deleted).count(),
        "sessions": store.sessions.len(),
        "comments": store.comments.len(),
        "byStatus": by_status,
        "avgScore": avg_score,
        "topProjectName": best.map(|project| project.name.clone()),
        "auditEntries": store.audit.len(),
        "outboxPending": store.outbox_pending(),
    })
}

pub fn check_bulk_size(ctx: &Ctx, operations: Option<&Value>) -> Result<(), AppError> {
    let count = operations.and_then(Value::as_array).map_or(0, Vec::len);
    if count < 1 || count > MAX_BULK_ITEMS {
        return Err(invalid(
            ctx,
            vec![fail("operations", "operations is out of range")],
        ));
    }
    Ok(())
}
