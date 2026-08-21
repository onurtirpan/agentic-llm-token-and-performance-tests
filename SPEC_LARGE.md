# Task Service — large tier specification

The large tier is the mid tier plus the machinery a long-lived enterprise service
accumulates: a layered module structure, optimistic concurrency, idempotent
writes, soft delete and restore, an audit trail, an outbox, per-caller quotas,
bulk writes, search, reports, metrics, comments, and user administration.

Everything in `SPEC_MID.md` still holds unless this document overrides it. Read
`SPEC_MID.md` first.

## Rules of the experiment

1. Each language uses the same framework as its small-tier and mid-tier
   implementation.
2. Identifier names are the same in every language. Only the casing follows the
   native convention.
3. JSON keys are byte-identical in every language. They use `camelCase`.
4. The server listens on port 8080. It keeps all state in memory.
5. No database, no disk, no outbound network call.

## Mandatory module structure

The large tier must be split into four modules, in this dependency order. A
module may only import from the modules above it.

| Module | Holds |
| --- | --- |
| `domain` | types, constants, enums, `computeScore`, transition table, pure validation |
| `store` | all mutable state, and the repository functions that read and write it |
| `service` | business rules, authorization decisions, audit and outbox emission |
| `api` | routing, middleware, request parse, response serialize, and the entry point |

The file names follow each language's convention, for example
`domain.py` / `domain.ts` / `Domain.cs` / `domain.go` / `domain.rs` /
`domain.zig` / `domain.c` with `domain.h` / `domain.cpp` with `domain.hpp` /
`Domain.php` / `Domain.java`. A language that needs a separate header or
interface file writes one; that cost is part of the measurement.

`api` may be split further if a language's framework demands it. No module may
be merged away.

## Deliberate exclusions

These carry over from `SPEC_MID.md` and remain excluded for the same reasons: no
password hashing, no signed tokens, and **no wall-clock timestamps in any
response body**.

Because there is no clock, two things that normally use time use a counter
instead:

1. **Audit and outbox ordering** uses a monotonic `seq` integer, not a timestamp.
2. **Rate limiting** is a per-session request **quota**, not a sliding window.

Both are documented below. This keeps every response byte-comparable.

## Constants

Everything from `SPEC_MID.md`, plus:

| Name | Value |
| --- | --- |
| `MAX_COMMENT_LENGTH` | 200 |
| `MAX_BULK_ITEMS` | 20 |
| `DEFAULT_QUOTA` | 10000 |
| `PROBE_QUOTA` | 5 |

## Data model

`User`, `Project` and `Task` extend the mid-tier shapes. Added fields are marked.

```
User
    id          int
    username    string
    password    string       # never serialized
    role        string       # "admin" or "user"
    quota       int          # added: total requests this user's sessions may make
    deleted     bool         # added: soft delete

Session
    token       string
    userId      int
    used        int          # added: requests consumed against the quota

Project
    id          int
    name        string
    ownerId     int
    version     int          # added: starts at 1, increments on every write
    deleted     bool         # added: soft delete
    taskCount   int          # derived, excludes deleted tasks

Task
    id            int
    projectId     int
    title         string
    priority      int
    status        string
    assigneeId    int | null
    internalNote  string     # added: visible to an admin only
    version       int        # added
    deleted       bool       # added
    score         int        # derived

Comment
    id        int
    taskId    int
    authorId  int
    body      string

AuditEntry
    seq         int
    actorId     int
    action      string    # "create" | "update" | "delete" | "restore"
    resource    string    # "project" | "task" | "comment" | "user"
    resourceId  int

OutboxEvent
    seq         int
    name        string    # "<resource>.<action>", for example "task.create"
    resourceId  int
    delivered   bool
```

## Seed data

| id | username | password | role | quota |
| --- | --- | --- | --- | --- |
| 1 | `admin` | `admin-secret` | `admin` | `DEFAULT_QUOTA` |
| 2 | `alice` | `alice-secret` | `user` | `DEFAULT_QUOTA` |
| 3 | `bob` | `bob-secret` | `user` | `DEFAULT_QUOTA` |
| 4 | `probe` | `probe-secret` | `user` | `PROBE_QUOTA` |

`nextProjectId`, `nextTaskId`, `nextCommentId`, `nextUserId` and `nextSeq` all
start at 1. `nextUserId` starts at 5.

## Field-level visibility

`internalNote` appears in a serialized task **only when the caller's role is
`admin`**. For a `user` role the key is absent from the object entirely — not
null, absent.

## Soft delete

`DELETE` never removes a row. It sets `deleted` to true, bumps `version`, and
writes an audit entry and an outbox event.

- A deleted row is invisible: reading it is `404 not_found`.
- A list excludes deleted rows.
- `POST /projects/{id}/restore` and `POST /tasks/{id}/restore` clear the flag.
  Restoring a row that is not deleted is `409 conflict`.
- **A restore route is the one exception to invisibility.** Its existence check
  finds a deleted row, so restoring a deleted id succeeds rather than returning
  `404`. Only a genuinely unknown id is `404` there. The `If-Match` on a restore
  compares against the deleted row's current `version`.
- An **admin** may pass `?includeDeleted=true` on a list to see deleted rows. A
  `user` passing it gets `403 forbidden`.
- Deleting a project cascades: every task inside it is soft-deleted too, each
  with its own audit entry. Restoring a project does **not** restore its tasks.

## What a delete returns

A soft delete leaves the row in existence, so it returns the resource, not an
empty body.

- `DELETE /projects/{id}`, `DELETE /tasks/{id}` and `DELETE /users/{id}` return
  `200` with the serialized resource, whose `deleted` is now `true` and whose
  `version` has been incremented.
- `POST /projects/{id}/restore` and `POST /tasks/{id}/restore` likewise return
  `200` with the serialized resource.
- `DELETE /comments/{id}` is a hard delete and returns `204` with no body.
- `POST /auth/logout` returns `204` with no body.

## Optimistic concurrency

Every write to an existing `Project` or `Task` requires the caller to state the
version it expects.

- The response to any single-resource read or write carries an `ETag` header
  whose value is the `version` as a decimal string, for example `ETag: 3`. This
  includes a `201` from a `POST` that creates one resource, and a replayed
  idempotent response.
- `PATCH`, `PUT`, `DELETE` and `restore` on a project or a task require an
  `If-Match` header.
- A missing `If-Match` is `428 precondition_required`, message
  `the If-Match header is required`.
- An `If-Match` that does not equal the current version is
  `412 precondition_failed`, message `the resource has changed`.
- A successful write increments `version` by one.

`version` starts at 1 when the row is created.

## Idempotent writes

Every `POST` accepts an optional `Idempotency-Key` header.

- The first request with a given key runs normally. The status and body are
  recorded against that key.
- A later request with the same key returns the **recorded status and body**
  without re-running the handler, and adds the header
  `Idempotency-Replayed: true`.
- The key is scoped per session token. The same key from a different token is a
  new request.
- A key is recorded even when the first response was an error.

## Request quota

Each session carries a `used` counter. Every authenticated request increments it
before the handler runs.

- When `used` would exceed the user's `quota`, the response is
  `429 quota_exceeded`, message `the request quota is exhausted`.
- Every authenticated response carries `X-Quota-Remaining`, the value of
  `quota - used` after the increment, never below zero.
- `GET /health` and `POST /auth/login` are unauthenticated and do not count.

The seeded `probe` user has a quota of `PROBE_QUOTA`, which makes the `429`
reachable in a test.

## Audit trail and outbox

Every successful create, update, delete or restore of a project, task, comment
or user appends **one** `AuditEntry` and **one** `OutboxEvent`, both taking the
next `seq`. A single `seq` counter serves both, so an audit entry and its
matching outbox event never share a number.

The outbox `name` is `<resource>.<action>`, for example `project.create`,
`task.delete`, `comment.create`, `user.update`.

A read never writes an audit entry. A failed write never writes one. A cascade
writes one entry per affected row.

## The error envelope

As `SPEC_MID.md`, with these codes added:

| Status | `code` | `message` |
| --- | --- | --- |
| 412 | `precondition_failed` | `the resource has changed` |
| 428 | `precondition_required` | `the If-Match header is required` |
| 429 | `quota_exceeded` | `the request quota is exhausted` |

## Structured logging

As `SPEC_MID.md`, with two keys appended, in this exact order:

```json
{"level":"info","requestId":"abc","method":"GET","path":"/projects","status":200,"durationMs":1,"userId":2,"quotaRemaining":9998,"auditSeq":0}
```

- `quotaRemaining` is an integer, or `null` for an unauthenticated request.
- `auditSeq` is the number of audit entries written by this request, so `0` for
  a read and `1` for a simple write. A cascade reports the real count.

## Validation details

Everything from `SPEC_MID.md`, plus:

| Field | Rule | Message |
| --- | --- | --- |
| `body` | not empty, at most `MAX_COMMENT_LENGTH` | `body is required` / `body is too long` |
| `internalNote` | at most `MAX_TITLE_LENGTH` | `internalNote is too long` |
| `role` | `admin` or `user` | `role is not valid` |
| `quota` | integer, 0 or more | `quota is out of range` |
| `operations` | 1 to `MAX_BULK_ITEMS` entries | `operations is out of range` |
| `groupBy` | `assignee`, `status` or `project` | `groupBy is not valid` |
| `q` | not empty | `q is required` |

## Endpoints

34 routes. Everything not listed is `404 not_found`.

### Unchanged from the mid tier

`GET /health` (now also reports `comments`), `POST /auth/login`,
`POST /auth/logout`, `GET /me`.

`GET /health` returns
`{"status":"ok","projects":n,"tasks":n,"comments":n}`, counting non-deleted rows.

### Users — admin only

| Route | Notes |
| --- | --- |
| `GET /users` | paginated, sort set `id`, `username`, `role` |
| `POST /users` | body `{"username","password","role","quota"}`; `role` defaults to `user`, `quota` defaults to `DEFAULT_QUOTA`; duplicate username is `409 conflict` |
| `GET /users/{id}` | |
| `PATCH /users/{id}` | body may carry `role` and `quota`; needs `If-Match` |
| `DELETE /users/{id}` | soft delete; needs `If-Match`; deleting yourself is `409 conflict` |

A user serializes as `{"id","username","role","quota","version","deleted"}`.
`password` never appears.

`User.version` starts at 1 and follows the same concurrency rules.

### Projects

| Route | Notes |
| --- | --- |
| `GET /projects` | paginated; sort `id`, `name`, `taskCount`; `?includeDeleted=true` for an admin |
| `POST /projects` | admin only |
| `GET /projects/{id}` | |
| `PATCH /projects/{id}` | admin only; needs `If-Match` |
| `DELETE /projects/{id}` | admin only; needs `If-Match`; cascades to tasks |
| `POST /projects/{id}/restore` | admin only; needs `If-Match` |

### Tasks

| Route | Notes |
| --- | --- |
| `GET /tasks` | paginated across every reachable project; sort `id`, `title`, `priority`, `score`, `status`; filters `?status=` and `?assigneeId=` |
| `GET /projects/{id}/tasks` | as the mid tier |
| `POST /projects/{id}/tasks` | body may carry `internalNote`, admin only for that field |
| `GET /tasks/{id}` | |
| `PUT /tasks/{id}` | needs `If-Match` |
| `PATCH /tasks/{id}/status` | needs `If-Match`; same transition table |
| `DELETE /tasks/{id}` | needs `If-Match` |
| `POST /tasks/{id}/restore` | needs `If-Match` |
| `POST /tasks/bulk` | see below |

Writing `internalNote` as a `user` role is `403 forbidden`.

`?status=` must be one of the four values, else `422` on field `status`.
`?assigneeId=` must be an integer, else `422` on field `assigneeId`.

### `POST /tasks/bulk`

Body: `{"operations": [ {"op": "create"|"status"|"delete", ...}, ... ]}`.

- `create` carries `projectId`, `title`, `priority`.
- `status` carries `id`, `status`, `version`.
- `delete` carries `id`, `version`.

The `version` inside an item replaces `If-Match` for that item. The request
itself needs no `If-Match`.

Each item is applied independently. The response is always `200`:

```json
{"results": [{"index": 0, "status": 201, "id": 7, "error": null},
             {"index": 1, "status": 412, "id": null, "error": "precondition_failed"}]}
```

`error` is the `code` string on failure and `null` on success. `id` is the
affected resource id on success and `null` on failure. An unknown `op` gives
`status` 422 and `error` `validation_failed`.

`operations` outside 1 to `MAX_BULK_ITEMS` is `422` on field `operations`.

### Comments

| Route | Notes |
| --- | --- |
| `GET /tasks/{id}/comments` | paginated; sort `id`, `authorId` |
| `POST /tasks/{id}/comments` | body `{"body"}`; author is the caller |
| `DELETE /comments/{id}` | the author or an admin; anyone else is `403` |

A comment has no `version`. `DELETE /comments/{id}` therefore needs **no**
`If-Match`, and it is a hard delete, not a soft delete. It still writes an audit
entry and an outbox event.

A comment serializes as `{"id","taskId","authorId","body"}`.

### `GET /search`

Query `?q=<text>`, required, non-empty. Case-insensitive substring match.

Searches project `name` and task `title` across everything the caller may reach,
excluding deleted rows.

```json
{"results": [{"type": "project", "id": 1, "label": "Apollo"},
             {"type": "task", "id": 4, "label": "Design"}],
 "total": 2}
```

Projects come before tasks. Within a type, `id` ascending. Not paginated.

### `GET /reports/workload`

Query `?groupBy=assignee|status|project`, defaults to `status`.

```json
{"groupBy": "status",
 "groups": [{"key": "todo", "tasks": 3, "totalScore": 90}, ...]}
```

- `groupBy=status` emits all four statuses, including zeros, in the order
  `todo`, `in_progress`, `done`, `archived`.
- `groupBy=assignee` emits one group per distinct `assigneeId` present, `key` as
  a decimal string, ascending, and `"unassigned"` last when any task has a null
  assignee.
- `groupBy=project` emits one group per reachable non-deleted project, `key` as
  the project name, ordered by project `id` ascending.

Deleted tasks are excluded. The row-level rule applies.

### `GET /audit` — admin only

Paginated; sort `seq` only. Filters `?actorId=`, `?resource=`, `?action=`.
An entry serializes as `{"seq","actorId","action","resource","resourceId"}`.

### `GET /outbox` — admin only

Paginated; sort `seq` only. Filter `?delivered=true|false`.
An event serializes as `{"seq","name","resourceId","delivered"}`.

### `POST /outbox/flush` — admin only

Marks every undelivered event as delivered.

```
200 {"flushed": <count>}
```

This writes no audit entry.

### `GET /metrics` — admin only

```json
{"requests": <total completed requests>,
 "byStatus": {"200": 12, "404": 3},
 "byRoute": [{"route": "GET /projects", "count": 4}],
 "auditEntries": <count>,
 "outboxPending": <count>}
```

- `byStatus` keys are the status codes as decimal strings, ascending numerically.
- `byRoute` is the matched route pattern, not the concrete path, so
  `GET /projects/{id}` and never `GET /projects/7`. Ordered by `route`
  ascending, then `count`.
- `requests` counts every request that **completed before** this one. The
  request being served is counted after its own response is built, so it does
  not appear in its own reply. `requests` therefore always equals the sum of
  `byStatus`.

### `GET /stats` — admin only

As the mid tier, plus `comments`, `auditEntries` and `outboxPending`. Counts
exclude deleted rows.

```json
{"projects":n,"tasks":n,"users":n,"sessions":n,"comments":n,
 "byStatus":{...},"avgScore":x,"topProjectName":s,
 "auditEntries":n,"outboxPending":n}
```

## Order of checks

This order is fixed and the test depends on it.

1. Request id.
2. Route match — unknown path is `404`.
3. Authentication — `401`.
4. Quota — `429`.
5. Role authorization — `403` for an admin-only route.
6. Path id parse — `400`.
7. Existence — `404`, including a soft-deleted row.
8. Row-level authorization — `403`.
9. `If-Match` presence — `428`.
10. `If-Match` value — `412`.
11. Body parse — `400`.
12. Idempotency replay, on a `POST` only — return the recorded status and body.
13. Validation — `422`.
14. Business rules — `409`.
15. Handler.

The replay sits at step 12, not earlier: a caller who is forbidden, or who names
an unknown resource, or who sends a malformed body, gets that answer rather than
a replay. Only a request that has cleared every structural check reaches the
recorded outcome. A recorded outcome may itself be an error from step 13 or 14.

Because a replayed body is returned byte for byte, it carries the `requestId` of
the **first** request, not the replaying one.

## Small clarifications

No conformance case distinguishes any of these. They are pinned so the ten
implementations agree anyway.

1. A `429` carries **no** `X-Quota-Remaining`. The quota is not charged when it
   is already exhausted, so there is no value to report.
2. A comment response carries **no** `ETag`. A `Comment` has no `version`.
3. An unmatched path is recorded in `metrics.byRoute` under the literal label
   `unmatched`, because no route pattern exists for it.
4. An unknown `op` inside a bulk item reports the detail
   `{"field": "op", "message": "op is not valid"}`.
5. The project cascade in the conformance suite writes **five** audit entries:
   the project itself plus tasks 1, 2, 3 and the bulk-created task 5.
