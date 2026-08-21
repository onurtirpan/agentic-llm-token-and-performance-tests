// Task Service, mid tier — Axum implementation.

use axum::extract::{Extension, Path, Query, Request, State};
use axum::http::{HeaderValue, StatusCode};
use axum::middleware::{self, Next};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, patch, post};
use axum::{Json, Router};
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap};
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

const MAX_TITLE_LENGTH: usize = 80;
const MAX_NAME_LENGTH: usize = 60;
const MIN_PRIORITY: i64 = 1;
const MAX_PRIORITY: i64 = 5;
const DEFAULT_LIMIT: i64 = 20;
const MAX_LIMIT: i64 = 100;
const PORT: u16 = 8080;

const STATUS_BONUS: [(&str, i64); 4] =
    [("todo", 0), ("in_progress", 3), ("done", 5), ("archived", 0)];
const TRANSITIONS: [(&str, &str); 5] = [
    ("todo", "in_progress"),
    ("todo", "archived"),
    ("in_progress", "todo"),
    ("in_progress", "done"),
    ("done", "archived"),
];
const PROJECT_SORTS: [&str; 3] = ["id", "name", "taskCount"];
const TASK_SORTS: [&str; 5] = ["id", "title", "priority", "score", "status"];

#[derive(Clone)]
struct User {
    id: i64,
    username: String,
    password: String,
    role: String,
}

#[derive(Clone)]
struct Project {
    id: i64,
    name: String,
    owner_id: i64,
}

#[derive(Clone)]
struct Task {
    id: i64,
    project_id: i64,
    title: String,
    priority: i64,
    status: String,
    assignee_id: Option<i64>,
    score: i64,
}

struct AppError {
    status: StatusCode,
    code: &'static str,
    message: &'static str,
    request_id: String,
    details: Vec<Value>,
}

struct Store {
    users: BTreeMap<i64, User>,
    sessions: BTreeMap<String, i64>,
    projects: BTreeMap<i64, Project>,
    tasks: BTreeMap<i64, Task>,
    next_project_id: i64,
    next_task_id: i64,
}

#[derive(Clone)]
struct Ctx {
    request_id: String,
    token: String,
    user_id: Arc<Mutex<Option<i64>>>,
}

type Shared = State<Arc<Mutex<Store>>>;

impl AppError {
    fn new(
        ctx: &Ctx,
        status: StatusCode,
        code: &'static str,
        message: &'static str,
        details: Vec<Value>,
    ) -> AppError {
        AppError {
            status,
            code,
            message,
            request_id: ctx.request_id.clone(),
            details,
        }
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        let status = self.status;
        let body = json!({ "error": {
            "code": self.code,
            "message": self.message,
            "requestId": self.request_id,
            "details": self.details,
        }});
        (status, Json(body)).into_response()
    }
}

fn nanos() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos()
}

fn compute_score(priority: i64, status: &str) -> i64 {
    let base_score = priority * 10;
    let bonus = STATUS_BONUS
        .iter()
        .find(|(name, _)| *name == status)
        .map_or(0, |(_, bonus)| *bonus);
    base_score + bonus
}

fn task_count(project_id: i64, store: &Store) -> usize {
    store
        .tasks
        .values()
        .filter(|task| task.project_id == project_id)
        .count()
}

fn serialize_project(project: &Project, store: &Store) -> Value {
    json!({
        "id": project.id,
        "name": project.name,
        "ownerId": project.owner_id,
        "taskCount": task_count(project.id, store),
    })
}

fn serialize_task(task: &Task) -> Value {
    json!({
        "id": task.id,
        "projectId": task.project_id,
        "title": task.title,
        "priority": task.priority,
        "status": task.status,
        "assigneeId": task.assignee_id,
        "score": task.score,
    })
}

fn bad_request(ctx: &Ctx) -> AppError {
    AppError::new(
        ctx,
        StatusCode::BAD_REQUEST,
        "bad_request",
        "the request is malformed",
        vec![],
    )
}

fn not_found(ctx: &Ctx) -> AppError {
    AppError::new(
        ctx,
        StatusCode::NOT_FOUND,
        "not_found",
        "the resource does not exist",
        vec![],
    )
}

fn forbidden(ctx: &Ctx) -> AppError {
    AppError::new(
        ctx,
        StatusCode::FORBIDDEN,
        "forbidden",
        "you may not access this resource",
        vec![],
    )
}

fn conflict(ctx: &Ctx) -> AppError {
    AppError::new(
        ctx,
        StatusCode::CONFLICT,
        "conflict",
        "the resource already exists",
        vec![],
    )
}

fn invalid(ctx: &Ctx, mut details: Vec<Value>) -> AppError {
    details.sort_by(|left, right| {
        (left["field"].as_str(), left["message"].as_str())
            .cmp(&(right["field"].as_str(), right["message"].as_str()))
    });
    AppError::new(
        ctx,
        StatusCode::UNPROCESSABLE_ENTITY,
        "validation_failed",
        "the request body is not valid",
        details,
    )
}

fn fail(field: &str, message: &str) -> Value {
    json!({ "field": field, "message": message })
}

fn read_body(ctx: &Ctx, raw: &str) -> Result<Value, AppError> {
    if raw.trim().is_empty() {
        return Ok(json!({}));
    }
    match serde_json::from_str::<Value>(raw) {
        Ok(parsed) if parsed.is_object() => Ok(parsed),
        _ => Err(bad_request(ctx)),
    }
}

fn read_int(
    ctx: &Ctx,
    body: &Value,
    field: &str,
    default: Option<i64>,
) -> Result<Option<i64>, AppError> {
    match body.get(field) {
        None => Ok(default),
        Some(Value::Null) => Ok(None),
        Some(Value::Number(number)) if number.is_i64() => Ok(number.as_i64()),
        _ => Err(bad_request(ctx)),
    }
}

fn read_string(
    ctx: &Ctx,
    body: &Value,
    field: &str,
    errors: &mut Vec<Value>,
    max_length: usize,
    required: bool,
) -> Result<String, AppError> {
    let value = match body.get(field) {
        None => "",
        Some(Value::String(text)) => text,
        _ => return Err(bad_request(ctx)),
    };
    if value.is_empty() {
        if required {
            errors.push(fail(field, &format!("{field} is required")));
        }
    } else if value.chars().count() > max_length {
        errors.push(fail(field, &format!("{field} is too long")));
    }
    Ok(value.to_string())
}

fn read_priority(ctx: &Ctx, body: &Value, errors: &mut Vec<Value>) -> Result<i64, AppError> {
    let value = read_int(ctx, body, "priority", Some(0))?;
    if !matches!(value, Some(number) if (MIN_PRIORITY..=MAX_PRIORITY).contains(&number)) {
        errors.push(fail("priority", "priority is out of range"));
    }
    Ok(value.unwrap_or(0))
}

fn read_user_ref(
    ctx: &Ctx,
    body: &Value,
    field: &str,
    errors: &mut Vec<Value>,
    default: Option<i64>,
    store: &Store,
) -> Result<Option<i64>, AppError> {
    let value = read_int(ctx, body, field, default)?;
    if matches!(value, Some(id) if !store.users.contains_key(&id)) {
        errors.push(fail(field, &format!("{field} is not a known user")));
    }
    Ok(value)
}

fn parse_id(ctx: &Ctx, raw: &str) -> Result<i64, AppError> {
    raw.parse::<i64>().map_err(|_| bad_request(ctx))
}

fn read_page(
    ctx: &Ctx,
    query: &HashMap<String, String>,
    allowed: &[&str],
) -> Result<(i64, i64, String, String), AppError> {
    let mut errors: Vec<Value> = vec![];
    let (mut limit, mut offset) = (DEFAULT_LIMIT, 0);
    let sort = query.get("sort").cloned().unwrap_or("id".to_string());
    let order = query.get("order").cloned().unwrap_or("asc".to_string());
    if let Some(raw) = query.get("limit") {
        limit = raw.parse::<i64>().unwrap_or(-1);
        if limit < 1 || limit > MAX_LIMIT {
            errors.push(fail("limit", "limit is out of range"));
        }
    }
    if let Some(raw) = query.get("offset") {
        offset = raw.parse::<i64>().unwrap_or(-1);
        if offset < 0 {
            errors.push(fail("offset", "offset is out of range"));
        }
    }
    if !allowed.contains(&sort.as_str()) {
        errors.push(fail("sort", "sort is not a valid field"));
    }
    if order != "asc" && order != "desc" {
        errors.push(fail("order", "order must be asc or desc"));
    }
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    Ok((limit, offset, sort, order))
}

fn paginate(mut rows: Vec<Value>, limit: i64, offset: i64, sort: &str, order: &str) -> Value {
    rows.sort_by_key(|row| row["id"].as_i64());
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
    json!({ "items": items, "total": total, "limit": limit, "offset": offset })
}

fn authenticate(ctx: &Ctx, store: &Store) -> Result<User, AppError> {
    let Some(&id) = store.sessions.get(&ctx.token) else {
        return Err(AppError::new(
            ctx,
            StatusCode::UNAUTHORIZED,
            "unauthorized",
            "authentication is required",
            vec![],
        ));
    };
    *ctx.user_id.lock().unwrap() = Some(id);
    Ok(store.users[&id].clone())
}

fn require_admin(ctx: &Ctx, user: &User) -> Result<(), AppError> {
    if user.role != "admin" {
        return Err(forbidden(ctx));
    }
    Ok(())
}

fn reachable_project(
    ctx: &Ctx,
    project_id: i64,
    user: &User,
    store: &Store,
) -> Result<Project, AppError> {
    let Some(project) = store.projects.get(&project_id) else {
        return Err(not_found(ctx));
    };
    if user.role != "admin" && project.owner_id != user.id {
        return Err(forbidden(ctx));
    }
    Ok(project.clone())
}

fn reachable_task(ctx: &Ctx, task_id: i64, user: &User, store: &Store) -> Result<Task, AppError> {
    let Some(task) = store.tasks.get(&task_id).cloned() else {
        return Err(not_found(ctx));
    };
    reachable_project(ctx, task.project_id, user, store)?;
    Ok(task)
}

async fn observe(mut request: Request, next: Next) -> Response {
    let ctx = Ctx {
        request_id: request
            .headers()
            .get("x-request-id")
            .and_then(|value| value.to_str().ok())
            .filter(|value| !value.is_empty())
            .map_or_else(|| format!("{:x}", nanos()), str::to_string),
        token: request
            .headers()
            .get("authorization")
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.strip_prefix("Bearer "))
            .unwrap_or("")
            .to_string(),
        user_id: Arc::new(Mutex::new(None)),
    };
    let method = request.method().to_string();
    let path = request.uri().path().to_string();
    let started = Instant::now();
    request.extensions_mut().insert(ctx.clone());
    let mut response = next.run(request).await;
    let status = response.status().as_u16();
    println!(
        "{}",
        json!({
            "level": if status >= 500 { "error" } else if status >= 400 { "warn" } else { "info" },
            "requestId": ctx.request_id,
            "method": method,
            "path": path,
            "status": status,
            "durationMs": started.elapsed().as_millis() as i64,
            "userId": *ctx.user_id.lock().unwrap(),
        })
    );
    response.headers_mut().insert(
        "x-request-id",
        HeaderValue::from_str(&ctx.request_id).unwrap(),
    );
    response
}

async fn get_health(State(store): Shared) -> Response {
    let store = store.lock().unwrap();
    Json(json!({
        "status": "ok",
        "projects": store.projects.len(),
        "tasks": store.tasks.len(),
    }))
    .into_response()
}

async fn login(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let username = read_string(&ctx, &body, "username", &mut errors, MAX_NAME_LENGTH, true)?;
    let password = read_string(&ctx, &body, "password", &mut errors, MAX_NAME_LENGTH, true)?;
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    let mut store = store.lock().unwrap();
    let user = store
        .users
        .values()
        .find(|user| user.username == username && user.password == password)
        .cloned()
        .ok_or_else(|| {
            AppError::new(
                &ctx,
                StatusCode::UNAUTHORIZED,
                "invalid_credentials",
                "the username or password is wrong",
                vec![],
            )
        })?;
    let token = format!("{:x}{:016x}", store.sessions.len(), nanos());
    store.sessions.insert(token.clone(), user.id);
    Ok(Json(json!({ "token": token, "userId": user.id, "role": user.role })).into_response())
}

async fn logout(State(store): Shared, Extension(ctx): Extension<Ctx>) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    authenticate(&ctx, &store)?;
    store.sessions.remove(&ctx.token);
    Ok(StatusCode::NO_CONTENT.into_response())
}

async fn get_me(State(store): Shared, Extension(ctx): Extension<Ctx>) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    Ok(Json(json!({
        "userId": user.id,
        "username": user.username,
        "role": user.role,
    }))
    .into_response())
}

async fn list_projects(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &PROJECT_SORTS)?;
    let rows = store
        .projects
        .values()
        .filter(|project| user.role == "admin" || project.owner_id == user.id)
        .map(|project| serialize_project(project, &store))
        .collect();
    Ok(Json(paginate(rows, limit, offset, &sort, &order)).into_response())
}

async fn create_project(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    require_admin(&ctx, &user)?;
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let name = read_string(&ctx, &body, "name", &mut errors, MAX_NAME_LENGTH, true)?;
    let owner_id = read_user_ref(&ctx, &body, "ownerId", &mut errors, Some(user.id), &store)?
        .unwrap_or(user.id);
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    if store
        .projects
        .values()
        .any(|project| project.owner_id == owner_id && project.name == name)
    {
        return Err(conflict(&ctx));
    }
    let project = Project {
        id: store.next_project_id,
        name,
        owner_id,
    };
    store.projects.insert(project.id, project.clone());
    store.next_project_id += 1;
    Ok((
        StatusCode::CREATED,
        Json(serialize_project(&project, &store)),
    )
        .into_response())
}

async fn get_project(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let project = reachable_project(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    Ok(Json(serialize_project(&project, &store)).into_response())
}

async fn update_project(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    require_admin(&ctx, &user)?;
    let mut project = reachable_project(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    let body = read_body(&ctx, &body)?;
    if body.get("name").is_none() {
        return Ok(Json(serialize_project(&project, &store)).into_response());
    }
    let mut errors: Vec<Value> = vec![];
    let name = read_string(&ctx, &body, "name", &mut errors, MAX_NAME_LENGTH, true)?;
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    if store.projects.values().any(|other| {
        other.owner_id == project.owner_id && other.name == name && other.id != project.id
    }) {
        return Err(conflict(&ctx));
    }
    project.name = name;
    store.projects.insert(project.id, project.clone());
    Ok(Json(serialize_project(&project, &store)).into_response())
}

async fn delete_project(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    require_admin(&ctx, &user)?;
    let project = reachable_project(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    store.tasks.retain(|_, task| task.project_id != project.id);
    store.projects.remove(&project.id);
    Ok(StatusCode::NO_CONTENT.into_response())
}

async fn list_tasks(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let project = reachable_project(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &TASK_SORTS)?;
    let rows = store
        .tasks
        .values()
        .filter(|task| task.project_id == project.id)
        .map(serialize_task)
        .collect();
    Ok(Json(paginate(rows, limit, offset, &sort, &order)).into_response())
}

async fn create_task(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let project = reachable_project(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let title = read_string(&ctx, &body, "title", &mut errors, MAX_TITLE_LENGTH, true)?;
    let priority = read_priority(&ctx, &body, &mut errors)?;
    let assignee_id = read_user_ref(&ctx, &body, "assigneeId", &mut errors, None, &store)?;
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    let task = Task {
        id: store.next_task_id,
        project_id: project.id,
        title,
        priority,
        status: "todo".to_string(),
        assignee_id,
        score: compute_score(priority, "todo"),
    };
    store.tasks.insert(task.id, task.clone());
    store.next_task_id += 1;
    Ok((StatusCode::CREATED, Json(serialize_task(&task))).into_response())
}

async fn get_task(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let task = reachable_task(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    Ok(Json(serialize_task(&task)).into_response())
}

async fn replace_task(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let mut task = reachable_task(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let title = read_string(&ctx, &body, "title", &mut errors, MAX_TITLE_LENGTH, true)?;
    let priority = read_priority(&ctx, &body, &mut errors)?;
    let assignee_id = read_user_ref(&ctx, &body, "assigneeId", &mut errors, None, &store)?;
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    task.title = title;
    task.priority = priority;
    task.assignee_id = assignee_id;
    task.score = compute_score(priority, &task.status);
    store.tasks.insert(task.id, task.clone());
    Ok(Json(serialize_task(&task)).into_response())
}

async fn update_status(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let mut task = reachable_task(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    let body = read_body(&ctx, &body)?;
    let status = body.get("status").and_then(Value::as_str).unwrap_or("");
    if !STATUS_BONUS.iter().any(|(name, _)| *name == status) {
        return Err(invalid(&ctx, vec![fail("status", "status is not valid")]));
    }
    if !TRANSITIONS.contains(&(task.status.as_str(), status)) {
        return Err(AppError::new(
            &ctx,
            StatusCode::CONFLICT,
            "invalid_transition",
            "the status change is not allowed",
            vec![],
        ));
    }
    task.status = status.to_string();
    task.score = compute_score(task.priority, status);
    store.tasks.insert(task.id, task.clone());
    Ok(Json(serialize_task(&task)).into_response())
}

async fn delete_task(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    let task = reachable_task(&ctx, parse_id(&ctx, &raw_id)?, &user, &store)?;
    store.tasks.remove(&task.id);
    Ok(StatusCode::NO_CONTENT.into_response())
}

async fn get_stats(
    State(store): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let store = store.lock().unwrap();
    let user = authenticate(&ctx, &store)?;
    require_admin(&ctx, &user)?;
    let mut by_status = serde_json::Map::new();
    for (name, _) in STATUS_BONUS {
        let count = store.tasks.values().filter(|task| task.status == name).count();
        by_status.insert(name.to_string(), json!(count));
    }
    let total = store.tasks.len();
    let sum_score: i64 = store.tasks.values().map(|task| task.score).sum();
    let avg_score = if total == 0 {
        0.0
    } else {
        (sum_score as f64 / total as f64 * 100.0).round() / 100.0
    };
    let mut best: Option<&Project> = None;
    for project in store.projects.values() {
        if best.is_none_or(|found| task_count(project.id, &store) > task_count(found.id, &store)) {
            best = Some(project);
        }
    }
    Ok(Json(json!({
        "projects": store.projects.len(),
        "tasks": total,
        "users": store.users.len(),
        "sessions": store.sessions.len(),
        "byStatus": by_status,
        "avgScore": avg_score,
        "topProjectName": best.map(|project| project.name.clone()),
    }))
    .into_response())
}

async fn fallback(Extension(ctx): Extension<Ctx>) -> AppError {
    not_found(&ctx)
}

#[tokio::main]
async fn main() {
    let store = Arc::new(Mutex::new(Store {
        users: BTreeMap::from([
            (
                1,
                User {
                    id: 1,
                    username: "admin".to_string(),
                    password: "admin-secret".to_string(),
                    role: "admin".to_string(),
                },
            ),
            (
                2,
                User {
                    id: 2,
                    username: "alice".to_string(),
                    password: "alice-secret".to_string(),
                    role: "user".to_string(),
                },
            ),
            (
                3,
                User {
                    id: 3,
                    username: "bob".to_string(),
                    password: "bob-secret".to_string(),
                    role: "user".to_string(),
                },
            ),
        ]),
        sessions: BTreeMap::new(),
        projects: BTreeMap::new(),
        tasks: BTreeMap::new(),
        next_project_id: 1,
        next_task_id: 1,
    }));
    let app = Router::new()
        .route("/health", get(get_health))
        .route("/auth/login", post(login))
        .route("/auth/logout", post(logout))
        .route("/me", get(get_me))
        .route("/projects", get(list_projects).post(create_project))
        .route(
            "/projects/{id}",
            get(get_project).patch(update_project).delete(delete_project),
        )
        .route("/projects/{id}/tasks", get(list_tasks).post(create_task))
        .route(
            "/tasks/{id}",
            get(get_task).put(replace_task).delete(delete_task),
        )
        .route("/tasks/{id}/status", patch(update_status))
        .route("/stats", get(get_stats))
        .fallback(fallback)
        .layer(middleware::from_fn(observe))
        .with_state(store);
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", PORT))
        .await
        .unwrap();
    axum::serve(listener, app).await.unwrap();
}
