// Task Service — Express implementation.

import express from "express";

const MAX_TITLE_LENGTH = 80;
const MIN_PRIORITY = 1;
const MAX_PRIORITY = 5;
const PORT = 8080;

const tasks = new Map();
let nextId = 1;

function computeScore(priority, done) {
  const baseScore = priority * 10;
  return done ? baseScore : baseScore + 5;
}

function validate(title, priority) {
  if (title.length === 0) return "title is required";
  if (title.length > MAX_TITLE_LENGTH) return "title is too long";
  if (priority < MIN_PRIORITY || priority > MAX_PRIORITY) return "priority is out of range";
  return null;
}

function fail(res, status, message) {
  res.status(status).json({ error: message });
}

function parseId(raw) {
  return /^-?\d+$/.test(raw) ? Number(raw) : null;
}

function readInput(body) {
  if (typeof body !== "object" || body === null) return null;
  const title = body.title ?? "";
  const priority = body.priority ?? 0;
  const done = body.done ?? false;
  if (typeof title !== "string" || typeof priority !== "number" || typeof done !== "boolean") {
    return null;
  }
  return { title, priority, done };
}

function sortedTasks() {
  return [...tasks.values()].sort((a, b) => a.id - b.id);
}

const app = express();
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ status: "ok", count: tasks.size });
});

app.get("/tasks", (req, res) => {
  const done = req.query.done;
  if (done !== undefined && done !== "true" && done !== "false") {
    return fail(res, 400, "done must be true or false");
  }
  const selected = sortedTasks().filter((t) => done === undefined || t.done === (done === "true"));
  selected.sort((a, b) => b.score - a.score || a.id - b.id);
  res.json({ tasks: selected, total: selected.length });
});

app.get("/tasks/:id", (req, res) => {
  const id = parseId(req.params.id);
  if (id === null) return fail(res, 400, "invalid id");
  const task = tasks.get(id);
  if (task === undefined) return fail(res, 404, "task not found");
  res.json(task);
});

app.post("/tasks", (req, res) => {
  const input = readInput(req.body);
  if (input === null) return fail(res, 400, "invalid json");
  const error = validate(input.title, input.priority);
  if (error !== null) return fail(res, 400, error);
  const task = {
    id: nextId,
    title: input.title,
    priority: input.priority,
    done: false,
    score: computeScore(input.priority, false),
  };
  tasks.set(nextId, task);
  nextId += 1;
  res.status(201).json(task);
});

app.put("/tasks/:id", (req, res) => {
  const id = parseId(req.params.id);
  if (id === null) return fail(res, 400, "invalid id");
  const task = tasks.get(id);
  if (task === undefined) return fail(res, 404, "task not found");
  const input = readInput(req.body);
  if (input === null) return fail(res, 400, "invalid json");
  const error = validate(input.title, input.priority);
  if (error !== null) return fail(res, 400, error);
  task.title = input.title;
  task.priority = input.priority;
  task.done = input.done;
  task.score = computeScore(input.priority, input.done);
  res.json(task);
});

app.delete("/tasks/:id", (req, res) => {
  const id = parseId(req.params.id);
  if (id === null) return fail(res, 400, "invalid id");
  if (!tasks.delete(id)) return fail(res, 404, "task not found");
  res.status(204).end();
});

app.get("/stats", (_req, res) => {
  const all = sortedTasks();
  const total = all.length;
  const doneCount = all.filter((t) => t.done).length;
  const sumScore = all.reduce((sum, t) => sum + t.score, 0);
  const avgScore = total === 0 ? 0 : Math.round((sumScore / total) * 100) / 100;
  let best = null;
  for (const task of all) {
    if (!task.done && (best === null || task.priority > best.priority)) best = task;
  }
  res.json({
    total,
    doneCount,
    openCount: total - doneCount,
    avgScore,
    topOpenTitle: best === null ? null : best.title,
  });
});

app.use((_req, res) => fail(res, 404, "not found"));
app.use((_error, _req, res, _next) => fail(res, 400, "invalid json"));

app.listen(PORT, "127.0.0.1");
