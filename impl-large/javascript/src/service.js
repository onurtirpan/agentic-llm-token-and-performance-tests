// Task Service, large tier — business rules, authorization and audit emission.

import {
  MAX_BULK_ITEMS,
  MAX_COMMENT_LENGTH,
  MAX_NAME_LENGTH,
  MAX_TITLE_LENGTH,
  STATUSES,
  TRANSITIONS,
  badRequest,
  checkPriority,
  checkQuota,
  checkRole,
  checkStatus,
  checkString,
  computeScore,
  conflict,
  fail,
  forbidden,
  invalid,
  invalidCredentials,
  invalidTransition,
  notFound,
  preconditionFailed,
  preconditionRequired,
  quotaExceeded,
  unauthorized,
} from "./domain.js";
import * as store from "./store.js";

// ------------------------------------------------------------------ serializers

export function serializeUser(user) {
  return {
    id: user.id,
    username: user.username,
    role: user.role,
    quota: user.quota,
    version: user.version,
    deleted: user.deleted,
  };
}

export function serializeProject(project) {
  return {
    id: project.id,
    name: project.name,
    ownerId: project.ownerId,
    taskCount: store.taskCount(project.id),
    version: project.version,
    deleted: project.deleted,
  };
}

export function serializeTask(task, role) {
  const body = {
    id: task.id,
    projectId: task.projectId,
    title: task.title,
    priority: task.priority,
    status: task.status,
    assigneeId: task.assigneeId,
  };
  if (role === "admin") body.internalNote = task.internalNote;
  body.version = task.version;
  body.deleted = task.deleted;
  body.score = computeScore(task.priority, task.status);
  return body;
}

export function serializeComment(comment) {
  return {
    id: comment.id,
    taskId: comment.taskId,
    authorId: comment.authorId,
    body: comment.body,
  };
}

export function serializeAudit(entry) {
  return {
    seq: entry.seq,
    actorId: entry.actorId,
    action: entry.action,
    resource: entry.resource,
    resourceId: entry.resourceId,
  };
}

export function serializeOutbox(event) {
  return {
    seq: event.seq,
    name: event.name,
    resourceId: event.resourceId,
    delivered: event.delivered,
  };
}

// ----------------------------------------------------------------- access rules

export function authenticate(header) {
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  const session = store.sessions.get(token);
  if (session === undefined) throw unauthorized();
  const user = store.findUser(session.userId);
  if (user === undefined) throw unauthorized();
  return [user, session];
}

export function chargeQuota(user, session) {
  if (session.used >= user.quota) throw quotaExceeded();
  session.used += 1;
  return Math.max(user.quota - session.used, 0);
}

export function requireAdmin(user) {
  if (user.role !== "admin") throw forbidden();
}

export function reachableProject(projectId, user, includeDeleted = false) {
  const project = store.findProject(projectId, includeDeleted);
  if (project === undefined) throw notFound();
  if (user.role !== "admin" && project.ownerId !== user.id) throw forbidden();
  return project;
}

export function reachableTask(taskId, user, includeDeleted = false) {
  const task = store.findTask(taskId, includeDeleted);
  if (task === undefined) throw notFound();
  reachableProject(task.projectId, user, true);
  return task;
}

export function checkIfMatch(header, version) {
  if (header === undefined || header === "") throw preconditionRequired();
  if (header !== String(version)) throw preconditionFailed();
}

export function checkIncludeDeleted(raw, user) {
  if (raw === undefined) return false;
  if (user.role !== "admin") throw forbidden();
  return raw === "true";
}

// ------------------------------------------------------------------- pagination

function compare(left, right) {
  const a = left;
  const b = right;
  return a < b ? -1 : a > b ? 1 : 0;
}

/** Sort by the tiebreak first, then stably by the requested field. */
export function paginate(rows, limit, offset, sort, order) {
  const tiebreak = rows.length > 0 && "seq" in rows[0] ? "seq" : "id";
  const direction = order === "desc" ? -1 : 1;
  rows.sort((a, b) => compare(a[tiebreak], b[tiebreak]));
  rows.sort((a, b) => direction * compare(a[sort], b[sort]));
  return { items: rows.slice(offset, offset + limit), total: rows.length, limit, offset };
}

// ------------------------------------------------------------------------- auth

export function login(username, password, token) {
  const user = store.findByUsername(username);
  if (user === undefined || user.password !== password) throw invalidCredentials();
  store.sessions.set(token, { token, userId: user.id, used: 0 });
  return user;
}

// --------------------------------------------------------------------- projects

export function createProject(actor, name, ownerId) {
  const errors = [];
  checkString(name, "name", MAX_NAME_LENGTH, errors);
  if (store.findUser(ownerId) === undefined) {
    errors.push(fail("ownerId", "ownerId is not a known user"));
  }
  if (errors.length > 0) throw invalid(errors);
  const taken = [...store.projects.values()].some(
    (p) => p.ownerId === ownerId && p.name === name && !p.deleted,
  );
  if (taken) throw conflict();
  const project = store.insertProject(name, ownerId);
  store.record(actor.id, "create", "project", project.id);
  return project;
}

export function renameProject(actor, project, name) {
  const errors = [];
  checkString(name, "name", MAX_NAME_LENGTH, errors);
  if (errors.length > 0) throw invalid(errors);
  const taken = [...store.projects.values()].some(
    (p) => p.ownerId === project.ownerId && p.name === name && p.id !== project.id && !p.deleted,
  );
  if (taken) throw conflict();
  project.name = name;
  project.version += 1;
  store.record(actor.id, "update", "project", project.id);
  return project;
}

export function deleteProject(actor, project) {
  project.deleted = true;
  project.version += 1;
  store.record(actor.id, "delete", "project", project.id);
  for (const task of store.liveTasksOf(project.id)) {
    task.deleted = true;
    task.version += 1;
    store.record(actor.id, "delete", "task", task.id);
  }
  return project;
}

export function restoreProject(actor, project) {
  if (!project.deleted) throw conflict();
  project.deleted = false;
  project.version += 1;
  store.record(actor.id, "restore", "project", project.id);
  return project;
}

// ------------------------------------------------------------------------ tasks

export function readNote(actor, body, errors, current) {
  if (!("internalNote" in body)) return current;
  if (actor.role !== "admin") throw forbidden();
  const note = body.internalNote;
  if (typeof note !== "string") throw badRequest();
  if (note.length > MAX_TITLE_LENGTH) {
    errors.push(fail("internalNote", "internalNote is too long"));
  }
  return note;
}

function checkTaskFields(title, priority, assigneeId, errors) {
  checkString(title, "title", MAX_TITLE_LENGTH, errors);
  checkPriority(priority, errors);
  if (assigneeId !== null && store.findUser(assigneeId) === undefined) {
    errors.push(fail("assigneeId", "assigneeId is not a known user"));
  }
  if (errors.length > 0) throw invalid(errors);
}

export function createTask(actor, project, title, priority, assigneeId, note, errors) {
  checkTaskFields(title, priority, assigneeId, errors);
  const task = store.insertTask(project.id, title, priority, assigneeId, note);
  store.record(actor.id, "create", "task", task.id);
  return task;
}

export function replaceTask(actor, task, title, priority, assigneeId, note, errors) {
  checkTaskFields(title, priority, assigneeId, errors);
  task.title = title;
  task.priority = priority;
  task.assigneeId = assigneeId;
  task.internalNote = note;
  task.version += 1;
  store.record(actor.id, "update", "task", task.id);
  return task;
}

export function moveStatus(actor, task, status) {
  const errors = [];
  checkStatus(status, errors);
  if (errors.length > 0) throw invalid(errors);
  if (!TRANSITIONS.has(`${task.status}->${status}`)) throw invalidTransition();
  task.status = status;
  task.version += 1;
  store.record(actor.id, "update", "task", task.id);
  return task;
}

export function deleteTask(actor, task) {
  task.deleted = true;
  task.version += 1;
  store.record(actor.id, "delete", "task", task.id);
  return task;
}

export function restoreTask(actor, task) {
  if (!task.deleted) throw conflict();
  task.deleted = false;
  task.version += 1;
  store.record(actor.id, "restore", "task", task.id);
  return task;
}

// --------------------------------------------------------------------- comments

export function createComment(actor, task, body) {
  const errors = [];
  checkString(body, "body", MAX_COMMENT_LENGTH, errors);
  if (errors.length > 0) throw invalid(errors);
  const comment = store.insertComment(task.id, actor.id, body);
  store.record(actor.id, "create", "comment", comment.id);
  return comment;
}

export function removeComment(actor, comment) {
  if (actor.role !== "admin" && comment.authorId !== actor.id) throw forbidden();
  store.comments.delete(comment.id);
  store.record(actor.id, "delete", "comment", comment.id);
}

// ------------------------------------------------------------------------ users

export function createUser(actor, username, password, role, quota) {
  const errors = [];
  checkString(username, "username", MAX_NAME_LENGTH, errors);
  checkString(password, "password", MAX_NAME_LENGTH, errors);
  checkRole(role, errors);
  checkQuota(quota, errors);
  if (errors.length > 0) throw invalid(errors);
  if (store.findByUsername(username) !== undefined) throw conflict();
  const user = store.insertUser(username, password, role, quota);
  store.record(actor.id, "create", "user", user.id);
  return user;
}

export function updateUser(actor, user, body) {
  const errors = [];
  if ("role" in body) checkRole(body.role, errors);
  if ("quota" in body) checkQuota(body.quota, errors);
  if (errors.length > 0) throw invalid(errors);
  if ("role" in body) user.role = body.role;
  if ("quota" in body) user.quota = body.quota;
  user.version += 1;
  store.record(actor.id, "update", "user", user.id);
  return user;
}

export function deleteUser(actor, user) {
  if (user.id === actor.id) throw conflict();
  user.deleted = true;
  user.version += 1;
  store.record(actor.id, "delete", "user", user.id);
  return user;
}

// ---------------------------------------------------------- queries and reports

export function visibleProjects(user, includeDeleted) {
  return [...store.projects.values()].filter(
    (p) =>
      (includeDeleted || !p.deleted) && (user.role === "admin" || p.ownerId === user.id),
  );
}

export function visibleTasks(user, includeDeleted) {
  const allowed = new Set(visibleProjects(user, true).map((p) => p.id));
  return [...store.tasks.values()].filter(
    (t) => allowed.has(t.projectId) && (includeDeleted || !t.deleted),
  );
}

export function search(user, query) {
  const needle = query.toLowerCase();
  const results = [];
  for (const project of visibleProjects(user, false)) {
    if (project.name.toLowerCase().includes(needle)) {
      results.push({ type: "project", id: project.id, label: project.name });
    }
  }
  for (const task of visibleTasks(user, false)) {
    if (task.title.toLowerCase().includes(needle)) {
      results.push({ type: "task", id: task.id, label: task.title });
    }
  }
  return { results, total: results.length };
}

export function workload(user, groupBy) {
  const rows = visibleTasks(user, false);
  const groups = [];
  const group = (key, picked) => ({
    key,
    tasks: picked.length,
    totalScore: picked.reduce((sum, t) => sum + computeScore(t.priority, t.status), 0),
  });
  if (groupBy === "status") {
    for (const status of STATUSES) {
      groups.push(group(status, rows.filter((t) => t.status === status)));
    }
  } else if (groupBy === "assignee") {
    const named = [...new Set(rows.filter((t) => t.assigneeId !== null).map((t) => t.assigneeId))];
    named.sort((a, b) => a - b);
    for (const assignee of named) {
      groups.push(group(String(assignee), rows.filter((t) => t.assigneeId === assignee)));
    }
    const loose = rows.filter((t) => t.assigneeId === null);
    if (loose.length > 0) groups.push(group("unassigned", loose));
  } else {
    for (const project of visibleProjects(user, false).sort((a, b) => a.id - b.id)) {
      groups.push(group(project.name, rows.filter((t) => t.projectId === project.id)));
    }
  }
  return { groupBy, groups };
}

export function flushOutbox() {
  const pending = store.outbox.filter((event) => !event.delivered);
  for (const event of pending) event.delivered = true;
  return pending.length;
}

export function metrics() {
  const byStatus = {};
  for (const code of [...store.byStatus.keys()].sort((a, b) => a - b)) {
    byStatus[String(code)] = store.byStatus.get(code);
  }
  const byRoute = [...store.byRoute.keys()]
    .sort()
    .map((route) => ({ route, count: store.byRoute.get(route) }));
  return {
    requests: store.requests,
    byStatus,
    byRoute,
    auditEntries: store.audit.length,
    outboxPending: store.outboxPending(),
  };
}

export function stats() {
  const live = [...store.tasks.values()].filter((t) => !t.deleted);
  const counts = { todo: 0, in_progress: 0, done: 0, archived: 0 };
  for (const task of live) counts[task.status] += 1;
  const total = live.length;
  const sumScore = live.reduce((sum, t) => sum + computeScore(t.priority, t.status), 0);
  let best = null;
  for (const project of store.projects.values()) {
    if (project.deleted) continue;
    if (best === null || store.taskCount(project.id) > store.taskCount(best.id)) best = project;
  }
  return {
    projects: [...store.projects.values()].filter((p) => !p.deleted).length,
    tasks: total,
    users: [...store.users.values()].filter((u) => !u.deleted).length,
    sessions: store.sessions.size,
    comments: store.comments.size,
    byStatus: counts,
    avgScore: total === 0 ? 0 : Math.round((sumScore / total) * 100) / 100,
    topProjectName: best === null ? null : best.name,
    auditEntries: store.audit.length,
    outboxPending: store.outboxPending(),
  };
}

export function checkBulkSize(operations) {
  if (!Array.isArray(operations) || operations.length < 1 || operations.length > MAX_BULK_ITEMS) {
    throw invalid([fail("operations", "operations is out of range")]);
  }
}
