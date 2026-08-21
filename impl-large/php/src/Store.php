<?php
// Task Service, large tier — the persisted state and its repositories.

declare(strict_types=1);

const STORE_PATH = __DIR__ . '/../store.json';

/**
 * PHP starts a fresh process for every request, so no state survives in memory.
 * The store reads and writes one JSON file instead, and that file must carry
 * everything mutable: the users, whose role, quota, version and deleted flag all
 * change, the sessions with their used counter, the projects, the tasks, the
 * comments, the audit trail, the outbox, the recorded idempotent responses, the
 * metrics counters, and every id and seq counter. The used counter grows on every
 * authenticated request and the metrics counters grow on every request at all, so
 * essentially no request is a pure read. The api layer therefore calls save() once
 * at the end of the middleware instead of leaving it to each handler.
 */
final class Store
{
    /** @var array<int, User> */
    public array $users = [];
    /** @var array<string, Session> */
    public array $sessions = [];
    /** @var array<int, Project> */
    public array $projects = [];
    /** @var array<int, Task> */
    public array $tasks = [];
    /** @var array<int, Comment> */
    public array $comments = [];
    /** @var list<AuditEntry> */
    public array $audit = [];
    /** @var list<OutboxEvent> */
    public array $outbox = [];
    /** @var array<string, array{status: int, body: array}> */
    public array $idempotency = [];
    /** @var array<int, int> */
    public array $byStatus = [];
    /** @var array<string, int> */
    public array $byRoute = [];
    public int $requests = 0;
    public int $nextProjectId = 1;
    public int $nextTaskId = 1;
    public int $nextCommentId = 1;
    public int $nextUserId = 5;
    public int $nextSeq = 1;

    public static function load(): self
    {
        $store = new self();
        if (!file_exists(STORE_PATH)) {
            $store->seed();
            return $store;
        }
        $raw = json_decode((string) file_get_contents(STORE_PATH), true);
        $store->users = self::hydrate($raw['users'], User::class, 'id');
        $store->sessions = self::hydrate($raw['sessions'], Session::class, 'token');
        $store->projects = self::hydrate($raw['projects'], Project::class, 'id');
        $store->tasks = self::hydrate($raw['tasks'], Task::class, 'id');
        $store->comments = self::hydrate($raw['comments'], Comment::class, 'id');
        $store->audit = self::hydrate($raw['audit'], AuditEntry::class, null);
        $store->outbox = self::hydrate($raw['outbox'], OutboxEvent::class, null);
        $store->idempotency = $raw['idempotency'];
        $store->byStatus = $raw['byStatus'];
        $store->byRoute = $raw['byRoute'];
        $store->requests = $raw['requests'];
        $store->nextProjectId = $raw['nextProjectId'];
        $store->nextTaskId = $raw['nextTaskId'];
        $store->nextCommentId = $raw['nextCommentId'];
        $store->nextUserId = $raw['nextUserId'];
        $store->nextSeq = $raw['nextSeq'];
        return $store;
    }

    public function save(): void
    {
        ksort($this->users);
        ksort($this->projects);
        ksort($this->tasks);
        ksort($this->comments);
        file_put_contents(STORE_PATH, (string) json_encode([
            'users' => array_map('get_object_vars', array_values($this->users)),
            'sessions' => array_map('get_object_vars', array_values($this->sessions)),
            'projects' => array_map('get_object_vars', array_values($this->projects)),
            'tasks' => array_map('get_object_vars', array_values($this->tasks)),
            'comments' => array_map('get_object_vars', array_values($this->comments)),
            'audit' => array_map('get_object_vars', $this->audit),
            'outbox' => array_map('get_object_vars', $this->outbox),
            'idempotency' => (object) $this->idempotency,
            'byStatus' => (object) $this->byStatus,
            'byRoute' => (object) $this->byRoute,
            'requests' => $this->requests,
            'nextProjectId' => $this->nextProjectId,
            'nextTaskId' => $this->nextTaskId,
            'nextCommentId' => $this->nextCommentId,
            'nextUserId' => $this->nextUserId,
            'nextSeq' => $this->nextSeq,
        ], JSON_UNESCAPED_SLASHES));
    }

    private function seed(): void
    {
        $this->users = [
            1 => new User(1, 'admin', 'admin-secret', 'admin', DEFAULT_QUOTA),
            2 => new User(2, 'alice', 'alice-secret', 'user', DEFAULT_QUOTA),
            3 => new User(3, 'bob', 'bob-secret', 'user', DEFAULT_QUOTA),
            4 => new User(4, 'probe', 'probe-secret', 'user', PROBE_QUOTA),
        ];
    }

    /** Rebuild one row list, keyed by $key, or left as a list when $key is null. */
    private static function hydrate(array $rows, string $class, ?string $key): array
    {
        $out = [];
        foreach ($rows as $row) {
            $item = new $class(...$row);
            if ($key === null) {
                $out[] = $item;
            } else {
                $out[$item->$key] = $item;
            }
        }
        return $out;
    }

    public function takeSeq(): int
    {
        $value = $this->nextSeq;
        $this->nextSeq += 1;
        return $value;
    }

    /** Append one audit entry and one outbox event for a successful write. */
    public function record(int $actorId, string $action, string $resource, int $resourceId): void
    {
        $this->audit[] = new AuditEntry($this->takeSeq(), $actorId, $action, $resource,
            $resourceId);
        $this->outbox[] = new OutboxEvent($this->takeSeq(), "$resource.$action", $resourceId);
    }

    public function countRequest(string $route, int $status): void
    {
        $this->requests += 1;
        $this->byRoute[$route] = ($this->byRoute[$route] ?? 0) + 1;
        $this->byStatus[$status] = ($this->byStatus[$status] ?? 0) + 1;
    }

    public function findUser(?int $userId, bool $includeDeleted = false): ?User
    {
        $user = $this->users[$userId] ?? null;
        if ($user === null || ($user->deleted && !$includeDeleted)) {
            return null;
        }
        return $user;
    }

    public function findByUsername(string $username): ?User
    {
        foreach ($this->users as $user) {
            if ($user->username === $username && !$user->deleted) {
                return $user;
            }
        }
        return null;
    }

    public function insertUser(string $username, string $password, string $role,
        int $quota): User
    {
        $user = new User($this->nextUserId, $username, $password, $role, $quota);
        $this->users[$user->id] = $user;
        $this->nextUserId += 1;
        return $user;
    }

    public function findProject(?int $projectId, bool $includeDeleted = false): ?Project
    {
        $project = $this->projects[$projectId] ?? null;
        if ($project === null || ($project->deleted && !$includeDeleted)) {
            return null;
        }
        return $project;
    }

    public function insertProject(string $name, int $ownerId): Project
    {
        $project = new Project($this->nextProjectId, $name, $ownerId);
        $this->projects[$project->id] = $project;
        $this->nextProjectId += 1;
        return $project;
    }

    public function findTask(?int $taskId, bool $includeDeleted = false): ?Task
    {
        $task = $this->tasks[$taskId] ?? null;
        if ($task === null || ($task->deleted && !$includeDeleted)) {
            return null;
        }
        return $task;
    }

    public function insertTask(int $projectId, string $title, int $priority, ?int $assigneeId,
        string $internalNote): Task
    {
        $task = new Task($this->nextTaskId, $projectId, $title, $priority, 'todo', $assigneeId,
            $internalNote);
        $this->tasks[$task->id] = $task;
        $this->nextTaskId += 1;
        return $task;
    }

    public function findComment(?int $commentId): ?Comment
    {
        return $this->comments[$commentId] ?? null;
    }

    public function insertComment(int $taskId, int $authorId, string $body): Comment
    {
        $comment = new Comment($this->nextCommentId, $taskId, $authorId, $body);
        $this->comments[$comment->id] = $comment;
        $this->nextCommentId += 1;
        return $comment;
    }

    /** @return array<int, Task> */
    public function liveTasksOf(int $projectId): array
    {
        return array_filter($this->tasks,
            fn (Task $task) => $task->projectId === $projectId && !$task->deleted);
    }

    public function taskCount(int $projectId): int
    {
        return count($this->liveTasksOf($projectId));
    }

    public function outboxPending(): int
    {
        return count(array_filter($this->outbox, fn (OutboxEvent $event) => !$event->delivered));
    }
}
