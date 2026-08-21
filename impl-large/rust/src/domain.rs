// Task Service, large tier — domain types, constants and pure rules.

use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};

pub const MAX_TITLE_LENGTH: usize = 80;
pub const MAX_NAME_LENGTH: usize = 60;
pub const MAX_COMMENT_LENGTH: usize = 200;
pub const MAX_BULK_ITEMS: usize = 20;
pub const MIN_PRIORITY: i64 = 1;
pub const MAX_PRIORITY: i64 = 5;
pub const DEFAULT_LIMIT: i64 = 20;
pub const MAX_LIMIT: i64 = 100;
pub const DEFAULT_QUOTA: i64 = 10000;
pub const PROBE_QUOTA: i64 = 5;
pub const PORT: u16 = 8080;

pub const ROLES: [&str; 2] = ["admin", "user"];
pub const STATUSES: [&str; 4] = ["todo", "in_progress", "done", "archived"];
pub const STATUS_BONUS: [(&str, i64); 4] =
    [("todo", 0), ("in_progress", 3), ("done", 5), ("archived", 0)];
pub const TRANSITIONS: [(&str, &str); 5] = [
    ("todo", "in_progress"),
    ("todo", "archived"),
    ("in_progress", "todo"),
    ("in_progress", "done"),
    ("done", "archived"),
];
pub const PROJECT_SORTS: [&str; 3] = ["id", "name", "taskCount"];
pub const TASK_SORTS: [&str; 5] = ["id", "title", "priority", "score", "status"];
pub const USER_SORTS: [&str; 3] = ["id", "username", "role"];
pub const COMMENT_SORTS: [&str; 2] = ["id", "authorId"];
pub const SEQ_SORTS: [&str; 1] = ["seq"];
pub const GROUP_BYS: [&str; 3] = ["assignee", "status", "project"];

#[derive(Clone)]
pub struct User {
    pub id: i64,
    pub username: String,
    pub password: String,
    pub role: String,
    pub quota: i64,
    pub version: i64,
    pub deleted: bool,
}

#[derive(Clone)]
pub struct Session {
    pub user_id: i64,
    pub used: i64,
}

#[derive(Clone)]
pub struct Project {
    pub id: i64,
    pub name: String,
    pub owner_id: i64,
    pub version: i64,
    pub deleted: bool,
}

#[derive(Clone)]
pub struct Task {
    pub id: i64,
    pub project_id: i64,
    pub title: String,
    pub priority: i64,
    pub status: String,
    pub assignee_id: Option<i64>,
    pub internal_note: String,
    pub version: i64,
    pub deleted: bool,
}

#[derive(Clone)]
pub struct Comment {
    pub id: i64,
    pub task_id: i64,
    pub author_id: i64,
    pub body: String,
}

#[derive(Clone)]
pub struct AuditEntry {
    pub seq: i64,
    pub actor_id: i64,
    pub action: String,
    pub resource: String,
    pub resource_id: i64,
}

#[derive(Clone)]
pub struct OutboxEvent {
    pub seq: i64,
    pub name: String,
    pub resource_id: i64,
    pub delivered: bool,
}

/// What the handler tells the middleware to log and to place in the headers.
#[derive(Default)]
pub struct Out {
    pub user_id: Option<i64>,
    pub quota_remaining: Option<i64>,
    pub replayed: bool,
}

/// The per-request context that every layer reads.
#[derive(Clone)]
pub struct Ctx {
    pub request_id: String,
    pub token: String,
    pub key: Option<String>,
    pub if_match: Option<String>,
    pub out: Arc<Mutex<Out>>,
}

/// Every failure path returns this. `IntoResponse` turns it into the envelope.
pub struct AppError {
    pub status: StatusCode,
    pub code: &'static str,
    pub message: &'static str,
    pub request_id: String,
    pub details: Vec<Value>,
}

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

    pub fn envelope(&self) -> Value {
        json!({ "error": {
            "code": self.code,
            "message": self.message,
            "requestId": self.request_id,
            "details": self.details,
        }})
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        (self.status, Json(self.envelope())).into_response()
    }
}

macro_rules! failures {
    ($($name:ident, $status:ident, $code:literal, $message:literal;)*) => { $(
        pub fn $name(ctx: &Ctx) -> AppError {
            AppError::new(ctx, StatusCode::$status, $code, $message, vec![])
        }
    )* };
}

failures! {
    bad_request, BAD_REQUEST, "bad_request", "the request is malformed";
    unauthorized, UNAUTHORIZED, "unauthorized", "authentication is required";
    invalid_credentials, UNAUTHORIZED, "invalid_credentials", "the username or password is wrong";
    forbidden, FORBIDDEN, "forbidden", "you may not access this resource";
    not_found, NOT_FOUND, "not_found", "the resource does not exist";
    conflict, CONFLICT, "conflict", "the resource already exists";
    invalid_transition, CONFLICT, "invalid_transition", "the status change is not allowed";
    precondition_failed, PRECONDITION_FAILED, "precondition_failed", "the resource has changed";
    precondition_required, PRECONDITION_REQUIRED, "precondition_required",
        "the If-Match header is required";
    quota_exceeded, TOO_MANY_REQUESTS, "quota_exceeded", "the request quota is exhausted";
}

pub fn invalid(ctx: &Ctx, mut details: Vec<Value>) -> AppError {
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

pub fn fail(field: &str, message: &str) -> Value {
    json!({ "field": field, "message": message })
}

pub fn compute_score(priority: i64, status: &str) -> i64 {
    let base_score = priority * 10;
    let bonus = STATUS_BONUS
        .iter()
        .find(|(name, _)| *name == status)
        .map_or(0, |(_, bonus)| *bonus);
    base_score + bonus
}

pub fn check_string(value: &str, field: &str, max_length: usize, errors: &mut Vec<Value>) {
    if value.is_empty() {
        errors.push(fail(field, &format!("{field} is required")));
    } else if value.chars().count() > max_length {
        errors.push(fail(field, &format!("{field} is too long")));
    }
}

pub fn check_priority(value: Option<i64>, errors: &mut Vec<Value>) {
    if !matches!(value, Some(number) if (MIN_PRIORITY..=MAX_PRIORITY).contains(&number)) {
        errors.push(fail("priority", "priority is out of range"));
    }
}

pub fn check_status(value: Option<&Value>, errors: &mut Vec<Value>) {
    if !value
        .and_then(Value::as_str)
        .is_some_and(|status| STATUSES.contains(&status))
    {
        errors.push(fail("status", "status is not valid"));
    }
}

pub fn check_role(value: &Value, errors: &mut Vec<Value>) {
    if !value.as_str().is_some_and(|role| ROLES.contains(&role)) {
        errors.push(fail("role", "role is not valid"));
    }
}

pub fn check_quota(value: &Value, errors: &mut Vec<Value>) {
    if !value.as_i64().is_some_and(|quota| quota >= 0) {
        errors.push(fail("quota", "quota is out of range"));
    }
}
