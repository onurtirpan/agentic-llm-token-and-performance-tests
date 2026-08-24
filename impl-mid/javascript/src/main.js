// Task Service, mid tier — Express implementation.

import { randomUUID } from "node:crypto";
import express from "express";

const MAX_TITLE_LENGTH = 80;
const MAX_NAME_LENGTH = 60;
const MIN_PRIORITY = 1;
const MAX_PRIORITY = 5;
const DEFAULT_LIMIT = 20;
const MAX_LIMIT = 100;
const PORT = 8080;

const STATUS_BONUS = { todo: 0, in_progress: 3, done: 5, archived: 0 };
const TRANSITIONS = new Set([
  "todo->in_progress",
  "todo->archived",
  "in_progress->todo",
  "in_progress->done",
  "done->archived",
]);
const PROJECT_SORTS = ["id", "name", "taskCount"];
const TASK_SORTS = ["id", "title", "priority", "score", "status"];

class AppError {
  constructor(status, code, message, details = []) {
    this.status = status;
    this.code = code;
    this.message = message;
    this.details = details;
  }
}

const users = new Map([
  [1, { id: 1, username: "admin", password: "admin-secret", role: "admin" }],
  [2, { id: 2, username: "alice", password: "alice-secret", role: "user" }],
  [3, { id: 3, username: "bob", password: "bob-secret", role: "user" }],
]);
const sessions = new Map();
const projects = new Map();
const tasks = new Map();
let nextProjectId = 1;
let nextTaskId = 1;

function computeScore(priority, status) {
  const baseScore = priority * 10;
  return baseScore + STATUS_BONUS[status];
}

function taskCount(projectId) {
  return [...tasks.values()].filter((task) => task.projectId === projectId).length;
}

function serializeProject(project) {
  return {
    id: project.id,
    name: project.name,
    ownerId: project.ownerId,
    taskCount: taskCount(project.id),
  };
}

function serializeTask(task) {
  return {
    id: task.id,
    projectId: task.projectId,
    title: task.title,
    priority: task.priority,
    status: task.status,
    assigneeId: task.assigneeId,
    score: task.score,
  };
}

function badRequest() {
  return new AppError(400, "bad_request", "the request is malformed");
}

function notFound() {
  return new AppError(404, "not_found", "the resource does not exist");
}

function forbidden() {
  return new AppError(403, "forbidden", "you may not access this resource");
}

function conflict() {
  return new AppError(409, "conflict", "the resource already exists");
}

function invalid(details) {
  details.sort((a, b) =>
    a.field < b.field ? -1
      : a.field > b.field ? 1
      : a.message < b.message ? -1
      : a.message > b.message ? 1
      : 0,
  );
  return new AppError(422, "validation_failed", "the request body is not valid", details);
}

function fail(field, message) {
  return { field, message };
}

function readBody(req) {
  const raw = typeof req.body === "string" ? req.body : "";
  if (raw.trim() === "") return {};
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw badRequest();
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw badRequest();
  return parsed;
}

function readInt(body, field, defaultValue) {
  const value = field in body ? body[field] : defaultValue;
  if (value === null) return null;
  if (typeof value === "number" && Number.isInteger(value)) return value;
  throw badRequest();
}

function readString(body, field, errors, maxLength, required) {
  const value = field in body ? body[field] : "";
  if (typeof value !== "string") throw badRequest();
  if (value === "") {
    if (required) errors.push(fail(field, `${field} is required`));
  } else if (value.length > maxLength) {
    errors.push(fail(field, `${field} is too long`));
  }
  return value;
}

function readPriority(body, errors) {
  const value = readInt(body, "priority", 0);
  if (value === null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
    errors.push(fail("priority", "priority is out of range"));
  }
  return value ?? 0;
}

function readUserRef(body, field, errors, defaultValue) {
  const value = readInt(body, field, defaultValue);
  if (value !== null && !users.has(value)) {
    errors.push(fail(field, `${field} is not a known user`));
  }
  return value;
}

function parseId(raw) {
  if (typeof raw !== "string" || !/^-?\d+$/.test(raw)) throw badRequest();
  return Number(raw);
}

function readPage(req, allowed) {
  const query = req.query;
  const errors = [];
  let limit = DEFAULT_LIMIT;
  let offset = 0;
  const sort = typeof query.sort === "string" ? query.sort : "id";
  const order = typeof query.order === "string" ? query.order : "asc";
  if ("limit" in query) {
    limit = /^-?\d+$/.test(String(query.limit)) ? Number(query.limit) : -1;
    if (limit < 1 || limit > MAX_LIMIT) errors.push(fail("limit", "limit is out of range"));
  }
  if ("offset" in query) {
    offset = /^-?\d+$/.test(String(query.offset)) ? Number(query.offset) : -1;
    if (offset < 0) errors.push(fail("offset", "offset is out of range"));
  }
  if (!allowed.includes(sort)) errors.push(fail("sort", "sort is not a valid field"));
  if (order !== "asc" && order !== "desc") errors.push(fail("order", "order must be asc or desc"));
  if (errors.length > 0) throw invalid(errors);
  return [limit, offset, sort, order];
}

function paginate(rows, limit, offset, sort, order) {
  const direction = order === "desc" ? -1 : 1;
  rows.sort((a, b) => Number(a.id) - Number(b.id));
  rows.sort((a, b) => {
    const left = a[sort];
    const right = b[sort];
    return direction * (left < right ? -1 : left > right ? 1 : 0);
  });
  return { items: rows.slice(offset, offset + limit), total: rows.length, limit, offset };
}

function authenticate(req, res) {
  const header = req.header("authorization") ?? "";
  const session = header.startsWith("Bearer ") ? sessions.get(header.slice(7)) : undefined;
  if (session === undefined) {
    throw new AppError(401, "unauthorized", "authentication is required");
  }
  res.locals.userId = session;
  return users.get(session);
}

function requireAdmin(user) {
  if (user.role !== "admin") throw forbidden();
}

function reachableProject(projectId, user) {
  const project = projects.get(projectId);
  if (project === undefined) throw notFound();
  if (user.role !== "admin" && project.ownerId !== user.id) throw forbidden();
  return project;
}

function reachableTask(taskId, user) {
  const task = tasks.get(taskId);
  if (task === undefined) throw notFound();
  reachableProject(task.projectId, user);
  return task;
}

function observe(req, res, next) {
  const requestId = req.header("x-request-id") || randomUUID().replace(/-/g, "").slice(0, 12);
  res.locals.requestId = requestId;
  res.locals.userId = null;
  res.setHeader("X-Request-Id", requestId);
  const started = Date.now();
  res.on("finish", () => {
    const status = res.statusCode;
    process.stdout.write(
      JSON.stringify({
        level: status >= 500 ? "error" : status >= 400 ? "warn" : "info",
        requestId,
        method: req.method,
        path: req.path,
        status,
        durationMs: Date.now() - started,
        userId: res.locals.userId,
      }) + "\n",
    );
  });
  next();
}

function getHealth(_req, res) {
  res.json({ status: "ok", projects: projects.size, tasks: tasks.size });
}

function login(req, res) {
  const body = readBody(req);
  const errors = [];
  const username = readString(body, "username", errors, MAX_NAME_LENGTH, true);
  const password = readString(body, "password", errors, MAX_NAME_LENGTH, true);
  if (errors.length > 0) throw invalid(errors);
  const user = [...users.values()].find(
    (candidate) => candidate.username === username && candidate.password === password,
  );
  if (user === undefined) {
    throw new AppError(401, "invalid_credentials", "the username or password is wrong");
  }
  const token = randomUUID().replace(/-/g, "");
  sessions.set(token, user.id);
  res.json({ token, userId: user.id, role: user.role });
}

function logout(req, res) {
  authenticate(req, res);
  sessions.delete((req.header("authorization") ?? "").slice(7));
  res.status(204).end();
}

function getMe(req, res) {
  const user = authenticate(req, res);
  res.json({ userId: user.id, username: user.username, role: user.role });
}

function listProjects(req, res) {
  const user = authenticate(req, res);
  const [limit, offset, sort, order] = readPage(req, PROJECT_SORTS);
  const rows = [...projects.values()]
    .filter((project) => user.role === "admin" || project.ownerId === user.id)
    .map(serializeProject);
  res.json(paginate(rows, limit, offset, sort, order));
}

function createProject(req, res) {
  const user = authenticate(req, res);
  requireAdmin(user);
  const body = readBody(req);
  const errors = [];
  const name = readString(body, "name", errors, MAX_NAME_LENGTH, true);
  const ownerId = readUserRef(body, "ownerId", errors, user.id);
  if (errors.length > 0) throw invalid(errors);
  if ([...projects.values()].some((p) => p.ownerId === ownerId && p.name === name)) {
    throw conflict();
  }
  const project = { id: nextProjectId, name, ownerId };
  projects.set(nextProjectId, project);
  nextProjectId += 1;
  res.status(201).json(serializeProject(project));
}

function getProject(req, res) {
  const user = authenticate(req, res);
  res.json(serializeProject(reachableProject(parseId(req.params.id), user)));
}

function updateProject(req, res) {
  const user = authenticate(req, res);
  requireAdmin(user);
  const project = reachableProject(parseId(req.params.id), user);
  const body = readBody(req);
  if ("name" in body) {
    const errors = [];
    const name = readString(body, "name", errors, MAX_NAME_LENGTH, true);
    if (errors.length > 0) throw invalid(errors);
    const taken = [...projects.values()].some(
      (p) => p.ownerId === project.ownerId && p.name === name && p.id !== project.id,
    );
    if (taken) throw conflict();
    project.name = name;
  }
  res.json(serializeProject(project));
}

function deleteProject(req, res) {
  const user = authenticate(req, res);
  requireAdmin(user);
  const project = reachableProject(parseId(req.params.id), user);
  for (const task of [...tasks.values()]) {
    if (task.projectId === project.id) tasks.delete(task.id);
  }
  projects.delete(project.id);
  res.status(204).end();
}

function listTasks(req, res) {
  const user = authenticate(req, res);
  const project = reachableProject(parseId(req.params.id), user);
  const [limit, offset, sort, order] = readPage(req, TASK_SORTS);
  const rows = [...tasks.values()]
    .filter((task) => task.projectId === project.id)
    .map(serializeTask);
  res.json(paginate(rows, limit, offset, sort, order));
}

function createTask(req, res) {
  const user = authenticate(req, res);
  const project = reachableProject(parseId(req.params.id), user);
  const body = readBody(req);
  const errors = [];
  const title = readString(body, "title", errors, MAX_TITLE_LENGTH, true);
  const priority = readPriority(body, errors);
  const assigneeId = readUserRef(body, "assigneeId", errors, null);
  if (errors.length > 0) throw invalid(errors);
  const task = {
    id: nextTaskId,
    projectId: project.id,
    title,
    priority,
    status: "todo",
    assigneeId,
    score: computeScore(priority, "todo"),
  };
  tasks.set(nextTaskId, task);
  nextTaskId += 1;
  res.status(201).json(serializeTask(task));
}

function getTask(req, res) {
  const user = authenticate(req, res);
  res.json(serializeTask(reachableTask(parseId(req.params.id), user)));
}

function replaceTask(req, res) {
  const user = authenticate(req, res);
  const task = reachableTask(parseId(req.params.id), user);
  const body = readBody(req);
  const errors = [];
  const title = readString(body, "title", errors, MAX_TITLE_LENGTH, true);
  const priority = readPriority(body, errors);
  const assigneeId = readUserRef(body, "assigneeId", errors, null);
  if (errors.length > 0) throw invalid(errors);
  task.title = title;
  task.priority = priority;
  task.assigneeId = assigneeId;
  task.score = computeScore(priority, task.status);
  res.json(serializeTask(task));
}

function updateStatus(req, res) {
  const user = authenticate(req, res);
  const task = reachableTask(parseId(req.params.id), user);
  const body = readBody(req);
  const status = body.status;
  if (typeof status !== "string" || !(status in STATUS_BONUS)) {
    throw invalid([fail("status", "status is not valid")]);
  }
  if (!TRANSITIONS.has(`${task.status}->${status}`)) {
    throw new AppError(409, "invalid_transition", "the status change is not allowed");
  }
  task.status = status;
  task.score = computeScore(task.priority, status);
  res.json(serializeTask(task));
}

function deleteTask(req, res) {
  const user = authenticate(req, res);
  const task = reachableTask(parseId(req.params.id), user);
  tasks.delete(task.id);
  res.status(204).end();
}

function getStats(req, res) {
  const user = authenticate(req, res);
  requireAdmin(user);
  const byStatus = { todo: 0, in_progress: 0, done: 0, archived: 0 };
  for (const task of tasks.values()) byStatus[task.status] += 1;
  const total = tasks.size;
  const sumScore = [...tasks.values()].reduce((sum, task) => sum + task.score, 0);
  const avgScore = total === 0 ? 0 : Math.round((sumScore / total) * 100) / 100;
  let best = null;
  for (const project of projects.values()) {
    if (best === null || taskCount(project.id) > taskCount(best.id)) best = project;
  }
  res.json({
    projects: projects.size,
    tasks: total,
    users: users.size,
    sessions: sessions.size,
    byStatus,
    avgScore,
    topProjectName: best === null ? null : best.name,
  });
}

function fallback(_req, _res) {
  throw notFound();
}

const app = express();
app.use(observe);
app.use(express.text({ type: "*/*" }));

app.get("/health", getHealth);
app.post("/auth/login", login);
app.post("/auth/logout", logout);
app.get("/me", getMe);
app.get("/projects", listProjects);
app.post("/projects", createProject);
app.get("/projects/:id", getProject);
app.patch("/projects/:id", updateProject);
app.delete("/projects/:id", deleteProject);
app.get("/projects/:id/tasks", listTasks);
app.post("/projects/:id/tasks", createTask);
app.get("/tasks/:id", getTask);
app.put("/tasks/:id", replaceTask);
app.patch("/tasks/:id/status", updateStatus);
app.delete("/tasks/:id", deleteTask);
app.get("/stats", getStats);

app.use(fallback);
app.use((error, _req, res, _next) => {
  const problem = error instanceof AppError ? error : badRequest();
  res.status(problem.status).json({
    error: {
      code: problem.code,
      message: problem.message,
      requestId: res.locals.requestId,
      details: problem.details,
    },
  });
});

app.listen(PORT, "127.0.0.1");
