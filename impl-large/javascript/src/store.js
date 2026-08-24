// Task Service, large tier — the in-memory state and its repositories.

import { DEFAULT_QUOTA, PROBE_QUOTA } from "./domain.js";

export const users = new Map();
export const sessions = new Map();
export const projects = new Map();
export const tasks = new Map();
export const comments = new Map();
export const audit = [];
export const outbox = [];
export const idempotency = new Map();
export const byStatus = new Map();
export const byRoute = new Map();

export let requests = 0;
let nextProjectId = 1;
let nextTaskId = 1;
let nextCommentId = 1;
let nextUserId = 5;
let nextSeq = 1;

export function seed() {
  const seeded = [
    [1, "admin", "admin-secret", "admin", DEFAULT_QUOTA],
    [2, "alice", "alice-secret", "user", DEFAULT_QUOTA],
    [3, "bob", "bob-secret", "user", DEFAULT_QUOTA],
    [4, "probe", "probe-secret", "user", PROBE_QUOTA],
  ];
  for (const [id, username, password, role, quota] of seeded) {
    users.set(id, { id, username, password, role, quota, version: 1, deleted: false });
  }
}

export function takeSeq() {
  const value = nextSeq;
  nextSeq += 1;
  return value;
}

/** Append one audit entry and one outbox event for a successful write. */
export function record(actorId, action, resource, resourceId) {
  audit.push({ seq: takeSeq(), actorId, action, resource, resourceId });
  outbox.push({ seq: takeSeq(), name: `${resource}.${action}`, resourceId, delivered: false });
}

export function countRequest(route, status) {
  requests += 1;
  byRoute.set(route, (byRoute.get(route) ?? 0) + 1);
  byStatus.set(status, (byStatus.get(status) ?? 0) + 1);
}

export function findUser(userId, includeDeleted = false) {
  const user = userId === null ? undefined : users.get(userId);
  if (user === undefined || (user.deleted && !includeDeleted)) return undefined;
  return user;
}

export function findByUsername(username) {
  return [...users.values()].find((u) => u.username === username && !u.deleted);
}

export function insertUser(username, password, role, quota) {
  const user = { id: nextUserId, username, password, role, quota, version: 1, deleted: false };
  users.set(user.id, user);
  nextUserId += 1;
  return user;
}

export function findProject(projectId, includeDeleted = false) {
  const project = projectId === null ? undefined : projects.get(projectId);
  if (project === undefined || (project.deleted && !includeDeleted)) return undefined;
  return project;
}

export function insertProject(name, ownerId) {
  const project = { id: nextProjectId, name, ownerId, version: 1, deleted: false };
  projects.set(project.id, project);
  nextProjectId += 1;
  return project;
}

export function findTask(taskId, includeDeleted = false) {
  const task = taskId === null ? undefined : tasks.get(taskId);
  if (task === undefined || (task.deleted && !includeDeleted)) return undefined;
  return task;
}

export function insertTask(projectId, title, priority, assigneeId, internalNote) {
  const task = {
    id: nextTaskId,
    projectId,
    title,
    priority,
    status: "todo",
    assigneeId,
    internalNote,
    version: 1,
    deleted: false,
  };
  tasks.set(task.id, task);
  nextTaskId += 1;
  return task;
}

export function findComment(commentId) {
  return comments.get(commentId);
}

export function insertComment(taskId, authorId, body) {
  const comment = { id: nextCommentId, taskId, authorId, body };
  comments.set(comment.id, comment);
  nextCommentId += 1;
  return comment;
}

export function liveTasksOf(projectId) {
  return [...tasks.values()].filter((t) => t.projectId === projectId && !t.deleted);
}

export function taskCount(projectId) {
  return liveTasksOf(projectId).length;
}

export function outboxPending() {
  return outbox.filter((event) => !event.delivered).length;
}
