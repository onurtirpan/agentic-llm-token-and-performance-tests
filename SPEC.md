# Task Service — shared specification

Every implementation must satisfy this document exactly. The conformance test in
`tools/conformance.py` checks all 22 cases against a running server.

## Rules of the experiment

1. Each language uses the framework a real team would pick (idiomatic frameworks).
2. Identifier names are the same in every language. Only the casing follows the
   native convention of the language. Example: `next_id` / `nextID` / `NextId`.
3. JSON keys are byte-identical in every language. They use `camelCase`.
4. The server listens on port 8080. It keeps all state in memory.
5. No database, no disk, no outbound network call.

## Constants

| Name | Value |
| --- | --- |
| `MAX_TITLE_LENGTH` | 80 |
| `MIN_PRIORITY` | 1 |
| `MAX_PRIORITY` | 5 |
| `PORT` | 8080 |

## Data model

```
Task
    id        int
    title     string
    priority  int
    done      bool
    score     int
```

The client never sends `id` or `score`. The server derives both.

## Derived value

```
computeScore(priority, done):
    baseScore = priority * 10
    if done: return baseScore
    return baseScore + 5
```

The local name is `baseScore`, not `base`, because `base` is a keyword in C#.

## Iteration order

The store holds the tasks by `id`. Every iteration runs in ascending `id` order.
An `id` always grows, so ascending `id` equals insertion order. This rule keeps
Go and C# deterministic, because their maps do not preserve insertion order.

## Two documented name exceptions

1. C# calls the entity `TaskItem`. The name `Task` collides with
   `System.Threading.Tasks.Task`.
2. Every language uses `baseScore` for the local in `computeScore`.

## Validation

```
validate(title, priority):
    if title is empty:                       return "title is required"
    if length(title) > MAX_TITLE_LENGTH:     return "title is too long"
    if priority < MIN_PRIORITY:              return "priority is out of range"
    if priority > MAX_PRIORITY:              return "priority is out of range"
    return null
```

`length` counts characters, not bytes.

## Endpoints

Every response body is JSON, except `204`, which sends no body.
Every error body is `{"error": "<message>"}`.

### `GET /health`

```
200 {"status": "ok", "count": <number of tasks>}
```

### `GET /tasks`

Optional query parameter `done`. The only accepted values are `true` and `false`.

```
if done is present and not "true" and not "false":
    400 {"error": "done must be true or false"}

selected = tasks that match the filter, or all tasks if done is absent
sort selected by score descending, then by id ascending

200 {"tasks": [<task>, ...], "total": <length of selected>}
```

### `GET /tasks/{id}`

```
if id is not an integer:  400 {"error": "invalid id"}
if id is unknown:         404 {"error": "task not found"}
200 <task>
```

### `POST /tasks`

Body: `{"title": <string>, "priority": <int>}`.
A missing `title` defaults to `""`. A missing `priority` defaults to `0`.

```
if the body is not valid JSON, or a field has the wrong type:
    400 {"error": "invalid json"}
error = validate(title, priority)
if error: 400 {"error": error}

id    = next_id
done  = false
score = computeScore(priority, false)
next_id = next_id + 1
201 <task>
```

### `PUT /tasks/{id}`

Body: `{"title": <string>, "priority": <int>, "done": <bool>}`.
A missing `done` defaults to `false`.

```
if id is not an integer:  400 {"error": "invalid id"}
if id is unknown:         404 {"error": "task not found"}
if the body is bad:       400 {"error": "invalid json"}
error = validate(title, priority)
if error: 400 {"error": error}

replace title, priority and done
score = computeScore(priority, done)
200 <task>
```

The `id` never changes.

### `DELETE /tasks/{id}`

```
if id is not an integer:  400 {"error": "invalid id"}
if id is unknown:         404 {"error": "task not found"}
204 <no body>
```

### `GET /stats`

```
total     = number of tasks
doneCount = number of tasks where done is true
openCount = total - doneCount
avgScore  = 0.0 if total is 0, else round(sum of score / total, 2)

topOpenTitle = null
best = null
for each task in insertion order:
    if task.done: continue
    if best is null or task.priority > best.priority: best = task
if best is not null: topOpenTitle = best.title

200 {"total": total, "doneCount": doneCount, "openCount": openCount,
     "avgScore": avgScore, "topOpenTitle": topOpenTitle}
```

The first task wins a tie on `priority`, because the loop uses `>`, not `>=`.

### Any other path

```
404 {"error": "not found"}
```

A wrong method on a known path is out of scope. Frameworks disagree there, and
the conformance test does not check it.

## Conformance cases

| # | Request | Expected |
| --- | --- | --- |
| 1 | `GET /health` | 200 `{"status":"ok","count":0}` |
| 2 | `POST /tasks {"title":"write spec","priority":3}` | 201 `id 1, score 35` |
| 3 | `POST /tasks {"title":"ship it","priority":5}` | 201 `id 2, score 55` |
| 4 | `POST /tasks {"title":"cleanup","priority":1}` | 201 `id 3, score 15` |
| 5 | `POST /tasks {"title":"","priority":3}` | 400 `title is required` |
| 6 | `POST /tasks {"title":"x","priority":9}` | 400 `priority is out of range` |
| 7 | `POST /tasks {"title":"<81 chars>","priority":3}` | 400 `title is too long` |
| 8 | `POST /tasks` with `{not json` | 400 `invalid json` |
| 9 | `GET /tasks/2` | 200 `score 55` |
| 10 | `GET /tasks/99` | 404 `task not found` |
| 11 | `GET /tasks/abc` | 400 `invalid id` |
| 12 | `PUT /tasks/3 {"title":"cleanup done","priority":2,"done":true}` | 200 `score 20` |
| 13 | `PUT /tasks/99 {...}` | 404 `task not found` |
| 14 | `GET /tasks` | 200 `total 3`, order `2, 1, 3` |
| 15 | `GET /tasks?done=true` | 200 `total 1`, order `3` |
| 16 | `GET /tasks?done=false` | 200 `total 2`, order `2, 1` |
| 17 | `GET /tasks?done=maybe` | 400 `done must be true or false` |
| 18 | `GET /stats` | 200 `total 3, doneCount 1, openCount 2, avgScore 36.67, topOpenTitle "ship it"` |
| 19 | `DELETE /tasks/1` | 204 empty |
| 20 | `DELETE /tasks/1` | 404 `task not found` |
| 21 | `GET /health` | 200 `count 2` |
| 22 | `GET /nope` | 404 `not found` |

Case 18 depends on cases 2, 3, 4 and 12. The scores are 35, 55 and 20.
The sum is 110. `110 / 3 = 36.6666...`, so `avgScore` is `36.67`.
