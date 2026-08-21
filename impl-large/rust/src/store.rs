// Task Service, large tier — the in-memory state and its repositories.

use crate::domain::{
    AuditEntry, Comment, OutboxEvent, Project, Session, Task, User, DEFAULT_QUOTA, PROBE_QUOTA,
};
use serde_json::Value;
use std::collections::BTreeMap;

pub struct Store {
    pub users: BTreeMap<i64, User>,
    pub sessions: BTreeMap<String, Session>,
    pub projects: BTreeMap<i64, Project>,
    pub tasks: BTreeMap<i64, Task>,
    pub comments: BTreeMap<i64, Comment>,
    pub audit: Vec<AuditEntry>,
    pub outbox: Vec<OutboxEvent>,
    pub idempotency: BTreeMap<(String, String), (u16, Value)>,
    pub by_status: BTreeMap<u16, i64>,
    pub by_route: BTreeMap<String, i64>,
    pub requests: i64,
    pub next_project_id: i64,
    pub next_task_id: i64,
    pub next_comment_id: i64,
    pub next_user_id: i64,
    pub next_seq: i64,
}

fn seed_user(id: i64, username: &str, password: &str, role: &str, quota: i64) -> (i64, User) {
    (
        id,
        User {
            id,
            username: username.to_string(),
            password: password.to_string(),
            role: role.to_string(),
            quota,
            version: 1,
            deleted: false,
        },
    )
}

impl Store {
    pub fn seed() -> Store {
        Store {
            users: BTreeMap::from([
                seed_user(1, "admin", "admin-secret", "admin", DEFAULT_QUOTA),
                seed_user(2, "alice", "alice-secret", "user", DEFAULT_QUOTA),
                seed_user(3, "bob", "bob-secret", "user", DEFAULT_QUOTA),
                seed_user(4, "probe", "probe-secret", "user", PROBE_QUOTA),
            ]),
            sessions: BTreeMap::new(),
            projects: BTreeMap::new(),
            tasks: BTreeMap::new(),
            comments: BTreeMap::new(),
            audit: vec![],
            outbox: vec![],
            idempotency: BTreeMap::new(),
            by_status: BTreeMap::new(),
            by_route: BTreeMap::new(),
            requests: 0,
            next_project_id: 1,
            next_task_id: 1,
            next_comment_id: 1,
            next_user_id: 5,
            next_seq: 1,
        }
    }

    pub fn take_seq(&mut self) -> i64 {
        let value = self.next_seq;
        self.next_seq += 1;
        value
    }

    /// Append one audit entry and one outbox event for a successful write.
    pub fn record(&mut self, actor_id: i64, action: &str, resource: &str, resource_id: i64) {
        let seq = self.take_seq();
        self.audit.push(AuditEntry {
            seq,
            actor_id,
            action: action.to_string(),
            resource: resource.to_string(),
            resource_id,
        });
        let seq = self.take_seq();
        self.outbox.push(OutboxEvent {
            seq,
            name: format!("{resource}.{action}"),
            resource_id,
            delivered: false,
        });
    }

    pub fn count_request(&mut self, route: &str, status: u16) {
        self.requests += 1;
        *self.by_route.entry(route.to_string()).or_insert(0) += 1;
        *self.by_status.entry(status).or_insert(0) += 1;
    }

    pub fn find_user(&self, user_id: i64, include_deleted: bool) -> Option<User> {
        self.users
            .get(&user_id)
            .filter(|user| include_deleted || !user.deleted)
            .cloned()
    }

    pub fn find_by_username(&self, username: &str) -> Option<User> {
        self.users
            .values()
            .find(|user| user.username == username && !user.deleted)
            .cloned()
    }

    pub fn insert_user(&mut self, username: &str, password: &str, role: &str, quota: i64) -> User {
        let (_, user) = seed_user(self.next_user_id, username, password, role, quota);
        self.users.insert(user.id, user.clone());
        self.next_user_id += 1;
        user
    }

    pub fn find_project(&self, project_id: i64, include_deleted: bool) -> Option<Project> {
        self.projects
            .get(&project_id)
            .filter(|project| include_deleted || !project.deleted)
            .cloned()
    }

    pub fn insert_project(&mut self, name: &str, owner_id: i64) -> Project {
        let project = Project {
            id: self.next_project_id,
            name: name.to_string(),
            owner_id,
            version: 1,
            deleted: false,
        };
        self.projects.insert(project.id, project.clone());
        self.next_project_id += 1;
        project
    }

    pub fn find_task(&self, task_id: i64, include_deleted: bool) -> Option<Task> {
        self.tasks
            .get(&task_id)
            .filter(|task| include_deleted || !task.deleted)
            .cloned()
    }

    pub fn insert_task(
        &mut self,
        project_id: i64,
        title: &str,
        priority: i64,
        assignee_id: Option<i64>,
        internal_note: &str,
    ) -> Task {
        let task = Task {
            id: self.next_task_id,
            project_id,
            title: title.to_string(),
            priority,
            status: "todo".to_string(),
            assignee_id,
            internal_note: internal_note.to_string(),
            version: 1,
            deleted: false,
        };
        self.tasks.insert(task.id, task.clone());
        self.next_task_id += 1;
        task
    }

    pub fn find_comment(&self, comment_id: i64) -> Option<Comment> {
        self.comments.get(&comment_id).cloned()
    }

    pub fn insert_comment(&mut self, task_id: i64, author_id: i64, body: &str) -> Comment {
        let comment = Comment {
            id: self.next_comment_id,
            task_id,
            author_id,
            body: body.to_string(),
        };
        self.comments.insert(comment.id, comment.clone());
        self.next_comment_id += 1;
        comment
    }

    pub fn live_tasks_of(&self, project_id: i64) -> Vec<Task> {
        self.tasks
            .values()
            .filter(|task| task.project_id == project_id && !task.deleted)
            .cloned()
            .collect()
    }

    pub fn task_count(&self, project_id: i64) -> usize {
        self.live_tasks_of(project_id).len()
    }

    pub fn outbox_pending(&self) -> usize {
        self.outbox.iter().filter(|event| !event.delivered).count()
    }
}
