<?php
// Task Service — Slim implementation.

declare(strict_types=1);

require __DIR__ . '/../vendor/autoload.php';

use Psr\Http\Message\ResponseInterface as Response;
use Psr\Http\Message\ServerRequestInterface as Request;
use Slim\Factory\AppFactory;

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

function writeJson(Response $response, int $status, mixed $body): Response
{
    $response->getBody()->write((string) json_encode($body));
    return $response->withHeader('Content-Type', 'application/json')->withStatus($status);
}

function fail(Response $response, int $status, string $message): Response
{
    return writeJson($response, $status, ['error' => $message]);
}

function parseId(string $raw): ?int
{
    return preg_match('/^-?\d+$/', $raw) === 1 ? (int) $raw : null;
}

/** @return array{title: string, priority: int, done: bool}|null */
function readInput(Request $request): ?array
{
    $raw = json_decode((string) $request->getBody(), true);
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

$app = AppFactory::create();

$app->get('/health', function (Request $request, Response $response): Response {
    $store = Store::load();
    return writeJson($response, 200, ['status' => 'ok', 'count' => count($store->tasks)]);
});

$app->get('/tasks', function (Request $request, Response $response): Response {
    $done = $request->getQueryParams()['done'] ?? null;
    if ($done !== null && $done !== 'true' && $done !== 'false') {
        return fail($response, 400, 'done must be true or false');
    }
    $store = Store::load();
    $selected = array_values(array_filter(
        $store->tasks,
        fn (Task $task) => $done === null || $task->done === ($done === 'true')
    ));
    usort($selected, fn (Task $a, Task $b) => $b->score <=> $a->score ?: $a->id <=> $b->id);
    return writeJson($response, 200, ['tasks' => $selected, 'total' => count($selected)]);
});

$app->get('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    $taskId = parseId($args['id']);
    if ($taskId === null) {
        return fail($response, 400, 'invalid id');
    }
    $store = Store::load();
    if (!isset($store->tasks[$taskId])) {
        return fail($response, 404, 'task not found');
    }
    return writeJson($response, 200, $store->tasks[$taskId]);
});

$app->post('/tasks', function (Request $request, Response $response): Response {
    $input = readInput($request);
    if ($input === null) {
        return fail($response, 400, 'invalid json');
    }
    $error = validate($input['title'], $input['priority']);
    if ($error !== null) {
        return fail($response, 400, $error);
    }
    $store = Store::load();
    $task = new Task($store->nextId, $input['title'], $input['priority'], false,
        computeScore($input['priority'], false));
    $store->tasks[$task->id] = $task;
    $store->nextId += 1;
    $store->save();
    return writeJson($response, 201, $task);
});

$app->put('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    $taskId = parseId($args['id']);
    if ($taskId === null) {
        return fail($response, 400, 'invalid id');
    }
    $store = Store::load();
    if (!isset($store->tasks[$taskId])) {
        return fail($response, 404, 'task not found');
    }
    $input = readInput($request);
    if ($input === null) {
        return fail($response, 400, 'invalid json');
    }
    $error = validate($input['title'], $input['priority']);
    if ($error !== null) {
        return fail($response, 400, $error);
    }
    $task = $store->tasks[$taskId];
    $task->title = $input['title'];
    $task->priority = $input['priority'];
    $task->done = $input['done'];
    $task->score = computeScore($input['priority'], $input['done']);
    $store->save();
    return writeJson($response, 200, $task);
});

$app->delete('/tasks/{id}', function (Request $request, Response $response, array $args): Response {
    $taskId = parseId($args['id']);
    if ($taskId === null) {
        return fail($response, 400, 'invalid id');
    }
    $store = Store::load();
    if (!isset($store->tasks[$taskId])) {
        return fail($response, 404, 'task not found');
    }
    unset($store->tasks[$taskId]);
    $store->save();
    return $response->withStatus(204);
});

$app->get('/stats', function (Request $request, Response $response): Response {
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
    return writeJson($response, 200, [
        'total' => $total,
        'doneCount' => $doneCount,
        'openCount' => $total - $doneCount,
        'avgScore' => $avgScore,
        'topOpenTitle' => $best === null ? null : $best->title,
    ]);
});

$app->any('/{path:.*}', function (Request $request, Response $response): Response {
    return fail($response, 404, 'not found');
});

$app->run();
