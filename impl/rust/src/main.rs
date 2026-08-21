// Task Service — Axum implementation.

use axum::extract::{Path, Query, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::{BTreeMap, HashMap};
use std::sync::{Arc, Mutex};

const MAX_TITLE_LENGTH: usize = 80;
const MIN_PRIORITY: i64 = 1;
const MAX_PRIORITY: i64 = 5;
const PORT: u16 = 8080;

#[derive(Clone, Serialize)]
struct Task {
    id: i64,
    title: String,
    priority: i64,
    done: bool,
    score: i64,
}

#[derive(Deserialize)]
struct TaskInput {
    #[serde(default)]
    title: String,
    #[serde(default)]
    priority: i64,
    #[serde(default)]
    done: bool,
}

struct Store {
    tasks: BTreeMap<i64, Task>,
    next_id: i64,
}

type Shared = State<Arc<Mutex<Store>>>;

fn compute_score(priority: i64, done: bool) -> i64 {
    let base_score = priority * 10;
    if done {
        base_score
    } else {
        base_score + 5
    }
}

fn validate(title: &str, priority: i64) -> Option<&'static str> {
    if title.is_empty() {
        return Some("title is required");
    }
    if title.chars().count() > MAX_TITLE_LENGTH {
        return Some("title is too long");
    }
    if priority < MIN_PRIORITY || priority > MAX_PRIORITY {
        return Some("priority is out of range");
    }
    None
}

fn fail(status: StatusCode, message: &str) -> Response {
    (status, Json(json!({ "error": message }))).into_response()
}

async fn get_health(State(store): Shared) -> Response {
    let store = store.lock().unwrap();
    Json(json!({ "status": "ok", "count": store.tasks.len() })).into_response()
}

async fn list_tasks(State(store): Shared, Query(query): Query<HashMap<String, String>>) -> Response {
    let filter = query.get("done");
    if let Some(value) = filter {
        if value != "true" && value != "false" {
            return fail(StatusCode::BAD_REQUEST, "done must be true or false");
        }
    }
    let store = store.lock().unwrap();
    let mut selected: Vec<Task> = store
        .tasks
        .values()
        .filter(|task| filter.is_none_or(|value| task.done == (value == "true")))
        .cloned()
        .collect();
    selected.sort_by(|a, b| b.score.cmp(&a.score).then(a.id.cmp(&b.id)));
    let total = selected.len();
    Json(json!({ "tasks": selected, "total": total })).into_response()
}

async fn get_task(State(store): Shared, Path(id): Path<String>) -> Response {
    let Ok(task_id) = id.parse::<i64>() else {
        return fail(StatusCode::BAD_REQUEST, "invalid id");
    };
    let store = store.lock().unwrap();
    match store.tasks.get(&task_id) {
        Some(task) => Json(task.clone()).into_response(),
        None => fail(StatusCode::NOT_FOUND, "task not found"),
    }
}

async fn create_task(State(store): Shared, body: String) -> Response {
    let Ok(input) = serde_json::from_str::<TaskInput>(&body) else {
        return fail(StatusCode::BAD_REQUEST, "invalid json");
    };
    if let Some(error) = validate(&input.title, input.priority) {
        return fail(StatusCode::BAD_REQUEST, error);
    }
    let mut store = store.lock().unwrap();
    let task = Task {
        id: store.next_id,
        title: input.title,
        priority: input.priority,
        done: false,
        score: compute_score(input.priority, false),
    };
    store.tasks.insert(task.id, task.clone());
    store.next_id += 1;
    (StatusCode::CREATED, Json(task)).into_response()
}

async fn update_task(State(store): Shared, Path(id): Path<String>, body: String) -> Response {
    let Ok(task_id) = id.parse::<i64>() else {
        return fail(StatusCode::BAD_REQUEST, "invalid id");
    };
    let mut store = store.lock().unwrap();
    if !store.tasks.contains_key(&task_id) {
        return fail(StatusCode::NOT_FOUND, "task not found");
    }
    let Ok(input) = serde_json::from_str::<TaskInput>(&body) else {
        return fail(StatusCode::BAD_REQUEST, "invalid json");
    };
    if let Some(error) = validate(&input.title, input.priority) {
        return fail(StatusCode::BAD_REQUEST, error);
    }
    let task = Task {
        id: task_id,
        title: input.title,
        priority: input.priority,
        done: input.done,
        score: compute_score(input.priority, input.done),
    };
    store.tasks.insert(task_id, task.clone());
    Json(task).into_response()
}

async fn delete_task(State(store): Shared, Path(id): Path<String>) -> Response {
    let Ok(task_id) = id.parse::<i64>() else {
        return fail(StatusCode::BAD_REQUEST, "invalid id");
    };
    let mut store = store.lock().unwrap();
    if store.tasks.remove(&task_id).is_none() {
        return fail(StatusCode::NOT_FOUND, "task not found");
    }
    StatusCode::NO_CONTENT.into_response()
}

async fn get_stats(State(store): Shared) -> Response {
    let store = store.lock().unwrap();
    let total = store.tasks.len() as i64;
    let done_count = store.tasks.values().filter(|task| task.done).count() as i64;
    let sum_score: i64 = store.tasks.values().map(|task| task.score).sum();
    let avg_score = if total == 0 {
        0.0
    } else {
        (sum_score as f64 / total as f64 * 100.0).round() / 100.0
    };
    let mut best: Option<&Task> = None;
    for task in store.tasks.values() {
        if !task.done && best.is_none_or(|found| task.priority > found.priority) {
            best = Some(task);
        }
    }
    Json(json!({
        "total": total,
        "doneCount": done_count,
        "openCount": total - done_count,
        "avgScore": avg_score,
        "topOpenTitle": best.map(|task| task.title.clone()),
    }))
    .into_response()
}

async fn fallback() -> Response {
    fail(StatusCode::NOT_FOUND, "not found")
}

#[tokio::main]
async fn main() {
    let store = Arc::new(Mutex::new(Store {
        tasks: BTreeMap::new(),
        next_id: 1,
    }));
    let app = Router::new()
        .route("/health", get(get_health))
        .route("/tasks", get(list_tasks).post(create_task))
        .route(
            "/tasks/{id}",
            get(get_task).put(update_task).delete(delete_task),
        )
        .route("/stats", get(get_stats))
        .fallback(fallback)
        .with_state(store);
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", PORT))
        .await
        .unwrap();
    axum::serve(listener, app).await.unwrap();
}
