<?php
// Task Service, mid tier — Slim implementation.

declare(strict_types=1);

require __DIR__ . '/../vendor/autoload.php';

use Psr\Http\Message\ResponseFactoryInterface;
use Psr\Http\Message\ResponseInterface as Response;
use Psr\Http\Message\ServerRequestInterface as Request;
use Psr\Http\Server\RequestHandlerInterface as RequestHandler;
use Slim\Factory\AppFactory;

const MAX_TITLE_LENGTH = 80;
const MAX_NAME_LENGTH = 60;
const MIN_PRIORITY = 1;
const MAX_PRIORITY = 5;
const DEFAULT_LIMIT = 20;
const MAX_LIMIT = 100;
const STORE_PATH = __DIR__ . '/../store.json';

const STATUS_BONUS = ['todo' => 0, 'in_progress' => 3, 'done' => 5, 'archived' => 0];
const TRANSITIONS = ['todo->in_progress', 'todo->archived', 'in_progress->todo',
    'in_progress->done', 'done->archived'];
const PROJECT_SORTS = ['id', 'name', 'taskCount'];
const TASK_SORTS = ['id', 'title', 'priority', 'score', 'status'];

final class User
{
    public function __construct(
        public int $id,
        public string $username,
        public string $password,
        public string $role,
    ) {
    }
}

final class Project
{
    public function __construct(
        public int $id,
        public string $name,
        public int $ownerId,
    ) {
    }
}

final class Task
{
    public function __construct(
        public int $id,
        public int $projectId,
        public string $title,
        public int $priority,
        public string $status,
        public ?int $assigneeId,
        public int $score,
    ) {
    }
}

final class AppError extends Exception
{
    /** @param list<array{field: string, message: string}> $details */
    public function __construct(
        public int $status,
        public $code,
        public $message,
        public array $details = [],
    ) {
    }
}

/**
 * PHP starts a fresh process for every request, so the state cannot stay in
 * memory. The store reads and writes one JSON file instead. It carries the
 * sessions, the projects, the tasks and both counters, so every handler that
 * changes one of them must call save(). The seeded users never change, so the
 * store builds them in code and leaves them out of the file.
 */
final class Store
{
    /** @var array<int, User> */
    public array $users;
    /** @var array<string, int> */
    public array $sessions = [];
    /** @var array<int, Project> */
    public array $projects = [];
    /** @var array<int, Task> */
    public array $tasks = [];
    public int $nextProjectId = 1;
    public int $nextTaskId = 1;

    public function __construct()
    {
        $this->users = [
            1 => new User(1, 'admin', 'admin-secret', 'admin'),
            2 => new User(2, 'alice', 'alice-secret', 'user'),
            3 => new User(3, 'bob', 'bob-secret', 'user'),
        ];
    }

    public static function load(): self
    {
        $store = new self();
        if (!file_exists(STORE_PATH)) {
            return $store;
        }
        $raw = json_decode((string) file_get_contents(STORE_PATH), true);
        $store->sessions = $raw['sessions'];
        $store->nextProjectId = $raw['nextProjectId'];
        $store->nextTaskId = $raw['nextTaskId'];
        foreach ($raw['projects'] as $item) {
            $store->projects[$item['id']] = new Project($item['id'], $item['name'],
                $item['ownerId']);
        }
        foreach ($raw['tasks'] as $item) {
            $store->tasks[$item['id']] = new Task($item['id'], $item['projectId'],
                $item['title'], $item['priority'], $item['status'], $item['assigneeId'],
                $item['score']);
        }
        return $store;
    }

    public function save(): void
    {
        ksort($this->projects);
        ksort($this->tasks);
        file_put_contents(STORE_PATH, (string) json_encode([
            'sessions' => $this->sessions,
            'projects' => array_values($this->projects),
            'tasks' => array_values($this->tasks),
            'nextProjectId' => $this->nextProjectId,
            'nextTaskId' => $this->nextTaskId,
        ]));
    }
}

$userId = null;

function computeScore(int $priority, string $status): int
{
    $baseScore = $priority * 10;
    return $baseScore + STATUS_BONUS[$status];
}

function taskCount(Store $store, int $projectId): int
{
    return count(array_filter($store->tasks, fn (Task $task) => $task->projectId === $projectId));
}

function serializeProject(Store $store, Project $project): array
{
    return ['id' => $project->id, 'name' => $project->name, 'ownerId' => $project->ownerId,
        'taskCount' => taskCount($store, $project->id)];
}

function serializeTask(Task $task): array
{
    return ['id' => $task->id, 'projectId' => $task->projectId, 'title' => $task->title,
        'priority' => $task->priority, 'status' => $task->status,
        'assigneeId' => $task->assigneeId, 'score' => $task->score];
}

function badRequest(): AppError
{
    return new AppError(400, 'bad_request', 'the request is malformed');
}

function notFound(): AppError
{
    return new AppError(404, 'not_found', 'the resource does not exist');
}

function forbidden(): AppError
{
    return new AppError(403, 'forbidden', 'you may not access this resource');
}

function conflict(): AppError
{
    return new AppError(409, 'conflict', 'the resource already exists');
}

function invalid(array $details): AppError
{
    usort($details, fn (array $a, array $b) => [$a['field'], $a['message']]
        <=> [$b['field'], $b['message']]);
    return new AppError(422, 'validation_failed', 'the request body is not valid', $details);
}

/** @return array{field: string, message: string} */
function fail(string $field, string $message): array
{
    return ['field' => $field, 'message' => $message];
}

function writeJson(Response $response, int $status, mixed $body): Response
{
    $response->getBody()->write((string) json_encode($body));
    return $response->withHeader('Content-Type', 'application/json')->withStatus($status);
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

function readInt(array $body, string $field, ?int $default): ?int
{
    $value = array_key_exists($field, $body) ? $body[$field] : $default;
    if ($value === null || is_int($value)) {
        return $value;
    }
    throw badRequest();
}

function readString(array $body, string $field, array &$errors, int $maxLength,
    bool $required): string
{
    $value = array_key_exists($field, $body) ? $body[$field] : '';
    if (!is_string($value)) {
        throw badRequest();
    }
    if ($value === '') {
        if ($required) {
            $errors[] = fail($field, "$field is required");
        }
    } elseif (mb_strlen($value) > $maxLength) {
        $errors[] = fail($field, "$field is too long");
    }
    return $value;
}

function readPriority(array $body, array &$errors): int
{
    $value = readInt($body, 'priority', 0);
    if ($value === null || $value < MIN_PRIORITY || $value > MAX_PRIORITY) {
        $errors[] = fail('priority', 'priority is out of range');
    }
    return $value ?? 0;
}

function readUserRef(Store $store, array $body, string $field, array &$errors,
    ?int $default): ?int
{
    $value = readInt($body, $field, $default);
    if ($value !== null && !isset($store->users[$value])) {
        $errors[] = fail($field, "$field is not a known user");
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
    $sort = $query['sort'] ?? 'id';
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

function paginate(array $rows, int $limit, int $offset, string $sort, string $order): array
{
    usort($rows, fn (array $a, array $b) => $a['id'] <=> $b['id']);
    usort($rows, fn (array $a, array $b) => $order === 'desc'
        ? $b[$sort] <=> $a[$sort]
        : $a[$sort] <=> $b[$sort]);
    return ['items' => array_values(array_slice($rows, $offset, $limit)),
        'total' => count($rows), 'limit' => $limit, 'offset' => $offset];
}

function authenticate(Store $store, Request $request): User
{
    global $userId;
    $header = $request->getHeaderLine('Authorization');
    $token = str_starts_with($header, 'Bearer ') ? substr($header, 7) : '';
    if (!isset($store->sessions[$token])) {
        throw new AppError(401, 'unauthorized', 'authentication is required');
    }
    $userId = $store->sessions[$token];
    return $store->users[$userId];
}

function requireAdmin(User $user): void
{
    if ($user->role !== 'admin') {
        throw forbidden();
    }
}

function reachableProject(Store $store, int $projectId, User $user): Project
{
    $project = $store->projects[$projectId] ?? null;
    if ($project === null) {
        throw notFound();
    }
    if ($user->role !== 'admin' && $project->ownerId !== $user->id) {
        throw forbidden();
    }
    return $project;
}

function reachableTask(Store $store, int $taskId, User $user): Task
{
    $task = $store->tasks[$taskId] ?? null;
    if ($task === null) {
        throw notFound();
    }
    reachableProject($store, $task->projectId, $user);
    return $task;
}

function observe(Request $request, RequestHandler $handler,
    ResponseFactoryInterface $factory): Response
{
    global $userId;
    $requestId = $request->getHeaderLine('X-Request-Id') ?: bin2hex(random_bytes(6));
    $userId = null;
    $started = hrtime(true);
    try {
        $response = $handler->handle($request);
    } catch (AppError $error) {
        $response = writeJson($factory->createResponse(), $error->status, ['error' => [
            'code' => $error->code, 'message' => $error->message,
            'requestId' => $requestId, 'details' => $error->details,
        ]]);
    }
    $status = $response->getStatusCode();
    file_put_contents('php://stdout', json_encode([
        'level' => $status >= 500 ? 'error' : ($status >= 400 ? 'warn' : 'info'),
        'requestId' => $requestId,
        'method' => $request->getMethod(),
        'path' => $request->getUri()->getPath(),
        'status' => $status,
        'durationMs' => intdiv(hrtime(true) - $started, 1000000),
        'userId' => $userId,
    ], JSON_UNESCAPED_SLASHES) . "\n");
    return $response->withHeader('X-Request-Id', $requestId);
}

$app = AppFactory::create();

$app->add(fn (Request $request, RequestHandler $handler): Response
    => observe($request, $handler, $app->getResponseFactory()));

$app->get('/health', function (Request $request, Response $response): Response {
    $store = Store::load();
    return writeJson($response, 200, ['status' => 'ok', 'projects' => count($store->projects),
        'tasks' => count($store->tasks)]);
});

$app->post('/auth/login', function (Request $request, Response $response): Response {
    $store = Store::load();
    $body = readBody($request);
    $errors = [];
    $username = readString($body, 'username', $errors, MAX_NAME_LENGTH, true);
    $password = readString($body, 'password', $errors, MAX_NAME_LENGTH, true);
    if ($errors !== []) {
        throw invalid($errors);
    }
    foreach ($store->users as $candidate) {
        if ($candidate->username === $username && $candidate->password === $password) {
            $token = bin2hex(random_bytes(16));
            $store->sessions[$token] = $candidate->id;
            $store->save();
            return writeJson($response, 200, ['token' => $token, 'userId' => $candidate->id,
                'role' => $candidate->role]);
        }
    }
    throw new AppError(401, 'invalid_credentials', 'the username or password is wrong');
});

$app->post('/auth/logout', function (Request $request, Response $response): Response {
    $store = Store::load();
    authenticate($store, $request);
    unset($store->sessions[substr($request->getHeaderLine('Authorization'), 7)]);
    $store->save();
    return $response->withStatus(204);
});

$app->get('/me', function (Request $request, Response $response): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    return writeJson($response, 200, ['userId' => $user->id, 'username' => $user->username,
        'role' => $user->role]);
});

$app->get('/projects', function (Request $request, Response $response): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    [$limit, $offset, $sort, $order] = readPage($request, PROJECT_SORTS);
    $rows = [];
    foreach ($store->projects as $project) {
        if ($user->role === 'admin' || $project->ownerId === $user->id) {
            $rows[] = serializeProject($store, $project);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/projects', function (Request $request, Response $response): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    requireAdmin($user);
    $body = readBody($request);
    $errors = [];
    $name = readString($body, 'name', $errors, MAX_NAME_LENGTH, true);
    $ownerId = readUserRef($store, $body, 'ownerId', $errors, $user->id);
    if ($errors !== []) {
        throw invalid($errors);
    }
    foreach ($store->projects as $other) {
        if ($other->ownerId === $ownerId && $other->name === $name) {
            throw conflict();
        }
    }
    $project = new Project($store->nextProjectId, $name, (int) $ownerId);
    $store->projects[$project->id] = $project;
    $store->nextProjectId += 1;
    $store->save();
    return writeJson($response, 201, serializeProject($store, $project));
});

$app->get('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $project = reachableProject($store, parseId($args['id']), $user);
    return writeJson($response, 200, serializeProject($store, $project));
});

$app->patch('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    requireAdmin($user);
    $project = reachableProject($store, parseId($args['id']), $user);
    $body = readBody($request);
    if (array_key_exists('name', $body)) {
        $errors = [];
        $name = readString($body, 'name', $errors, MAX_NAME_LENGTH, true);
        if ($errors !== []) {
            throw invalid($errors);
        }
        foreach ($store->projects as $other) {
            if ($other->ownerId === $project->ownerId && $other->name === $name
                && $other->id !== $project->id) {
                throw conflict();
            }
        }
        $project->name = $name;
        $store->save();
    }
    return writeJson($response, 200, serializeProject($store, $project));
});

$app->delete('/projects/{id}', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    requireAdmin($user);
    $project = reachableProject($store, parseId($args['id']), $user);
    foreach ($store->tasks as $task) {
        if ($task->projectId === $project->id) {
            unset($store->tasks[$task->id]);
        }
    }
    unset($store->projects[$project->id]);
    $store->save();
    return $response->withStatus(204);
});

$app->get('/projects/{id}/tasks', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $project = reachableProject($store, parseId($args['id']), $user);
    [$limit, $offset, $sort, $order] = readPage($request, TASK_SORTS);
    $rows = [];
    foreach ($store->tasks as $task) {
        if ($task->projectId === $project->id) {
            $rows[] = serializeTask($task);
        }
    }
    return writeJson($response, 200, paginate($rows, $limit, $offset, $sort, $order));
});

$app->post('/projects/{id}/tasks', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $project = reachableProject($store, parseId($args['id']), $user);
    $body = readBody($request);
    $errors = [];
    $title = readString($body, 'title', $errors, MAX_TITLE_LENGTH, true);
    $priority = readPriority($body, $errors);
    $assigneeId = readUserRef($store, $body, 'assigneeId', $errors, null);
    if ($errors !== []) {
        throw invalid($errors);
    }
    $task = new Task($store->nextTaskId, $project->id, $title, $priority, 'todo', $assigneeId,
        computeScore($priority, 'todo'));
    $store->tasks[$task->id] = $task;
    $store->nextTaskId += 1;
    $store->save();
    return writeJson($response, 201, serializeTask($task));
});

$app->get('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    return writeJson($response, 200, serializeTask(reachableTask($store, parseId($args['id']),
        $user)));
});

$app->put('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $task = reachableTask($store, parseId($args['id']), $user);
    $body = readBody($request);
    $errors = [];
    $title = readString($body, 'title', $errors, MAX_TITLE_LENGTH, true);
    $priority = readPriority($body, $errors);
    $assigneeId = readUserRef($store, $body, 'assigneeId', $errors, null);
    if ($errors !== []) {
        throw invalid($errors);
    }
    $task->title = $title;
    $task->priority = $priority;
    $task->assigneeId = $assigneeId;
    $task->score = computeScore($priority, $task->status);
    $store->save();
    return writeJson($response, 200, serializeTask($task));
});

$app->patch('/tasks/{id}/status', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $task = reachableTask($store, parseId($args['id']), $user);
    $body = readBody($request);
    $status = $body['status'] ?? null;
    if (!is_string($status) || !array_key_exists($status, STATUS_BONUS)) {
        throw invalid([fail('status', 'status is not valid')]);
    }
    if (!in_array("{$task->status}->{$status}", TRANSITIONS, true)) {
        throw new AppError(409, 'invalid_transition', 'the status change is not allowed');
    }
    $task->status = $status;
    $task->score = computeScore($task->priority, $status);
    $store->save();
    return writeJson($response, 200, serializeTask($task));
});

$app->delete('/tasks/{id}', function (Request $request, Response $response,
    array $args): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    $task = reachableTask($store, parseId($args['id']), $user);
    unset($store->tasks[$task->id]);
    $store->save();
    return $response->withStatus(204);
});

$app->get('/stats', function (Request $request, Response $response): Response {
    $store = Store::load();
    $user = authenticate($store, $request);
    requireAdmin($user);
    $byStatus = ['todo' => 0, 'in_progress' => 0, 'done' => 0, 'archived' => 0];
    $sumScore = 0;
    foreach ($store->tasks as $task) {
        $byStatus[$task->status] += 1;
        $sumScore += $task->score;
    }
    $total = count($store->tasks);
    $best = null;
    foreach ($store->projects as $project) {
        if ($best === null || taskCount($store, $project->id) > taskCount($store, $best->id)) {
            $best = $project;
        }
    }
    return writeJson($response, 200, [
        'projects' => count($store->projects),
        'tasks' => $total,
        'users' => count($store->users),
        'sessions' => count($store->sessions),
        'byStatus' => $byStatus,
        'avgScore' => $total === 0 ? 0.0 : round($sumScore / $total, 2),
        'topProjectName' => $best === null ? null : $best->name,
    ]);
});

$app->any('/{path:.*}', function (Request $request, Response $response): Response {
    throw notFound();
});

$app->run();
