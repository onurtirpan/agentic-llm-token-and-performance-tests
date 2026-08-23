<?php
// Task Service, mid tier — bare PHP implementation, no framework.

declare(strict_types=1);

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

function writeJson(int $status, mixed $body): void
{
    http_response_code($status);
    header('Content-Type: application/json');
    echo (string) json_encode($body);
}

function readBody(): array
{
    $raw = trim((string) file_get_contents('php://input'));
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
function readPage(array $allowed): array
{
    $query = $_GET;
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

function authenticate(Store $store): User
{
    global $userId;
    $header = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
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

function dispatch(array $routes, string $method, string $path): void
{
    foreach ($routes as [$verb, $pattern, $handler]) {
        if ($verb === $method && preg_match($pattern, $path, $match) === 1) {
            $handler(...array_slice($match, 1));
            return;
        }
    }
    throw notFound();
}

$routes = [
    ['GET', '#^/health$#', function (): void {
        $store = Store::load();
        writeJson(200, ['status' => 'ok', 'projects' => count($store->projects),
            'tasks' => count($store->tasks)]);
    }],

    ['POST', '#^/auth/login$#', function (): void {
        $store = Store::load();
        $body = readBody();
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
                writeJson(200, ['token' => $token, 'userId' => $candidate->id,
                    'role' => $candidate->role]);
                return;
            }
        }
        throw new AppError(401, 'invalid_credentials', 'the username or password is wrong');
    }],

    ['POST', '#^/auth/logout$#', function (): void {
        $store = Store::load();
        authenticate($store);
        unset($store->sessions[substr($_SERVER['HTTP_AUTHORIZATION'], 7)]);
        $store->save();
        http_response_code(204);
    }],

    ['GET', '#^/me$#', function (): void {
        $store = Store::load();
        $user = authenticate($store);
        writeJson(200, ['userId' => $user->id, 'username' => $user->username,
            'role' => $user->role]);
    }],

    ['GET', '#^/projects$#', function (): void {
        $store = Store::load();
        $user = authenticate($store);
        [$limit, $offset, $sort, $order] = readPage(PROJECT_SORTS);
        $rows = [];
        foreach ($store->projects as $project) {
            if ($user->role === 'admin' || $project->ownerId === $user->id) {
                $rows[] = serializeProject($store, $project);
            }
        }
        writeJson(200, paginate($rows, $limit, $offset, $sort, $order));
    }],

    ['POST', '#^/projects$#', function (): void {
        $store = Store::load();
        $user = authenticate($store);
        requireAdmin($user);
        $body = readBody();
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
        writeJson(201, serializeProject($store, $project));
    }],

    ['GET', '#^/projects/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $project = reachableProject($store, parseId($id), $user);
        writeJson(200, serializeProject($store, $project));
    }],

    ['PATCH', '#^/projects/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        requireAdmin($user);
        $project = reachableProject($store, parseId($id), $user);
        $body = readBody();
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
        writeJson(200, serializeProject($store, $project));
    }],

    ['DELETE', '#^/projects/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        requireAdmin($user);
        $project = reachableProject($store, parseId($id), $user);
        foreach ($store->tasks as $task) {
            if ($task->projectId === $project->id) {
                unset($store->tasks[$task->id]);
            }
        }
        unset($store->projects[$project->id]);
        $store->save();
        http_response_code(204);
    }],

    ['GET', '#^/projects/([^/]+)/tasks$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $project = reachableProject($store, parseId($id), $user);
        [$limit, $offset, $sort, $order] = readPage(TASK_SORTS);
        $rows = [];
        foreach ($store->tasks as $task) {
            if ($task->projectId === $project->id) {
                $rows[] = serializeTask($task);
            }
        }
        writeJson(200, paginate($rows, $limit, $offset, $sort, $order));
    }],

    ['POST', '#^/projects/([^/]+)/tasks$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $project = reachableProject($store, parseId($id), $user);
        $body = readBody();
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
        writeJson(201, serializeTask($task));
    }],

    ['GET', '#^/tasks/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        writeJson(200, serializeTask(reachableTask($store, parseId($id), $user)));
    }],

    ['PUT', '#^/tasks/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $task = reachableTask($store, parseId($id), $user);
        $body = readBody();
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
        writeJson(200, serializeTask($task));
    }],

    ['PATCH', '#^/tasks/([^/]+)/status$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $task = reachableTask($store, parseId($id), $user);
        $body = readBody();
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
        writeJson(200, serializeTask($task));
    }],

    ['DELETE', '#^/tasks/([^/]+)$#', function (string $id): void {
        $store = Store::load();
        $user = authenticate($store);
        $task = reachableTask($store, parseId($id), $user);
        unset($store->tasks[$task->id]);
        $store->save();
        http_response_code(204);
    }],

    ['GET', '#^/stats$#', function (): void {
        $store = Store::load();
        $user = authenticate($store);
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
        writeJson(200, [
            'projects' => count($store->projects),
            'tasks' => $total,
            'users' => count($store->users),
            'sessions' => count($store->sessions),
            'byStatus' => $byStatus,
            'avgScore' => $total === 0 ? 0.0 : round($sumScore / $total, 2),
            'topProjectName' => $best === null ? null : $best->name,
        ]);
    }],
];

$method = $_SERVER['REQUEST_METHOD'];
$path = explode('?', $_SERVER['REQUEST_URI'], 2)[0];
$requestId = ($_SERVER['HTTP_X_REQUEST_ID'] ?? '') ?: bin2hex(random_bytes(6));
$started = hrtime(true);
header('X-Request-Id: ' . $requestId);

try {
    dispatch($routes, $method, $path);
} catch (AppError $error) {
    writeJson($error->status, ['error' => [
        'code' => $error->code, 'message' => $error->message,
        'requestId' => $requestId, 'details' => $error->details,
    ]]);
}

$status = (int) http_response_code();
file_put_contents('php://stdout', json_encode([
    'level' => $status >= 500 ? 'error' : ($status >= 400 ? 'warn' : 'info'),
    'requestId' => $requestId,
    'method' => $method,
    'path' => $path,
    'status' => $status,
    'durationMs' => intdiv(hrtime(true) - $started, 1000000),
    'userId' => $userId,
], JSON_UNESCAPED_SLASHES) . "\n");
