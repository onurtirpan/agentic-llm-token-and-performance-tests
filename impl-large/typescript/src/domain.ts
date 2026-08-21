// Task Service, large tier — domain types, constants and pure rules.

export const MAX_TITLE_LENGTH = 80;
export const MAX_NAME_LENGTH = 60;
export const MAX_COMMENT_LENGTH = 200;
export const MAX_BULK_ITEMS = 20;
export const MIN_PRIORITY = 1;
export const MAX_PRIORITY = 5;
export const DEFAULT_LIMIT = 20;
export const MAX_LIMIT = 100;
export const DEFAULT_QUOTA = 10000;
export const PROBE_QUOTA = 5;
export const PORT = 8080;

export const ROLES = ["admin", "user"];
export const STATUSES = ["todo", "in_progress", "done", "archived"];
export const STATUS_BONUS: Record<string, number> = {
  todo: 0,
  in_progress: 3,
  done: 5,
  archived: 0,
};
export const TRANSITIONS = new Set([
  "todo->in_progress",
  "todo->archived",
  "in_progress->todo",
  "in_progress->done",
  "done->archived",
]);
export const ACTIONS = ["create", "update", "delete", "restore"];
export const PROJECT_SORTS = ["id", "name", "taskCount"];
export const TASK_SORTS = ["id", "title", "priority", "score", "status"];
export const USER_SORTS = ["id", "username", "role"];
export const COMMENT_SORTS = ["id", "authorId"];
export const SEQ_SORTS = ["seq"];
export const GROUP_BYS = ["assignee", "status", "project"];

export interface User {
  id: number;
  username: string;
  password: string;
  role: string;
  quota: number;
  version: number;
  deleted: boolean;
}

export interface Session {
  token: string;
  userId: number;
  used: number;
}

export interface Project {
  id: number;
  name: string;
  ownerId: number;
  version: number;
  deleted: boolean;
}

export interface Task {
  id: number;
  projectId: number;
  title: string;
  priority: number;
  status: string;
  assigneeId: number | null;
  internalNote: string;
  version: number;
  deleted: boolean;
}

export interface Comment {
  id: number;
  taskId: number;
  authorId: number;
  body: string;
}

export interface AuditEntry {
  seq: number;
  actorId: number;
  action: string;
  resource: string;
  resourceId: number;
}

export interface OutboxEvent {
  seq: number;
  name: string;
  resourceId: number;
  delivered: boolean;
}

export interface Detail {
  field: string;
  message: string;
}

export type Body = Record<string, unknown>;
export type Row = Record<string, unknown>;

/** Every failure path throws this. The api layer turns it into the envelope. */
export class AppError {
  constructor(
    readonly status: number,
    readonly code: string,
    readonly message: string,
    readonly details: Detail[] = [],
  ) {}
}

export function badRequest(): AppError {
  return new AppError(400, "bad_request", "the request is malformed");
}

export function unauthorized(): AppError {
  return new AppError(401, "unauthorized", "authentication is required");
}

export function invalidCredentials(): AppError {
  return new AppError(401, "invalid_credentials", "the username or password is wrong");
}

export function forbidden(): AppError {
  return new AppError(403, "forbidden", "you may not access this resource");
}

export function notFound(): AppError {
  return new AppError(404, "not_found", "the resource does not exist");
}

export function conflict(): AppError {
  return new AppError(409, "conflict", "the resource already exists");
}

export function invalidTransition(): AppError {
  return new AppError(409, "invalid_transition", "the status change is not allowed");
}

export function preconditionFailed(): AppError {
  return new AppError(412, "precondition_failed", "the resource has changed");
}

export function preconditionRequired(): AppError {
  return new AppError(428, "precondition_required", "the If-Match header is required");
}

export function quotaExceeded(): AppError {
  return new AppError(429, "quota_exceeded", "the request quota is exhausted");
}

export function invalid(details: Detail[]): AppError {
  details.sort((a, b) =>
    a.field < b.field ? -1
      : a.field > b.field ? 1
      : a.message < b.message ? -1
      : a.message > b.message ? 1
      : 0,
  );
  return new AppError(422, "validation_failed", "the request body is not valid", details);
}

export function fail(field: string, message: string): Detail {
  return { field, message };
}

export function computeScore(priority: number, status: string): number {
  const baseScore = priority * 10;
  return baseScore + STATUS_BONUS[status];
}

export function checkString(
  value: string,
  fieldName: string,
  maxLength: number,
  errors: Detail[],
): void {
  if (value === "") {
    errors.push(fail(fieldName, `${fieldName} is required`));
  } else if (value.length > maxLength) {
    errors.push(fail(fieldName, `${fieldName} is too long`));
  }
}

export function checkPriority(value: number | null, errors: Detail[]): void {
  if (value === null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
    errors.push(fail("priority", "priority is out of range"));
  }
}

export function checkStatus(value: unknown, errors: Detail[]): void {
  if (typeof value !== "string" || !STATUSES.includes(value)) {
    errors.push(fail("status", "status is not valid"));
  }
}

export function checkRole(value: unknown, errors: Detail[]): void {
  if (typeof value !== "string" || !ROLES.includes(value)) {
    errors.push(fail("role", "role is not valid"));
  }
}

export function checkQuota(value: unknown, errors: Detail[]): void {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
    errors.push(fail("quota", "quota is out of range"));
  }
}
