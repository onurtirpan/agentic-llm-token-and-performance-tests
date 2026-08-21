// Task Service, large tier — the in-memory state and its repositories.

import {
  DEFAULT_QUOTA,
  PROBE_QUOTA,
  type AuditEntry,
  type Comment,
  type OutboxEvent,
  type Project,
  type Row,
  type Session,
  type Task,
  type User,
} from "./domain.js";

export const users = new Map<number, User>();
export const sessions = new Map<string, Session>();
export const projects = new Map<number, Project>();
export const tasks = new Map<number, Task>();
export const comments = new Map<number, Comment>();
export const audit: AuditEntry[] = [];
export const outbox: OutboxEvent[] = [];
export const idempotency = new Map<string, [number, Row]>();
export const byStatus = new Map<number, number>();
export const byRoute = new Map<string, number>();

export let requests = 0;
let nextProjectId = 1;
let nextTaskId = 1;
let nextCommentId = 1;
let nextUserId = 5;
let nextSeq = 1;

export function seed(): void {
  const seeded: [number, string, string, string, number][] = [
    [1, "admin", "admin-secret", "admin", DEFAULT_QUOTA],
    [2, "alice", "alice-secret", "user", DEFAULT_QUOTA],
    [3, "bob", "bob-secret", "user", DEFAULT_QUOTA],
    [4, "probe", "probe-secret", "user", PROBE_QUOTA],
  ];
  for (const [id, username, password, role, quota] of seeded) {
    users.set(id, { id, username, password, role, quota, version: 1, deleted: false });
  }
}

export function takeSeq(): number {
  const value = nextSeq;
  nextSeq += 1;
  return value;
}

/** Append one audit entry and one outbox event for a successful write. */
export function record(actorId: number, action: string, resource: string, resourceId: number): void {
  audit.push({ seq: takeSeq(), actorId, action, resource, resourceId });
  outbox.push({ seq: takeSeq(), name: `${resource}.${action}`, resourceId, delivered: false });
}

export function countRequest(route: string, status: number): void {
  requests += 1;
  byRoute.set(route, (byRoute.get(route) ?? 0) + 1);
  byStatus.set(status, (byStatus.get(status) ?? 0) + 1);
}

export function findUser(userId: number | null, includeDeleted = false): User | undefined {
  const user = userId === null ? undefined : users.get(userId);
  if (user === undefined || (user.deleted && !includeDeleted)) return undefined;
  return user;
}

export function findByUsername(username: string): User | undefined {
  return [...users.values()].find((u) => u.username === username && !u.deleted);
}

export function insertUser(username: string, password: string, role: string, quota: number): User {
  const user: User = { id: nextUserId, username, password, role, quota, version: 1, deleted: false };
  users.set(user.id, user);
  nextUserId += 1;
  return user;
}

export function findProject(projectId: number | null, includeDeleted = false): Project | undefined {
  const project = projectId === null ? undefined : projects.get(projectId);
  if (project === undefined || (project.deleted && !includeDeleted)) return undefined;
  return project;
}

export function insertProject(name: string, ownerId: number): Project {
  const project: Project = { id: nextProjectId, name, ownerId, version: 1, deleted: false };
  projects.set(project.id, project);
  nextProjectId += 1;
  return project;
}

export function findTask(taskId: number | null, includeDeleted = false): Task | undefined {
  const task = taskId === null ? undefined : tasks.get(taskId);
  if (task === undefined || (task.deleted && !includeDeleted)) return undefined;
  return task;
}

export function insertTask(
  projectId: number,
  title: string,
  priority: number,
  assigneeId: number | null,
  internalNote: string,
): Task {
  const task: Task = {
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

export function findComment(commentId: number): Comment | undefined {
  return comments.get(commentId);
}

export function insertComment(taskId: number, authorId: number, body: string): Comment {
  const comment: Comment = { id: nextCommentId, taskId, authorId, body };
  comments.set(comment.id, comment);
  nextCommentId += 1;
  return comment;
}

export function liveTasksOf(projectId: number): Task[] {
  return [...tasks.values()].filter((t) => t.projectId === projectId && !t.deleted);
}

export function taskCount(projectId: number): number {
  return liveTasksOf(projectId).length;
}

export function outboxPending(): number {
  return outbox.filter((event) => !event.delivered).length;
}
