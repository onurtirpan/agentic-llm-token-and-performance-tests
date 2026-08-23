<?php
// Task Service — bare PHP implementation, no framework.

declare(strict_types=1);

const MAX_TITLE_LENGTH = 80;
const MIN_PRIORITY = 1;
const MAX_PRIORITY = 5;
const STORE_PATH = __DIR__ . '/../store.json';

final class Task
{
    public function __construct(
        public int $id,
        public string $title,
        public int $priority,
        public bool $done,
        public int $score,
    ) {
    }
}

/**
 * PHP starts a fresh process for every request, so the state cannot stay in
 * memory. The store reads and writes one JSON file instead.
 */
final class Store
{
    /** @var array<int, Task> */
    public array $tasks = [];
    public int $nextId = 1;

    public static function load(): self
    {
        $store = new self();
        if (!file_exists(STORE_PATH)) {
            return $store;
        }
        $raw = json_decode((string) file_get_contents(STORE_PATH), true);
        $store->nextId = $raw['nextId'];
        foreach ($raw['tasks'] as $item) {
            $store->tasks[$item['id']] = new Task($item['id'], $item['title'],
                $item['priority'], $item['done'], $item['score']);
        }
        return $store;
    }

    public function save(): void
    {
        ksort($this->tasks);
        file_put_contents(STORE_PATH, (string) json_encode([
            'nextId' => $this->nextId,
            'tasks' => array_values($this->tasks),
        ]));
    }
}

function computeScore(int $priority, bool $done): int
{
    $baseScore = $priority * 10;
    return $done ? $baseScore : $baseScore + 5;
}

function validate(string $title, int $priority): ?string
{
    if ($title === '') {
        return 'title is required';
    }
    if (mb_strlen($title) > MAX_TITLE_LENGTH) {
        return 'title is too long';
    }
    if ($priority < MIN_PRIORITY || $priority > MAX_PRIORITY) {
        return 'priority is out of range';
    }
    return null;
}

function writeJson(int $status, mixed $body): void
{
    http_response_code($status);
    header('Content-Type: application/json');
    echo (string) json_encode($body);
}

function fail(int $status, string $message): void
{
    writeJson($status, ['error' => $message]);
}

function parseId(string $raw): ?int
{
    return preg_match('/^-?\d+$/', $raw) === 1 ? (int) $raw : null;
}

/** @return array{title: string, priority: int, done: bool}|null */
function readInput(): ?array
{
    $raw = json_decode((string) file_get_contents('php://input'), true);
    if (!is_array($raw)) {
        return null;
    }
    $title = $raw['title'] ?? '';
    $priority = $raw['priority'] ?? 0;
    $done = $raw['done'] ?? false;
    if (!is_string($title) || !is_int($priority) || !is_bool($done)) {
        return null;
    }
    return ['title' => $title, 'priority' => $priority, 'done' => $done];
}

$routes = [
    ['GET', '#^/health$#', function (): void {
        $store = Store::load();
        writeJson(200, ['status' => 'ok', 'count' => count($store->tasks)]);
    }],

    ['GET', '#^/tasks$#', function (): void {
        $done = $_GET['done'] ?? null;
        if ($done !== null && $done !== 'true' && $done !== 'false') {
            fail(400, 'done must be true or false');
            return;
        }
        $store = Store::load();
        $selected = array_values(array_filter(
            $store->tasks,
            fn (Task $task) => $done === null || $task->done === ($done === 'true')
        ));
        usort($selected, fn (Task $a, Task $b) => $b->score <=> $a->score ?: $a->id <=> $b->id);
        writeJson(200, ['tasks' => $selected, 'total' => count($selected)]);
    }],

    ['GET', '#^/tasks/([^/]+)$#', function (string $id): void {
        $taskId = parseId($id);
        if ($taskId === null) {
            fail(400, 'invalid id');
            return;
        }
        $store = Store::load();
        if (!isset($store->tasks[$taskId])) {
            fail(404, 'task not found');
            return;
        }
        writeJson(200, $store->tasks[$taskId]);
    }],

    ['POST', '#^/tasks$#', function (): void {
        $input = readInput();
        if ($input === null) {
            fail(400, 'invalid json');
            return;
        }
        $error = validate($input['title'], $input['priority']);
        if ($error !== null) {
            fail(400, $error);
            return;
        }
        $store = Store::load();
        $task = new Task($store->nextId, $input['title'], $input['priority'], false,
            computeScore($input['priority'], false));
        $store->tasks[$task->id] = $task;
        $store->nextId += 1;
        $store->save();
        writeJson(201, $task);
    }],

    ['PUT', '#^/tasks/([^/]+)$#', function (string $id): void {
        $taskId = parseId($id);
        if ($taskId === null) {
            fail(400, 'invalid id');
            return;
        }
        $store = Store::load();
        if (!isset($store->tasks[$taskId])) {
            fail(404, 'task not found');
            return;
        }
        $input = readInput();
        if ($input === null) {
            fail(400, 'invalid json');
            return;
        }
        $error = validate($input['title'], $input['priority']);
        if ($error !== null) {
            fail(400, $error);
            return;
        }
        $task = $store->tasks[$taskId];
        $task->title = $input['title'];
        $task->priority = $input['priority'];
        $task->done = $input['done'];
        $task->score = computeScore($input['priority'], $input['done']);
        $store->save();
        writeJson(200, $task);
    }],

    ['DELETE', '#^/tasks/([^/]+)$#', function (string $id): void {
        $taskId = parseId($id);
        if ($taskId === null) {
            fail(400, 'invalid id');
            return;
        }
        $store = Store::load();
        if (!isset($store->tasks[$taskId])) {
            fail(404, 'task not found');
            return;
        }
        unset($store->tasks[$taskId]);
        $store->save();
        http_response_code(204);
    }],

    ['GET', '#^/stats$#', function (): void {
        $store = Store::load();
        $total = count($store->tasks);
        $doneCount = 0;
        $sumScore = 0;
        $best = null;
        foreach ($store->tasks as $task) {
            if ($task->done) {
                $doneCount += 1;
            }
            $sumScore += $task->score;
            if (!$task->done && ($best === null || $task->priority > $best->priority)) {
                $best = $task;
            }
        }
        $avgScore = $total === 0 ? 0.0 : round($sumScore / $total, 2);
        writeJson(200, [
            'total' => $total,
            'doneCount' => $doneCount,
            'openCount' => $total - $doneCount,
            'avgScore' => $avgScore,
            'topOpenTitle' => $best === null ? null : $best->title,
        ]);
    }],
];

$method = $_SERVER['REQUEST_METHOD'];
$path = explode('?', $_SERVER['REQUEST_URI'], 2)[0];

foreach ($routes as [$verb, $pattern, $handler]) {
    if ($verb === $method && preg_match($pattern, $path, $match) === 1) {
        $handler(...array_slice($match, 1));
        return;
    }
}
fail(404, 'not found');
