# Task Service — mid tier specification

The mid tier adds the concerns an enterprise service always carries:
authentication, role and row level authorization, a middleware chain, structured
logging, a uniform error envelope, multi-error validation, pagination, sorting,
a status state machine, and cascade delete.

Every implementation must satisfy this document exactly. `tools/conformance_mid.py`
checks all cases against a running server.

## Rules of the experiment

These carry over from `SPEC.md` unchanged.

1. Each language uses the same framework as its small-tier implementation.
2. Identifier names are the same in every language. Only the casing follows the
   native convention.
3. JSON keys are byte-identical in every language. They use `camelCase`.
4. The server listens on port 8080. It keeps all state in memory.
5. No database, no disk, no outbound network call.

## Deliberate exclusions

Read these before judging the design. Each one is excluded to keep the workload
identical across ten languages, not because it does not matter in production.

1. **No password hashing.** Credentials are seeded in plaintext and compared
   directly. A real service hashes with bcrypt or argon2. Nine languages have a
   library call for it; C has a hundred-line detour. Including it would measure
   crypto availability, not API structure.
2. **No signed tokens.** A session token is an opaque server-side handle, not a
   JWT. Same reason: HMAC-SHA256 is a standard-library call in nine languages and
   absent in C.
3. **No wall-clock timestamps in any response body.** A response must be
   byte-comparable, so no `createdAt` field exists. `durationMs` appears in logs
   only, where its value is never asserted.

## Constants

| Name | Value |
| --- | --- |
| `MAX_TITLE_LENGTH` | 80 |
| `MAX_NAME_LENGTH` | 60 |
| `MIN_PRIORITY` | 1 |
| `MAX_PRIORITY` | 5 |
| `DEFAULT_LIMIT` | 20 |
| `MAX_LIMIT` | 100 |
| `PORT` | 8080 |

## Data model

```
User
    id        int
    username  string
    password  string      # never serialized
    role      string      # "admin" or "user"

Session
    token     string      # opaque, at least 16 characters, unique
    userId    int

Project
    id        int
    name      string
    ownerId   int
    taskCount int         # derived, never stored

Task
    id         int
    projectId  int
    title      string
    priority   int        # 1..5
    status     string     # "todo" | "in_progress" | "done" | "archived"
    assigneeId int | null
    score      int        # derived
```

The client never sends `id`, `score`, `taskCount`, or `ownerId`.

## Seed data

The store starts with these users and nothing else. No project, no task, no
session.

| id | username | password | role |
| --- | --- | --- | --- |
| 1 | `admin` | `admin-secret` | `admin` |
| 2 | `alice` | `alice-secret` | `user` |
| 3 | `bob` | `bob-secret` | `user` |

`nextProjectId` and `nextTaskId` both start at 1.

## Derived value

```
STATUS_BONUS = { "todo": 0, "in_progress": 3, "done": 5, "archived": 0 }

computeScore(priority, status):
    baseScore = priority * 10
    return baseScore + STATUS_BONUS[status]
```

`taskCount` for a project is the number of tasks whose `projectId` matches.

## Iteration order

Every iteration runs in ascending `id` order. An `id` always grows, so ascending
`id` equals insertion order.

## The error envelope

Every error response, without exception, has this exact shape.

```json
{
  "error": {
    "code": "<machine_code>",
    "message": "<human message>",
    "requestId": "<the request id>",
    "details": []
  }
}
```

`details` is always an array. It is empty unless the code is
`validation_failed`.

| Status | `code` | `message` |
| --- | --- | --- |
| 400 | `bad_request` | `the request is malformed` |
| 401 | `unauthorized` | `authentication is required` |
| 401 | `invalid_credentials` | `the username or password is wrong` |
| 403 | `forbidden` | `you may not access this resource` |
| 404 | `not_found` | `the resource does not exist` |
| 409 | `conflict` | `the resource already exists` |
| 409 | `invalid_transition` | `the status change is not allowed` |
| 422 | `validation_failed` | `the request body is not valid` |

A malformed JSON body is `400 bad_request`. A well-formed body that breaks a
rule is `422 validation_failed`.

## Validation details

When the code is `validation_failed`, `details` holds one entry per broken rule:

```json
{"field": "<field name>", "message": "<what is wrong>"}
```

All broken rules are reported together, never just the first. The array is
sorted by `field` ascending, then by `message` ascending.

The rule set:

| Field | Rule | Message |
| --- | --- | --- |
| `name` | not empty | `name is required` |
| `name` | at most `MAX_NAME_LENGTH` characters | `name is too long` |
| `title` | not empty | `title is required` |
| `title` | at most `MAX_TITLE_LENGTH` characters | `title is too long` |
| `priority` | `MIN_PRIORITY` to `MAX_PRIORITY` | `priority is out of range` |
| `status` | one of the four values | `status is not valid` |
| `assigneeId` | null, or an existing user id | `assigneeId is not a known user` |
| `username` | not empty | `username is required` |
| `password` | not empty | `password is required` |
| `limit` | integer, 1 to `MAX_LIMIT` | `limit is out of range` |
| `offset` | integer, 0 or more | `offset is out of range` |
| `sort` | in the allowed set for the route | `sort is not a valid field` |
| `order` | `asc` or `desc` | `order must be asc or desc` |

A field absent from the body takes its default and is not validated, except
where an endpoint marks it required.

## The middleware chain

Every request passes through this chain, in this exact order:

1. **Request id.** If the request carries an `X-Request-Id` header with a
   non-empty value, use it. Otherwise generate an opaque id of at least 8
   characters. The response always carries `X-Request-Id` with this value.
2. **Route match.** An unknown path is `404 not_found`.
3. **Authentication.** Every route except `GET /health` and `POST /auth/login`
   requires a valid session. A missing or malformed `Authorization` header, or a
   token with no session, is `401 unauthorized`. The header format is
   `Authorization: Bearer <token>`.
4. **Authorization.** A route marked *admin only* rejects a `user` role with
   `403 forbidden`. A row-level rule rejects with `403 forbidden`.
5. **Handler.**
6. **Logging.** One log line per completed request, written to stdout.

## Structured logging

Each completed request writes exactly one JSON object on its own line to
stdout:

```json
{"level":"info","requestId":"abc","method":"GET","path":"/projects","status":200,"durationMs":1,"userId":2}
```

Rules:

- `level` is `error` when `status` is 500 or more, `warn` when `status` is 400 to
  499, and `info` otherwise.
- `path` is the path only. It excludes the query string.
- `durationMs` is an integer of 0 or more. Its value is never asserted.
- `userId` is the authenticated user id, or `null` when there is none.
- The key order above is the required order.
- Nothing else may be written to stdout. Framework banners go to stderr, or get
  silenced.

## Row-level authorization

A `user` role sees and touches only the projects it owns, and only the tasks
inside those projects. An `admin` role sees everything.

Applied as:

- `GET /projects` lists only owned projects for a `user`, and all for an `admin`.
- `GET /projects/{id}`, `GET /projects/{id}/tasks`,
  `POST /projects/{id}/tasks`, `GET /tasks/{id}`, `PUT /tasks/{id}`,
  `PATCH /tasks/{id}/status` and `DELETE /tasks/{id}` return `403 forbidden`
  when the project exists but the caller is a `user` who does not own it.
- The `404` check runs **before** the `403` check. An unknown id is `404`, even
  for a caller who would be forbidden.

## Pagination and sorting

`GET /projects` and `GET /projects/{id}/tasks` accept four query parameters.

| Parameter | Default | Rule |
| --- | --- | --- |
| `limit` | `DEFAULT_LIMIT` | integer 1 to `MAX_LIMIT` |
| `offset` | `0` | integer 0 or more |
| `sort` | `id` | route-specific allowed set |
| `order` | `asc` | `asc` or `desc` |

Allowed `sort` values:

- `GET /projects`: `id`, `name`, `taskCount`
- `GET /projects/{id}/tasks`: `id`, `title`, `priority`, `score`, `status`

A bad value in any of the four is `422 validation_failed`, with every bad
parameter reported together.

The sort is stable. `id` ascending breaks every tie. A string sort compares
by code point. Sorting happens before the offset and limit are applied.

The response shape:

```json
{"items": [ ... ], "total": 12, "limit": 20, "offset": 0}
```

`total` is the count of all matching rows before pagination, after the
row-level filter.

## Endpoints

### `GET /health`

No authentication.

```
200 {"status": "ok", "projects": <count>, "tasks": <count>}
```

### `POST /auth/login`

No authentication. Body: `{"username": <string>, "password": <string>}`.
Both are required.

```
422 when username or password is empty or absent
401 invalid_credentials when no user matches both
200 {"token": "<opaque>", "userId": <int>, "role": "<role>"}
```

A second login for the same user creates a second, independent session. Both
tokens stay valid.

### `POST /auth/logout`

```
204 <no body>       and the token stops working
```

### `GET /me`

```
200 {"userId": <int>, "username": "<string>", "role": "<string>"}
```

### `GET /projects`

Paginated. Row-level filtered. Sort set: `id`, `name`, `taskCount`.

```
200 {"items": [<project>], "total": n, "limit": l, "offset": o}
```

A project serializes as
`{"id": .., "name": .., "ownerId": .., "taskCount": ..}`.

### `POST /projects`

**Admin only.** Body: `{"name": <string>, "ownerId": <int>}`.
`ownerId` defaults to the caller's id. It must be an existing user, reported on
field `ownerId` with message `ownerId is not a known user`.

```
403 for a user role
422 for a broken rule
409 conflict when the same ownerId already has a project with this name
201 <project>
```

### `GET /projects/{id}`

```
400 bad_request when id is not an integer
404 when unknown
403 when a user role does not own it
200 <project>
```

Every `{id}` path segment in this specification follows the same rule: a
non-integer is `400 bad_request`.

### `PATCH /projects/{id}`

**Admin only.** Body: `{"name": <string>}`. `name` is the only mutable field.
An absent `name` leaves it unchanged and is not an error.

```
200 <project>
409 conflict when the new name collides for the same ownerId
```

A name that collides with the project's own current name is not a conflict.

### `DELETE /projects/{id}`

**Admin only.** Deletes the project and every task inside it.

```
204 <no body>
```

### `GET /projects/{id}/tasks`

Paginated. Sort set: `id`, `title`, `priority`, `score`, `status`.

```
200 {"items": [<task>], "total": n, "limit": l, "offset": o}
```

A task serializes as `{"id": .., "projectId": .., "title": .., "priority": ..,
"status": .., "assigneeId": .., "score": ..}`.

### `POST /projects/{id}/tasks`

Body: `{"title": <string>, "priority": <int>, "assigneeId": <int|null>}`.
`priority` defaults to `0`, which fails validation. `assigneeId` defaults to
`null`. A new task always starts at status `todo`.

```
201 <task>
```

### `GET /tasks/{id}`

```
200 <task>
```

### `PUT /tasks/{id}`

Full replace. Body: `{"title": <string>, "priority": <int>,
"assigneeId": <int|null>}`. The `status` is **not** settable here; it keeps its
current value. `id` and `projectId` never change.

```
200 <task>
```

### `PATCH /tasks/{id}/status`

Body: `{"status": <string>}`. `status` is required.

The allowed transitions, and nothing else:

```
todo         -> in_progress
todo         -> archived
in_progress  -> todo
in_progress  -> done
done         -> archived
```

```
422 when status is absent or not one of the four values
409 invalid_transition when the value is valid but the move is not allowed
200 <task>
```

A move to the status it already holds is not allowed. It is
`409 invalid_transition`.

### `DELETE /tasks/{id}`

```
204 <no body>
```

### `GET /stats`

**Admin only.**

```
200 {
  "projects": <int>,
  "tasks": <int>,
  "users": <int>,
  "sessions": <int>,
  "byStatus": {"todo": <int>, "in_progress": <int>, "done": <int>, "archived": <int>},
  "avgScore": <float, 2 decimals, 0.0 when there is no task>,
  "topProjectName": <string|null>
}
```

`topProjectName` is the name of the project with the most tasks. The lowest
`id` wins a tie. It is `null` when no project exists.

`byStatus` always carries all four keys, including the zeros.

### Any other path

```
404 not_found
```

A wrong method on a known path is out of scope, exactly as in `SPEC.md`.
