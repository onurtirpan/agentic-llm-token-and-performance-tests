<?php
// Task Service, large tier — bare PHP: the front controller, its dispatch and middleware.

declare(strict_types=1);

require __DIR__ . '/../src/Domain.php';
require __DIR__ . '/../src/Store.php';
require __DIR__ . '/../src/Service.php';

$store = null;
$requestId = '';
$userId = null;
$quotaRemaining = null;
$replayed = false;
$rawBody = '';

// ---------------------------------------------------------------------- helpers

function headerLine(string $name): string
{
    return (string) ($_SERVER['HTTP_' . strtoupper(str_replace('-', '_', $name))] ?? '');
}

function envelope(AppError $error): array
{
    global $requestId;
    return ['error' => ['code' => $error->code, 'message' => $error->message,
        'requestId' => $requestId, 'details' => $error->details]];
}

/**
 * Authenticate, charge the quota, then check the role. This order is fixed.
 *
 * @return array{Store, User, Session}
 */
function begin(bool $admin = false): array
{
    global $store, $userId, $quotaRemaining;
    [$user, $session] = authenticate($store, headerLine('Authorization'));
    $userId = $user->id;
    $quotaRemaining = chargeQuota($user, $session);
    if ($admin) {
        requireAdmin($user);
    }
    return [$store, $user, $session];
}

function readBody(): array
{
    global $rawBody;
    $raw = trim($rawBody);
    if ($raw === '') {
        return [];
    }
    $parsed = json_decode($raw);
    if (!$parsed instanceof stdClass) {
        throw badRequest();
    }
    return (array) $parsed;
}

function whole(array $body, string $field, ?int $default): ?int
{
    $value = array_key_exists($field, $body) ? $body[$field] : $default;
    if ($value === null || is_int($value)) {
        return $value;
    }
    throw badRequest();
}

function text(array $body, string $field, string $default = ''): string
{
    $value = array_key_exists($field, $body) ? $body[$field] : $default;
    if (!is_string($value)) {
        throw badRequest();
    }
    return $value;
}

function parseId(string $raw): int
{
    if (preg_match('/^-?\d+$/', $raw) !== 1) {
        throw badRequest();
    }
    return (int) $raw;
}

/** @return array{int, int, string, string} */
function readPage(array $allowed): array
{
    $query = $_GET;
    $errors = [];
    $limit = DEFAULT_LIMIT;
    $offset = 0;
    $sort = $query['sort'] ?? $allowed[0];
    $order = $query['order'] ?? 'asc';
    if (isset($query['limit'])) {
        $limit = preg_match('/^-?\d+$/', $query['limit']) === 1 ? (int) $query['limit'] : -1;
        if ($limit < 1 || $limit > MAX_LIMIT) {
            $errors[] = fail('limit', 'limit is out of range');
        }
    }
    if (isset($query['offset'])) {
        $offset = preg_match('/^-?\d+$/', $query['offset']) === 1 ? (int) $query['offset'] : -1;
        if ($offset < 0) {
            $errors[] = fail('offset', 'offset is out of range');
        }
    }
    if (!in_array($sort, $allowed, true)) {
        $errors[] = fail('sort', 'sort is not a valid field');
    }
    if ($order !== 'asc' && $order !== 'desc') {
        $errors[] = fail('order', 'order must be asc or desc');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    return [$limit, $offset, $sort, $order];
}

function ifMatch(int $version): void
{
    checkIfMatch(headerLine('If-Match'), $version);
}

/**
 * Run $produce once per Idempotency-Key, then replay the recorded outcome.
 *
 * @return array{int, array}
 */
function idempotent(Session $session, callable $produce): array
{
    global $store, $replayed;
    $key = headerLine('Idempotency-Key');
    if ($key === '') {
        return $produce();
    }
    $slot = $session->token . "\n" . $key;
    if (isset($store->idempotency[$slot])) {
        $replayed = true;
        $record = $store->idempotency[$slot];
        return [$record['status'], $record['body']];
    }
    try {
        [$status, $body] = $produce();
    } catch (AppError $error) {
        $store->idempotency[$slot] = ['status' => $error->status, 'body' => envelope($error)];
        throw $error;
    }
    $store->idempotency[$slot] = ['status' => $status, 'body' => $body];
    return [$status, $body];
}

/** @param array<int, Task> $rows */
function taskFilters(array $rows): array
{
    $query = $_GET;
    $errors = [];
    $status = $query['status'] ?? null;
    $assignee = $query['assigneeId'] ?? null;
    if ($status !== null && !in_array($status, STATUSES, true)) {
        $errors[] = fail('status', 'status is not valid');
    }
    if ($assignee !== null && preg_match('/^-?\d+$/', (string) $assignee) !== 1) {
        $errors[] = fail('assigneeId', 'assigneeId is not a known user');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    if ($status !== null) {
        $rows = array_filter($rows, fn (Task $task) => $task->status === $status);
    }
    if ($assignee !== null) {
        $rows = array_filter($rows, fn (Task $task) => $task->assigneeId === (int) $assignee);
    }
    return $rows;
}

function applyBulk(Store $store, User $actor, mixed $raw): array
{
    if (!$raw instanceof stdClass) {
        throw badRequest();
    }
    $item = (array) $raw;
    $operation = $item['op'] ?? null;
    if ($operation === 'create') {
        $project = reachableProject($store, whole($item, 'projectId', 0), $actor);
        $task = createTask($store, $actor, $project, text($item, 'title'),
            whole($item, 'priority', 0), null, '', []);
        return ['status' => 201, 'id' => $task->id, 'error' => null];
    }
    if ($operation === 'status') {
        $task = reachableTask($store, whole($item, 'id', 0), $actor);
        checkIfMatch((string) ($item['version'] ?? -1), $task->version);
        moveStatus($store, $actor, $task, $item['status'] ?? null);
        return ['status' => 200, 'id' => $task->id, 'error' => null];
    }
    if ($operation === 'delete') {
        $task = reachableTask($store, whole($item, 'id', 0), $actor);
        checkIfMatch((string) ($item['version'] ?? -1), $task->version);
        deleteTask($store, $actor, $task);
        return ['status' => 200, 'id' => $task->id, 'error' => null];
    }
    throw invalid([fail('op', 'op is not valid')]);
}

/**
 * Walk the table in order and return the pattern that matched, so the metrics
 * record `GET /projects/{id}` rather than `GET /projects/7`. A literal segment
 * wins over `{id}`, which is why `/tasks/bulk` sits above `/tasks/{id}`.
 *
 * @return array{string, ?callable, array<string, string>}
 */
function matchRoute(array $routes, string $method, string $path): array
{
    $given = explode('/', trim($path, '/'));
    foreach ($routes as [$verb, $pattern, $handler]) {
        $wanted = explode('/', trim($pattern, '/'));
        if ($verb !== $method || count($wanted) !== count($given)) {
            continue;
        }
        $args = [];
        foreach ($wanted as $index => $part) {
            if ($part === '{id}') {
                $args['id'] = $given[$index];
            } elseif ($part !== $given[$index]) {
                continue 2;
            }
        }
        return [$pattern, $handler, $args];
    }
    return ['unmatched', null, []];
}

// ------------------------------------------------------------------ the routes

$routes = [

// ------------------------------------------------------------- health and auth

['GET', '/health', function (): array {
    global $store;
    return [200, ['status' => 'ok',
        'projects' => count(array_filter($store->projects,
            fn (Project $project) => !$project->deleted)),
        'tasks' => count(array_filter($store->tasks, fn (Task $task) => !$task->deleted)),
        'comments' => count($store->comments)]];
}],

['POST', '/auth/login', function (): array {
    global $store;
    $body = readBody();
    $errors = [];
    $username = text($body, 'username');
    $password = text($body, 'password');
    if ($username === '') {
        $errors[] = fail('username', 'username is required');
    }
    if ($password === '') {
        $errors[] = fail('password', 'password is required');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    $token = bin2hex(random_bytes(16));
    $user = login($store, $username, $password, $token);
    return [200, ['token' => $token, 'userId' => $user->id, 'role' => $user->role]];
}],

['POST', '/auth/logout', function (): array {
    [$store, $user, $session] = begin();
    unset($store->sessions[$session->token]);
    return [204, null];
}],

['GET', '/me', function (): array {
    [$store, $user] = begin();
    return [200, ['userId' => $user->id, 'username' => $user->username,
        'role' => $user->role]];
}],

// ----------------------------------------------------------------------- users

['GET', '/users', function (): array {
    [$store] = begin(true);
    [$limit, $offset, $sort, $order] = readPage(USER_SORTS);
    $rows = [];
    foreach ($store->users as $user) {
        if (!$user->deleted) {
            $rows[] = serializeUser($user);
        }
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/users', function (): array {
    [$store, $actor, $session] = begin(true);
    $body = readBody();
    return idempotent($session, function () use ($store, $actor, $body): array {
        $user = createUser($store, $actor, text($body, 'username'), text($body, 'password'),
            $body['role'] ?? 'user', $body['quota'] ?? DEFAULT_QUOTA);
        return [201, serializeUser($user)];
    });
}],

['GET', '/users/{id}', function (array $args): array {
    [$store] = begin(true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    return [200, serializeUser($user)];
}],

['PATCH', '/users/{id}', function (array $args): array {
    [$store, $actor] = begin(true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    ifMatch($user->version);
    updateUser($store, $actor, $user, readBody());
    return [200, serializeUser($user)];
}],

['DELETE', '/users/{id}', function (array $args): array {
    [$store, $actor] = begin(true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    ifMatch($user->version);
    deleteUser($store, $actor, $user);
    return [200, serializeUser($user)];
}],

// -------------------------------------------------------------------- projects

['GET', '/projects', function (): array {
    [$store, $user] = begin();
    $include = checkIncludeDeleted($_GET['includeDeleted'] ?? null, $user);
    [$limit, $offset, $sort, $order] = readPage(PROJECT_SORTS);
    $rows = [];
    foreach (visibleProjects($store, $user, $include) as $project) {
        $rows[] = serializeProject($store, $project);
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/projects', function (): array {
    [$store, $actor, $session] = begin(true);
    $body = readBody();
    return idempotent($session, function () use ($store, $actor, $body): array {
        $project = createProject($store, $actor, text($body, 'name'),
            whole($body, 'ownerId', $actor->id));
        return [201, serializeProject($store, $project)];
    });
}],

['GET', '/projects/{id}', function (array $args): array {
    [$store, $user] = begin();
    $project = reachableProject($store, parseId($args['id']), $user);
    return [200, serializeProject($store, $project)];
}],

['PATCH', '/projects/{id}', function (array $args): array {
    [$store, $actor] = begin(true);
    $project = reachableProject($store, parseId($args['id']), $actor);
    ifMatch($project->version);
    $body = readBody();
    if (array_key_exists('name', $body)) {
        renameProject($store, $actor, $project, text($body, 'name'));
    }
    return [200, serializeProject($store, $project)];
}],

['DELETE', '/projects/{id}', function (array $args): array {
    [$store, $actor] = begin(true);
    $project = reachableProject($store, parseId($args['id']), $actor);
    ifMatch($project->version);
    deleteProject($store, $actor, $project);
    return [200, serializeProject($store, $project)];
}],

['POST', '/projects/{id}/restore', function (array $args): array {
    [$store, $actor] = begin(true);
    $project = reachableProject($store, parseId($args['id']), $actor, true);
    ifMatch($project->version);
    restoreProject($store, $actor, $project);
    return [200, serializeProject($store, $project)];
}],

['GET', '/projects/{id}/tasks', function (array $args): array {
    [$store, $user] = begin();
    $project = reachableProject($store, parseId($args['id']), $user);
    [$limit, $offset, $sort, $order] = readPage(TASK_SORTS);
    $rows = [];
    foreach ($store->liveTasksOf($project->id) as $task) {
        $rows[] = serializeTask($task, $user->role);
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/projects/{id}/tasks', function (array $args): array {
    [$store, $actor, $session] = begin();
    $project = reachableProject($store, parseId($args['id']), $actor);
    $body = readBody();
    return idempotent($session, function () use ($store, $actor, $project, $body): array {
        $errors = [];
        $note = readNote($actor, $body, $errors, '');
        $task = createTask($store, $actor, $project, text($body, 'title'),
            whole($body, 'priority', 0), whole($body, 'assigneeId', null), $note, $errors);
        return [201, serializeTask($task, $actor->role)];
    });
}],

// ----------------------------------------------------------------------- tasks

['GET', '/tasks', function (): array {
    [$store, $user] = begin();
    $include = checkIncludeDeleted($_GET['includeDeleted'] ?? null, $user);
    [$limit, $offset, $sort, $order] = readPage(TASK_SORTS);
    $rows = [];
    foreach (taskFilters(visibleTasks($store, $user, $include)) as $task) {
        $rows[] = serializeTask($task, $user->role);
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/tasks/bulk', function (): array {
    [$store, $actor] = begin();
    $body = readBody();
    $operations = $body['operations'] ?? null;
    checkBulkSize($operations);
    $results = [];
    foreach (array_values($operations) as $index => $item) {
        try {
            $results[] = ['index' => $index] + applyBulk($store, $actor, $item);
        } catch (AppError $error) {
            $results[] = ['index' => $index, 'status' => $error->status, 'id' => null,
                'error' => $error->code];
        }
    }
    return [200, ['results' => $results]];
}],

['GET', '/tasks/{id}', function (array $args): array {
    [$store, $user] = begin();
    $task = reachableTask($store, parseId($args['id']), $user);
    return [200, serializeTask($task, $user->role)];
}],

['PUT', '/tasks/{id}', function (array $args): array {
    [$store, $actor] = begin();
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($task->version);
    $body = readBody();
    $errors = [];
    $note = readNote($actor, $body, $errors, $task->internalNote);
    replaceTask($store, $actor, $task, text($body, 'title'), whole($body, 'priority', 0),
        whole($body, 'assigneeId', null), $note, $errors);
    return [200, serializeTask($task, $actor->role)];
}],

['PATCH', '/tasks/{id}/status', function (array $args): array {
    [$store, $actor] = begin();
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($task->version);
    $body = readBody();
    moveStatus($store, $actor, $task, $body['status'] ?? null);
    return [200, serializeTask($task, $actor->role)];
}],

['DELETE', '/tasks/{id}', function (array $args): array {
    [$store, $actor] = begin();
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($task->version);
    deleteTask($store, $actor, $task);
    return [200, serializeTask($task, $actor->role)];
}],

['POST', '/tasks/{id}/restore', function (array $args): array {
    [$store, $actor] = begin();
    $task = reachableTask($store, parseId($args['id']), $actor, true);
    ifMatch($task->version);
    restoreTask($store, $actor, $task);
    return [200, serializeTask($task, $actor->role)];
}],

// -------------------------------------------------------------------- comments

['GET', '/tasks/{id}/comments', function (array $args): array {
    [$store, $user] = begin();
    $task = reachableTask($store, parseId($args['id']), $user);
    [$limit, $offset, $sort, $order] = readPage(COMMENT_SORTS);
    $rows = [];
    foreach ($store->comments as $comment) {
        if ($comment->taskId === $task->id) {
            $rows[] = serializeComment($comment);
        }
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/tasks/{id}/comments', function (array $args): array {
    [$store, $actor, $session] = begin();
    $task = reachableTask($store, parseId($args['id']), $actor);
    $body = readBody();
    return idempotent($session, function () use ($store, $actor, $task, $body): array {
        $comment = createComment($store, $actor, $task, text($body, 'body'));
        return [201, serializeComment($comment)];
    });
}],

['DELETE', '/comments/{id}', function (array $args): array {
    [$store, $actor] = begin();
    $comment = $store->findComment(parseId($args['id']));
    if ($comment === null) {
        throw notFound();
    }
    reachableTask($store, $comment->taskId, $actor, true);
    removeComment($store, $actor, $comment);
    return [204, null];
}],

// --------------------------------------------------- search, reports, telemetry

['GET', '/search', function (): array {
    [$store, $user] = begin();
    $query = $_GET['q'] ?? '';
    if (!is_string($query) || $query === '') {
        throw invalid([fail('q', 'q is required')]);
    }
    return [200, search($store, $user, $query)];
}],

['GET', '/reports/workload', function (): array {
    [$store, $user] = begin();
    $groupBy = $_GET['groupBy'] ?? 'status';
    if (!in_array($groupBy, GROUP_BYS, true)) {
        throw invalid([fail('groupBy', 'groupBy is not valid')]);
    }
    return [200, workload($store, $user, $groupBy)];
}],

['GET', '/audit', function (): array {
    [$store] = begin(true);
    [$limit, $offset, $sort, $order] = readPage(SEQ_SORTS);
    $query = $_GET;
    $rows = [];
    foreach ($store->audit as $entry) {
        if ((!isset($query['actorId']) || (string) $entry->actorId === $query['actorId'])
            && (!isset($query['resource']) || $entry->resource === $query['resource'])
            && (!isset($query['action']) || $entry->action === $query['action'])) {
            $rows[] = serializeAudit($entry);
        }
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['GET', '/outbox', function (): array {
    [$store] = begin(true);
    [$limit, $offset, $sort, $order] = readPage(SEQ_SORTS);
    $wanted = $_GET['delivered'] ?? null;
    $rows = [];
    foreach ($store->outbox as $event) {
        if ($wanted === null || $event->delivered === ($wanted === 'true')) {
            $rows[] = serializeOutbox($event);
        }
    }
    return [200, paginate($rows, $limit, $offset, $sort, $order)];
}],

['POST', '/outbox/flush', function (): array {
    [$store] = begin(true);
    return [200, ['flushed' => flushOutbox($store)]];
}],

['GET', '/metrics', function (): array {
    [$store] = begin(true);
    return [200, metrics($store)];
}],

['GET', '/stats', function (): array {
    [$store] = begin(true);
    return [200, stats($store)];
}],

];

// ---------------------------------------------------------------- the dispatch

$store = Store::load();
$requestId = headerLine('X-Request-Id') ?: bin2hex(random_bytes(6));
$rawBody = (string) file_get_contents('php://input');
$method = (string) $_SERVER['REQUEST_METHOD'];
$path = explode('?', (string) $_SERVER['REQUEST_URI'], 2)[0];
$auditBefore = count($store->audit);
$started = hrtime(true);
[$route, $handler, $args] = matchRoute($routes, $method, $path);
try {
    if ($handler === null) {
        throw notFound();
    }
    [$status, $body] = $handler($args);
} catch (AppError $error) {
    [$status, $body] = [$error->status, envelope($error)];
} catch (Throwable $error) {
    [$status, $body] = [500,
        envelope(new AppError(500, 'internal_error', $error->getMessage()))];
}

$store->countRequest("$method $route", $status);
file_put_contents('php://stdout', json_encode([
    'level' => $status >= 500 ? 'error' : ($status >= 400 ? 'warn' : 'info'),
    'requestId' => $requestId,
    'method' => $method,
    'path' => $path,
    'status' => $status,
    'durationMs' => intdiv(hrtime(true) - $started, 1000000),
    'userId' => $userId,
    'quotaRemaining' => $quotaRemaining,
    'auditSeq' => count($store->audit) - $auditBefore,
], JSON_UNESCAPED_SLASHES) . "\n");
$store->save();

// Every header goes out before the first byte of the body, so build then send.
http_response_code($status);
header('X-Request-Id: ' . $requestId);
if ($quotaRemaining !== null) {
    header('X-Quota-Remaining: ' . $quotaRemaining);
}
if ($replayed) {
    header('Idempotency-Replayed: true');
}
if ($body !== null) {
    // A single-resource body carries its version, so the ETag comes for free.
    if (isset($body['version'])) {
        header('ETag: ' . $body['version']);
    }
    header('Content-Type: application/json');
    echo json_encode($body, JSON_UNESCAPED_SLASHES);
}
