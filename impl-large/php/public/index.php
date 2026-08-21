<?php
// Task Service, large tier — the Slim app: routing, middleware and the entry point.

declare(strict_types=1);

require __DIR__ . '/../vendor/autoload.php';
require __DIR__ . '/../src/Domain.php';
require __DIR__ . '/../src/Store.php';
require __DIR__ . '/../src/Service.php';

use Psr\Http\Message\ResponseFactoryInterface;
use Psr\Http\Message\ResponseInterface as Response;
use Psr\Http\Message\ServerRequestInterface as Request;
use Psr\Http\Server\RequestHandlerInterface as RequestHandler;
use Slim\Factory\AppFactory;
use Slim\Routing\RouteContext;

$store = null;
$requestId = '';
$userId = null;
$quotaRemaining = null;
$replayed = false;

// ---------------------------------------------------------------------- helpers

function writeJson(Response $response, int $status, mixed $body): Response
{
    $response->getBody()->write((string) json_encode($body, JSON_UNESCAPED_SLASHES));
    return $response->withHeader('Content-Type', 'application/json')->withStatus($status);
}

/** A single-resource body carries its version, so the ETag comes for free. */
function responded(Response $response, int $status, array $body): Response
{
    $written = writeJson($response, $status, $body);
    return isset($body['version'])
        ? $written->withHeader('ETag', (string) $body['version'])
        : $written;
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
function begin(Request $request, bool $admin = false): array
{
    global $store, $userId, $quotaRemaining;
    [$user, $session] = authenticate($store, $request->getHeaderLine('Authorization'));
    $userId = $user->id;
    $quotaRemaining = chargeQuota($user, $session);
    if ($admin) {
        requireAdmin($user);
    }
    return [$store, $user, $session];
}

function readBody(Request $request): array
{
    $raw = trim((string) $request->getBody());
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
function readPage(Request $request, array $allowed): array
{
    $query = $request->getQueryParams();
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

function ifMatch(Request $request, int $version): void
{
    checkIfMatch($request->getHeaderLine('If-Match'), $version);
}

/** Run $produce once per Idempotency-Key, then replay the recorded outcome. */
function idempotent(Request $request, Response $response, Session $session,
    callable $produce): Response
{
    global $store, $replayed;
    $key = $request->getHeaderLine('Idempotency-Key');
    if ($key === '') {
        [$status, $body] = $produce();
        return responded($response, $status, $body);
    }
    $slot = $session->token . "\n" . $key;
    if (isset($store->idempotency[$slot])) {
        $replayed = true;
        $record = $store->idempotency[$slot];
        return responded($response, $record['status'], $record['body']);
    }
    try {
        [$status, $body] = $produce();
    } catch (AppError $error) {
        $store->idempotency[$slot] = ['status' => $error->status, 'body' => envelope($error)];
        throw $error;
    }
    $store->idempotency[$slot] = ['status' => $status, 'body' => $body];
    return responded($response, $status, $body);
}

/** @param array<int, Task> $rows */
function taskFilters(Request $request, array $rows): array
{
    $query = $request->getQueryParams();
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

// ------------------------------------------------------------------- middleware

function observe(Request $request, RequestHandler $handler,
    ResponseFactoryInterface $factory): Response
{
    global $store, $requestId, $userId, $quotaRemaining, $replayed;
    $store = Store::load();
    $requestId = $request->getHeaderLine('X-Request-Id') ?: bin2hex(random_bytes(6));
    $userId = null;
    $quotaRemaining = null;
    $replayed = false;
    $auditBefore = count($store->audit);
    $started = hrtime(true);
    try {
        $response = $handler->handle($request);
    } catch (AppError $error) {
        $response = writeJson($factory->createResponse(), $error->status, envelope($error));
    } catch (Throwable $error) {
        $response = writeJson($factory->createResponse(), 500,
            envelope(new AppError(500, 'internal_error', $error->getMessage())));
    }
    $status = $response->getStatusCode();
    $route = $request->getAttribute(RouteContext::ROUTE);
    $store->countRequest($request->getMethod() . ' '
        . ($route === null ? 'unmatched' : $route->getPattern()), $status);
    file_put_contents('php://stdout', json_encode([
        'level' => $status >= 500 ? 'error' : ($status >= 400 ? 'warn' : 'info'),
        'requestId' => $requestId,
        'method' => $request->getMethod(),
        'path' => $request->getUri()->getPath(),
        'status' => $status,
        'durationMs' => intdiv(hrtime(true) - $started, 1000000),
        'userId' => $userId,
        'quotaRemaining' => $quotaRemaining,
        'auditSeq' => count($store->audit) - $auditBefore,
    ], JSON_UNESCAPED_SLASHES) . "\n");
    $store->save();
    $response = $response->withHeader('X-Request-Id', $requestId);
    if ($quotaRemaining !== null) {
        $response = $response->withHeader('X-Quota-Remaining', (string) $quotaRemaining);
    }
    if ($replayed) {
        $response = $response->withHeader('Idempotency-Replayed', 'true');
    }
    return $response;
}

$app = AppFactory::create();

$app->add(fn (Request $request, RequestHandler $handler): Response
    => observe($request, $handler, $app->getResponseFactory()));
$app->addRoutingMiddleware();

// ------------------------------------------------------------------ health, auth

$app->get('/health', function (Request $request, Response $response): Response {
    global $store;
    return writeJson($response, 200, ['status' => 'ok',
        'projects' => count(array_filter($store->projects,
            fn (Project $project) => !$project->deleted)),
        'tasks' => count(array_filter($store->tasks, fn (Task $task) => !$task->deleted)),
        'comments' => count($store->comments)]);
});

$app->post('/auth/login', function (Request $request, Response $response): Response {
    global $store;
    $body = readBody($request);
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
    return writeJson($response, 200, ['token' => $token, 'userId' => $user->id,
        'role' => $user->role]);
});

$app->post('/auth/logout', function (Request $request, Response $response): Response {
    [$store, $user, $session] = begin($request);
    unset($store->sessions[$session->token]);
    return $response->withStatus(204);
});

$app->get('/me', function (Request $request, Response $response): Response {
    [$store, $user] = begin($request);
    return writeJson($response, 200, ['userId' => $user->id, 'username' => $user->username,
        'role' => $user->role]);
});

// ------------------------------------------------------------------------ users

$app->get('/users', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    [$limit, $offset, $sort, $order] = readPage($request, USER_SORTS);
    $rows = [];
    foreach ($store->users as $user) {
        if (!$user->deleted) {
            $rows[] = serializeUser($user);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/users', function (Request $request, Response $response): Response {
    [$store, $actor, $session] = begin($request, true);
    $body = readBody($request);
    return idempotent($request, $response, $session,
        function () use ($store, $actor, $body): array {
            $user = createUser($store, $actor, text($body, 'username'), text($body, 'password'),
                $body['role'] ?? 'user', $body['quota'] ?? DEFAULT_QUOTA);
            return [201, serializeUser($user)];
        });
});

$app->get('/users/{id}', function (Request $request, Response $response, array $args): Response {
    [$store] = begin($request, true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    return responded($response, 200, serializeUser($user));
});

$app->patch('/users/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request, true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    ifMatch($request, $user->version);
    updateUser($store, $actor, $user, readBody($request));
    return responded($response, 200, serializeUser($user));
});

$app->delete('/users/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request, true);
    $user = $store->findUser(parseId($args['id']));
    if ($user === null) {
        throw notFound();
    }
    ifMatch($request, $user->version);
    deleteUser($store, $actor, $user);
    return responded($response, 200, serializeUser($user));
});

// --------------------------------------------------------------------- projects

$app->get('/projects', function (Request $request, Response $response): Response {
    [$store, $user] = begin($request);
    $include = checkIncludeDeleted($request->getQueryParams()['includeDeleted'] ?? null, $user);
    [$limit, $offset, $sort, $order] = readPage($request, PROJECT_SORTS);
    $rows = [];
    foreach (visibleProjects($store, $user, $include) as $project) {
        $rows[] = serializeProject($store, $project);
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/projects', function (Request $request, Response $response): Response {
    [$store, $actor, $session] = begin($request, true);
    $body = readBody($request);
    return idempotent($request, $response, $session,
        function () use ($store, $actor, $body): array {
            $project = createProject($store, $actor, text($body, 'name'),
                whole($body, 'ownerId', $actor->id));
            return [201, serializeProject($store, $project)];
        });
});

$app->get('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $user] = begin($request);
    $project = reachableProject($store, parseId($args['id']), $user);
    return responded($response, 200, serializeProject($store, $project));
});

$app->patch('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request, true);
    $project = reachableProject($store, parseId($args['id']), $actor);
    ifMatch($request, $project->version);
    $body = readBody($request);
    if (array_key_exists('name', $body)) {
        renameProject($store, $actor, $project, text($body, 'name'));
    }
    return responded($response, 200, serializeProject($store, $project));
});

$app->delete('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request, true);
    $project = reachableProject($store, parseId($args['id']), $actor);
    ifMatch($request, $project->version);
    deleteProject($store, $actor, $project);
    return responded($response, 200, serializeProject($store, $project));
});

$app->post('/projects/{id}/restore', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request, true);
    $project = reachableProject($store, parseId($args['id']), $actor, true);
    ifMatch($request, $project->version);
    restoreProject($store, $actor, $project);
    return responded($response, 200, serializeProject($store, $project));
});

// ------------------------------------------------------------------------ tasks

$app->get('/tasks', function (Request $request, Response $response): Response {
    [$store, $user] = begin($request);
    $include = checkIncludeDeleted($request->getQueryParams()['includeDeleted'] ?? null, $user);
    [$limit, $offset, $sort, $order] = readPage($request, TASK_SORTS);
    $rows = [];
    foreach (taskFilters($request, visibleTasks($store, $user, $include)) as $task) {
        $rows[] = serializeTask($task, $user->role);
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->get('/projects/{id}/tasks', function (Request $request, Response $response,
    array $args): Response {
    [$store, $user] = begin($request);
    $project = reachableProject($store, parseId($args['id']), $user);
    [$limit, $offset, $sort, $order] = readPage($request, TASK_SORTS);
    $rows = [];
    foreach ($store->liveTasksOf($project->id) as $task) {
        $rows[] = serializeTask($task, $user->role);
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/projects/{id}/tasks', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor, $session] = begin($request);
    $project = reachableProject($store, parseId($args['id']), $actor);
    $body = readBody($request);
    return idempotent($request, $response, $session,
        function () use ($store, $actor, $project, $body): array {
            $errors = [];
            $note = readNote($actor, $body, $errors, '');
            $task = createTask($store, $actor, $project, text($body, 'title'),
                whole($body, 'priority', 0), whole($body, 'assigneeId', null), $note, $errors);
            return [201, serializeTask($task, $actor->role)];
        });
});

$app->post('/tasks/bulk', function (Request $request, Response $response): Response {
    [$store, $actor] = begin($request);
    $body = readBody($request);
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
    return writeJson($response, 200, ['results' => $results]);
});

$app->get('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    [$store, $user] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $user);
    return responded($response, 200, serializeTask($task, $user->role));
});

$app->put('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    [$store, $actor] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($request, $task->version);
    $body = readBody($request);
    $errors = [];
    $note = readNote($actor, $body, $errors, $task->internalNote);
    replaceTask($store, $actor, $task, text($body, 'title'), whole($body, 'priority', 0),
        whole($body, 'assigneeId', null), $note, $errors);
    return responded($response, 200, serializeTask($task, $actor->role));
});

$app->patch('/tasks/{id}/status', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($request, $task->version);
    $body = readBody($request);
    moveStatus($store, $actor, $task, $body['status'] ?? null);
    return responded($response, 200, serializeTask($task, $actor->role));
});

$app->delete('/tasks/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $actor);
    ifMatch($request, $task->version);
    deleteTask($store, $actor, $task);
    return responded($response, 200, serializeTask($task, $actor->role));
});

$app->post('/tasks/{id}/restore', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $actor, true);
    ifMatch($request, $task->version);
    restoreTask($store, $actor, $task);
    return responded($response, 200, serializeTask($task, $actor->role));
});

// --------------------------------------------------------------------- comments

$app->get('/tasks/{id}/comments', function (Request $request, Response $response,
    array $args): Response {
    [$store, $user] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $user);
    [$limit, $offset, $sort, $order] = readPage($request, COMMENT_SORTS);
    $rows = [];
    foreach ($store->comments as $comment) {
        if ($comment->taskId === $task->id) {
            $rows[] = serializeComment($comment);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/tasks/{id}/comments', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor, $session] = begin($request);
    $task = reachableTask($store, parseId($args['id']), $actor);
    $body = readBody($request);
    return idempotent($request, $response, $session,
        function () use ($store, $actor, $task, $body): array {
            $comment = createComment($store, $actor, $task, text($body, 'body'));
            return [201, serializeComment($comment)];
        });
});

$app->delete('/comments/{id}', function (Request $request, Response $response,
    array $args): Response {
    [$store, $actor] = begin($request);
    $comment = $store->findComment(parseId($args['id']));
    if ($comment === null) {
        throw notFound();
    }
    reachableTask($store, $comment->taskId, $actor, true);
    removeComment($store, $actor, $comment);
    return $response->withStatus(204);
});

// ---------------------------------------------------- search, reports, telemetry

$app->get('/search', function (Request $request, Response $response): Response {
    [$store, $user] = begin($request);
    $query = $request->getQueryParams()['q'] ?? '';
    if (!is_string($query) || $query === '') {
        throw invalid([fail('q', 'q is required')]);
    }
    return writeJson($response, 200, search($store, $user, $query));
});

$app->get('/reports/workload', function (Request $request, Response $response): Response {
    [$store, $user] = begin($request);
    $groupBy = $request->getQueryParams()['groupBy'] ?? 'status';
    if (!in_array($groupBy, GROUP_BYS, true)) {
        throw invalid([fail('groupBy', 'groupBy is not valid')]);
    }
    return writeJson($response, 200, workload($store, $user, $groupBy));
});

$app->get('/audit', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    [$limit, $offset, $sort, $order] = readPage($request, SEQ_SORTS);
    $query = $request->getQueryParams();
    $rows = [];
    foreach ($store->audit as $entry) {
        if ((!isset($query['actorId']) || (string) $entry->actorId === $query['actorId'])
            && (!isset($query['resource']) || $entry->resource === $query['resource'])
            && (!isset($query['action']) || $entry->action === $query['action'])) {
            $rows[] = serializeAudit($entry);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->get('/outbox', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    [$limit, $offset, $sort, $order] = readPage($request, SEQ_SORTS);
    $wanted = $request->getQueryParams()['delivered'] ?? null;
    $rows = [];
    foreach ($store->outbox as $event) {
        if ($wanted === null || $event->delivered === ($wanted === 'true')) {
            $rows[] = serializeOutbox($event);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/outbox/flush', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    return writeJson($response, 200, ['flushed' => flushOutbox($store)]);
});

$app->get('/metrics', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    return writeJson($response, 200, metrics($store));
});

$app->get('/stats', function (Request $request, Response $response): Response {
    [$store] = begin($request, true);
    return writeJson($response, 200, stats($store));
});

$app->any('/{path:.*}', function (Request $request, Response $response): Response {
    throw notFound();
});

$app->run();
