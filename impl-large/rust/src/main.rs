// Task Service, large tier — HTTP routing, middleware and the entry point.

mod domain;
mod service;
mod store;

use axum::extract::{Extension, MatchedPath, Path, Query, Request, State};
use axum::http::{HeaderValue, StatusCode};
use axum::middleware::{self, Next};
use axum::response::{IntoResponse, Response};
use axum::routing::{delete, get, patch, post};
use axum::{Json, Router};
use domain::{
    bad_request, fail, invalid, not_found, AppError, Ctx, Out, Task, User, COMMENT_SORTS,
    DEFAULT_LIMIT, GROUP_BYS, MAX_LIMIT, PORT, PROJECT_SORTS, SEQ_SORTS, STATUSES, TASK_SORTS,
    USER_SORTS,
};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};
use store::Store;

type Shared = State<Arc<Mutex<Store>>>;

fn nanos() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos()
}

// ------------------------------------------------------------------- middleware

fn header(request: &Request, name: &str) -> Option<String> {
    request
        .headers()
        .get(name)
        .and_then(|value| value.to_str().ok())
        .map(str::to_string)
}

async fn observe(State(shared): Shared, mut request: Request, next: Next) -> Response {
    let ctx = Ctx {
        request_id: header(&request, "x-request-id")
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| format!("{:x}", nanos())),
        token: header(&request, "authorization")
            .and_then(|value| value.strip_prefix("Bearer ").map(str::to_string))
            .unwrap_or_default(),
        key: header(&request, "idempotency-key"),
        if_match: header(&request, "if-match"),
        out: Arc::new(Mutex::new(Out::default())),
    };
    let method = request.method().to_string();
    let path = request.uri().path().to_string();
    let route = request.extensions().get::<MatchedPath>().map_or_else(
        || "unmatched".to_string(),
        |matched| format!("{method} {}", matched.as_str()),
    );
    let before = shared.lock().unwrap().audit.len();
    let started = Instant::now();
    request.extensions_mut().insert(ctx.clone());
    let mut response = next.run(request).await;
    let status = response.status().as_u16();
    let audit_seq = {
        let mut store = shared.lock().unwrap();
        store.count_request(&route, status);
        store.audit.len() - before
    };
    let out = ctx.out.lock().unwrap();
    println!(
        "{}",
        json!({
            "level": if status >= 500 { "error" } else if status >= 400 { "warn" } else { "info" },
            "requestId": ctx.request_id,
            "method": method,
            "path": path,
            "status": status,
            "durationMs": started.elapsed().as_millis() as i64,
            "userId": out.user_id,
            "quotaRemaining": out.quota_remaining,
            "auditSeq": audit_seq,
        })
    );
    let headers = response.headers_mut();
    headers.insert(
        "x-request-id",
        HeaderValue::from_str(&ctx.request_id).unwrap(),
    );
    if let Some(remaining) = out.quota_remaining {
        headers.insert("x-quota-remaining", HeaderValue::from(remaining));
    }
    if out.replayed {
        headers.insert("idempotency-replayed", HeaderValue::from_static("true"));
    }
    response
}

// ---------------------------------------------------------------------- helpers

/// Authenticate, then charge the quota. This order is fixed.
fn begin(ctx: &Ctx, store: &mut Store) -> Result<User, AppError> {
    let user = service::authenticate(ctx, store)?;
    service::charge_quota(ctx, store, &user)?;
    Ok(user)
}

fn begin_admin(ctx: &Ctx, store: &mut Store) -> Result<User, AppError> {
    let user = begin(ctx, store)?;
    service::require_admin(ctx, &user)?;
    Ok(user)
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

fn text(ctx: &Ctx, body: &Value, field: &str) -> Result<String, AppError> {
    match body.get(field) {
        None => Ok(String::new()),
        Some(Value::String(value)) => Ok(value.clone()),
        _ => Err(bad_request(ctx)),
    }
}

fn whole(
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
    let sort = query.get("sort").cloned().unwrap_or(allowed[0].to_string());
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

fn tagged(body: Value, version: i64) -> Response {
    ([("etag", version.to_string())], Json(body)).into_response()
}

/// A single-resource body carries its version, so the ETag comes for free.
fn responded(status: u16, body: Value) -> Response {
    let status = StatusCode::from_u16(status).unwrap();
    match body.get("version").and_then(Value::as_i64) {
        Some(version) => (status, [("etag", version.to_string())], Json(body)).into_response(),
        None => (status, Json(body)).into_response(),
    }
}

fn slot(ctx: &Ctx) -> Option<(String, String)> {
    ctx.key.clone().map(|key| (ctx.token.clone(), key))
}

/// Replay the outcome recorded against this Idempotency-Key, if there is one.
fn replay(ctx: &Ctx, store: &Store) -> Option<Response> {
    let (status, body) = store.idempotency.get(&slot(ctx)?)?.clone();
    ctx.out.lock().unwrap().replayed = true;
    Some(responded(status, body))
}

/// Record the outcome against the key, so a later request replays it.
fn settle(
    ctx: &Ctx,
    store: &mut Store,
    outcome: Result<(u16, Value), AppError>,
) -> Result<Response, AppError> {
    match outcome {
        Ok((status, body)) => {
            if let Some(slot) = slot(ctx) {
                store.idempotency.insert(slot, (status, body.clone()));
            }
            Ok(responded(status, body))
        }
        Err(error) => {
            if let Some(slot) = slot(ctx) {
                store
                    .idempotency
                    .insert(slot, (error.status.as_u16(), error.envelope()));
            }
            Err(error)
        }
    }
}

// ----------------------------------------------------------------- health, auth

async fn get_health(State(shared): Shared) -> Response {
    let store = shared.lock().unwrap();
    Json(json!({
        "status": "ok",
        "projects": store.projects.values().filter(|row| !row.deleted).count(),
        "tasks": store.tasks.values().filter(|row| !row.deleted).count(),
        "comments": store.comments.len(),
    }))
    .into_response()
}

async fn login(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let username = text(&ctx, &body, "username")?;
    let password = text(&ctx, &body, "password")?;
    if username.is_empty() {
        errors.push(fail("username", "username is required"));
    }
    if password.is_empty() {
        errors.push(fail("password", "password is required"));
    }
    if !errors.is_empty() {
        return Err(invalid(&ctx, errors));
    }
    let mut store = shared.lock().unwrap();
    let token = format!("{:x}{:016x}", store.sessions.len(), nanos());
    let user = service::login(&ctx, &mut store, &username, &password, &token)?;
    Ok(Json(json!({"token": token, "userId": user.id, "role": user.role})).into_response())
}

async fn logout(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin(&ctx, &mut store)?;
    store.sessions.remove(&ctx.token);
    Ok(StatusCode::NO_CONTENT.into_response())
}

async fn get_me(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    Ok(Json(json!({
        "userId": user.id,
        "username": user.username,
        "role": user.role,
    }))
    .into_response())
}

// ------------------------------------------------------------------------ users

async fn list_users(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &USER_SORTS)?;
    let rows: Vec<Value> = store
        .users
        .values()
        .filter(|user| !user.deleted)
        .map(service::serialize_user)
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

fn produce_user(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    raw: &str,
) -> Result<(u16, Value), AppError> {
    let body = read_body(ctx, raw)?;
    let user = service::create_user(
        ctx,
        store,
        actor,
        &text(ctx, &body, "username")?,
        &text(ctx, &body, "password")?,
        &body,
    )?;
    Ok((201, service::serialize_user(&user)))
}

async fn create_user(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    if let Some(hit) = replay(&ctx, &store) {
        return Ok(hit);
    }
    service::require_admin(&ctx, &actor)?;
    let outcome = produce_user(&ctx, &mut store, &actor, &body);
    settle(&ctx, &mut store, outcome)
}

async fn get_user(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    let Some(user) = store.find_user(parse_id(&ctx, &raw_id)?, false) else {
        return Err(not_found(&ctx));
    };
    Ok(tagged(service::serialize_user(&user), user.version))
}

async fn update_user(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin_admin(&ctx, &mut store)?;
    let Some(found) = store.find_user(parse_id(&ctx, &raw_id)?, false) else {
        return Err(not_found(&ctx));
    };
    service::check_if_match(&ctx, ctx.if_match.as_deref(), found.version)?;
    let body = read_body(&ctx, &body)?;
    let user = service::update_user(&ctx, &mut store, &actor, found.id, &body)?;
    Ok(tagged(service::serialize_user(&user), user.version))
}

async fn delete_user(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin_admin(&ctx, &mut store)?;
    let Some(found) = store.find_user(parse_id(&ctx, &raw_id)?, false) else {
        return Err(not_found(&ctx));
    };
    service::check_if_match(&ctx, ctx.if_match.as_deref(), found.version)?;
    let user = service::delete_user(&ctx, &mut store, &actor, found.id)?;
    Ok(tagged(service::serialize_user(&user), user.version))
}

// --------------------------------------------------------------------- projects

async fn list_projects(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let include = service::check_include_deleted(&ctx, query.get("includeDeleted"), &user)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &PROJECT_SORTS)?;
    let rows: Vec<Value> = service::visible_projects(&store, &user, include)
        .iter()
        .map(|project| service::serialize_project(project, &store))
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

fn produce_project(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    raw: &str,
) -> Result<(u16, Value), AppError> {
    let body = read_body(ctx, raw)?;
    let name = text(ctx, &body, "name")?;
    let owner_id = whole(ctx, &body, "ownerId", Some(actor.id))?;
    let project = service::create_project(ctx, store, actor, &name, owner_id)?;
    Ok((201, service::serialize_project(&project, store)))
}

async fn create_project(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    if let Some(hit) = replay(&ctx, &store) {
        return Ok(hit);
    }
    service::require_admin(&ctx, &actor)?;
    let outcome = produce_project(&ctx, &mut store, &actor, &body);
    settle(&ctx, &mut store, outcome)
}

async fn get_project(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &user, false)?;
    Ok(tagged(
        service::serialize_project(&project, &store),
        project.version,
    ))
}

async fn update_project(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin_admin(&ctx, &mut store)?;
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), project.version)?;
    let body = read_body(&ctx, &body)?;
    let project = match body.get("name") {
        None => project,
        Some(_) => {
            let name = text(&ctx, &body, "name")?;
            service::rename_project(&ctx, &mut store, &actor, &project, &name)?
        }
    };
    Ok(tagged(
        service::serialize_project(&project, &store),
        project.version,
    ))
}

async fn delete_project(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin_admin(&ctx, &mut store)?;
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), project.version)?;
    let project = service::delete_project(&mut store, &actor, project.id);
    Ok(tagged(
        service::serialize_project(&project, &store),
        project.version,
    ))
}

async fn restore_project(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin_admin(&ctx, &mut store)?;
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, true)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), project.version)?;
    let project = service::restore_project(&ctx, &mut store, &actor, &project)?;
    Ok(tagged(
        service::serialize_project(&project, &store),
        project.version,
    ))
}

// ------------------------------------------------------------------------ tasks

fn task_filters(
    ctx: &Ctx,
    query: &HashMap<String, String>,
    rows: Vec<Task>,
) -> Result<Vec<Task>, AppError> {
    let mut errors: Vec<Value> = vec![];
    let status = query.get("status").cloned();
    let raw_assignee = query.get("assigneeId");
    let assignee = raw_assignee.and_then(|raw| raw.parse::<i64>().ok());
    if status
        .as_deref()
        .is_some_and(|value| !STATUSES.contains(&value))
    {
        errors.push(fail("status", "status is not valid"));
    }
    if raw_assignee.is_some() && assignee.is_none() {
        errors.push(fail("assigneeId", "assigneeId is not a known user"));
    }
    if !errors.is_empty() {
        return Err(invalid(ctx, errors));
    }
    Ok(rows
        .into_iter()
        .filter(|task| status.as_deref().is_none_or(|value| task.status == value))
        .filter(|task| assignee.is_none_or(|id| task.assignee_id == Some(id)))
        .collect())
}

async fn list_all_tasks(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let include = service::check_include_deleted(&ctx, query.get("includeDeleted"), &user)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &TASK_SORTS)?;
    let rows: Vec<Value> = task_filters(&ctx, &query, service::visible_tasks(&store, &user, include))?
        .iter()
        .map(|task| service::serialize_task(task, &user.role))
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

async fn list_tasks(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &user, false)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &TASK_SORTS)?;
    let rows: Vec<Value> = store
        .live_tasks_of(project.id)
        .iter()
        .map(|task| service::serialize_task(task, &user.role))
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

fn produce_task(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    project_id: i64,
    raw: &str,
) -> Result<(u16, Value), AppError> {
    let body = read_body(ctx, raw)?;
    let mut errors: Vec<Value> = vec![];
    let note = service::read_note(ctx, actor, &body, &mut errors, "")?;
    let task = service::create_task(
        ctx,
        store,
        actor,
        project_id,
        &text(ctx, &body, "title")?,
        whole(ctx, &body, "priority", Some(0))?,
        whole(ctx, &body, "assigneeId", None)?,
        &note,
        errors,
    )?;
    Ok((201, service::serialize_task(&task, &actor.role)))
}

async fn create_task(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    if let Some(hit) = replay(&ctx, &store) {
        return Ok(hit);
    }
    let project = service::reachable_project(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    let outcome = produce_task(&ctx, &mut store, &actor, project.id, &body);
    settle(&ctx, &mut store, outcome)
}

async fn get_task(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &user, false)?;
    Ok(tagged(
        service::serialize_task(&task, &user.role),
        task.version,
    ))
}

async fn replace_task(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), task.version)?;
    let body = read_body(&ctx, &body)?;
    let mut errors: Vec<Value> = vec![];
    let note = service::read_note(&ctx, &actor, &body, &mut errors, &task.internal_note)?;
    let task = service::replace_task(
        &ctx,
        &mut store,
        &actor,
        task.id,
        &text(&ctx, &body, "title")?,
        whole(&ctx, &body, "priority", Some(0))?,
        whole(&ctx, &body, "assigneeId", None)?,
        &note,
        errors,
    )?;
    Ok(tagged(
        service::serialize_task(&task, &actor.role),
        task.version,
    ))
}

async fn update_status(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), task.version)?;
    let body = read_body(&ctx, &body)?;
    let task = service::move_status(&ctx, &mut store, &actor, &task, body.get("status"))?;
    Ok(tagged(
        service::serialize_task(&task, &actor.role),
        task.version,
    ))
}

async fn delete_task(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), task.version)?;
    let task = service::delete_task(&mut store, &actor, task.id);
    Ok(tagged(
        service::serialize_task(&task, &actor.role),
        task.version,
    ))
}

async fn restore_task(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, true)?;
    service::check_if_match(&ctx, ctx.if_match.as_deref(), task.version)?;
    let task = service::restore_task(&ctx, &mut store, &actor, &task)?;
    Ok(tagged(
        service::serialize_task(&task, &actor.role),
        task.version,
    ))
}

/// The `version` inside an item stands in for the `If-Match` header.
fn item_version(item: &Value) -> String {
    match item.get("version") {
        Some(Value::Number(number)) => number.to_string(),
        Some(Value::String(value)) => value.clone(),
        _ => "-".to_string(),
    }
}

fn apply_bulk(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    item: &Value,
) -> Result<(u16, i64), AppError> {
    if !item.is_object() {
        return Err(bad_request(ctx));
    }
    match item.get("op").and_then(Value::as_str) {
        Some("create") => {
            let project_id = whole(ctx, item, "projectId", Some(0))?.unwrap_or_default();
            let project = service::reachable_project(ctx, store, project_id, actor, false)?;
            let task = service::create_task(
                ctx,
                store,
                actor,
                project.id,
                &text(ctx, item, "title")?,
                whole(ctx, item, "priority", Some(0))?,
                None,
                "",
                vec![],
            )?;
            Ok((201, task.id))
        }
        Some("status") => {
            let task_id = whole(ctx, item, "id", Some(0))?.unwrap_or_default();
            let task = service::reachable_task(ctx, store, task_id, actor, false)?;
            service::check_if_match(ctx, Some(&item_version(item)), task.version)?;
            let task = service::move_status(ctx, store, actor, &task, item.get("status"))?;
            Ok((200, task.id))
        }
        Some("delete") => {
            let task_id = whole(ctx, item, "id", Some(0))?.unwrap_or_default();
            let task = service::reachable_task(ctx, store, task_id, actor, false)?;
            service::check_if_match(ctx, Some(&item_version(item)), task.version)?;
            let task = service::delete_task(store, actor, task.id);
            Ok((200, task.id))
        }
        _ => Err(invalid(ctx, vec![fail("op", "op is not valid")])),
    }
}

async fn bulk_tasks(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let body = read_body(&ctx, &body)?;
    service::check_bulk_size(&ctx, body.get("operations"))?;
    let operations = body["operations"].as_array().cloned().unwrap_or_default();
    let mut results: Vec<Value> = vec![];
    for (index, item) in operations.iter().enumerate() {
        results.push(match apply_bulk(&ctx, &mut store, &actor, item) {
            Ok((status, id)) => json!({"index": index, "status": status, "id": id, "error": null}),
            Err(error) => json!({"index": index, "status": error.status.as_u16(),
                                 "id": null, "error": error.code}),
        });
    }
    Ok(Json(json!({"results": results})).into_response())
}

// --------------------------------------------------------------------- comments

async fn list_comments(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &user, false)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &COMMENT_SORTS)?;
    let rows: Vec<Value> = store
        .comments
        .values()
        .filter(|comment| comment.task_id == task.id)
        .map(service::serialize_comment)
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

fn produce_comment(
    ctx: &Ctx,
    store: &mut Store,
    actor: &User,
    task_id: i64,
    raw: &str,
) -> Result<(u16, Value), AppError> {
    let body = read_body(ctx, raw)?;
    let comment =
        service::create_comment(ctx, store, actor, task_id, &text(ctx, &body, "body")?)?;
    Ok((201, service::serialize_comment(&comment)))
}

async fn create_comment(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
    body: String,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    if let Some(hit) = replay(&ctx, &store) {
        return Ok(hit);
    }
    let task = service::reachable_task(&ctx, &store, parse_id(&ctx, &raw_id)?, &actor, false)?;
    let outcome = produce_comment(&ctx, &mut store, &actor, task.id, &body);
    settle(&ctx, &mut store, outcome)
}

async fn delete_comment(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Path(raw_id): Path<String>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let actor = begin(&ctx, &mut store)?;
    let Some(comment) = store.find_comment(parse_id(&ctx, &raw_id)?) else {
        return Err(not_found(&ctx));
    };
    service::reachable_task(&ctx, &store, comment.task_id, &actor, true)?;
    service::remove_comment(&ctx, &mut store, &actor, &comment)?;
    Ok(StatusCode::NO_CONTENT.into_response())
}

// ---------------------------------------------------- search, reports, telemetry

async fn search(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let needle = query.get("q").cloned().unwrap_or_default();
    if needle.is_empty() {
        return Err(invalid(&ctx, vec![fail("q", "q is required")]));
    }
    Ok(Json(service::search(&store, &user, &needle)).into_response())
}

async fn workload(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    let user = begin(&ctx, &mut store)?;
    let group_by = query
        .get("groupBy")
        .cloned()
        .unwrap_or("status".to_string());
    if !GROUP_BYS.contains(&group_by.as_str()) {
        return Err(invalid(&ctx, vec![fail("groupBy", "groupBy is not valid")]));
    }
    Ok(Json(service::workload(&store, &user, &group_by)).into_response())
}

async fn list_audit(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &SEQ_SORTS)?;
    let rows: Vec<Value> = store
        .audit
        .iter()
        .filter(|entry| {
            query
                .get("actorId")
                .is_none_or(|want| entry.actor_id.to_string() == *want)
                && query.get("resource").is_none_or(|want| entry.resource == *want)
                && query.get("action").is_none_or(|want| entry.action == *want)
        })
        .map(service::serialize_audit)
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

async fn list_outbox(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    let (limit, offset, sort, order) = read_page(&ctx, &query, &SEQ_SORTS)?;
    let rows: Vec<Value> = store
        .outbox
        .iter()
        .filter(|event| {
            query
                .get("delivered")
                .is_none_or(|want| event.delivered == (want == "true"))
        })
        .map(service::serialize_outbox)
        .collect();
    Ok(Json(service::paginate(rows, limit, offset, &sort, &order)).into_response())
}

async fn flush_outbox(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    Ok(Json(json!({"flushed": service::flush_outbox(&mut store)})).into_response())
}

async fn get_metrics(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    Ok(Json(service::metrics(&store)).into_response())
}

async fn get_stats(
    State(shared): Shared,
    Extension(ctx): Extension<Ctx>,
) -> Result<Response, AppError> {
    let mut store = shared.lock().unwrap();
    begin_admin(&ctx, &mut store)?;
    Ok(Json(service::stats(&store)).into_response())
}

async fn fallback(Extension(ctx): Extension<Ctx>) -> AppError {
    not_found(&ctx)
}

#[tokio::main]
async fn main() {
    let shared = Arc::new(Mutex::new(Store::seed()));
    let app = Router::new()
        .route("/health", get(get_health))
        .route("/auth/login", post(login))
        .route("/auth/logout", post(logout))
        .route("/me", get(get_me))
        .route("/users", get(list_users).post(create_user))
        .route(
            "/users/{id}",
            get(get_user).patch(update_user).delete(delete_user),
        )
        .route("/projects", get(list_projects).post(create_project))
        .route(
            "/projects/{id}",
            get(get_project).patch(update_project).delete(delete_project),
        )
        .route("/projects/{id}/restore", post(restore_project))
        .route("/projects/{id}/tasks", get(list_tasks).post(create_task))
        .route("/tasks", get(list_all_tasks))
        .route("/tasks/bulk", post(bulk_tasks))
        .route(
            "/tasks/{id}",
            get(get_task).put(replace_task).delete(delete_task),
        )
        .route("/tasks/{id}/status", patch(update_status))
        .route("/tasks/{id}/restore", post(restore_task))
        .route(
            "/tasks/{id}/comments",
            get(list_comments).post(create_comment),
        )
        .route("/comments/{id}", delete(delete_comment))
        .route("/search", get(search))
        .route("/reports/workload", get(workload))
        .route("/audit", get(list_audit))
        .route("/outbox", get(list_outbox))
        .route("/outbox/flush", post(flush_outbox))
        .route("/metrics", get(get_metrics))
        .route("/stats", get(get_stats))
        .fallback(fallback)
        .layer(middleware::from_fn_with_state(shared.clone(), observe))
        .with_state(shared);
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", PORT))
        .await
        .unwrap();
    axum::serve(listener, app).await.unwrap();
}
