<?php
// Task Service, large tier — the persisted state and its repositories.

declare(strict_types=1);

const STORE_PATH = __DIR__ . '/../store.json';
const AUDIT_PATH = __DIR__ . '/../audit.log';
const OUTBOX_PATH = __DIR__ . '/../outbox.log';

/**
 * PHP starts a fresh process for every request, so no state survives in memory.
 * The store reads and writes files instead, and those files must carry everything
 * mutable: the users, whose role, quota, version and deleted flag all change, the
 * sessions with their used counter, the projects, the tasks, the comments, the
 * audit trail, the outbox, the recorded idempotent responses, the metrics counters,
 * and every id and seq counter. The used counter grows on every authenticated
 * request and the metrics counters grow on every request at all, so essentially no
 * request is a pure read. The api layer therefore calls save() once at the end of
 * the middleware instead of leaving it to each handler.
 *
 * The state sits in two kinds of file, because the tables are two shapes. Bounded
 * or edited-in-place rows stay in store.json, which every request decodes whole and
 * re-encodes whole. The audit trail and the outbox are neither bounded nor edited:
 * they gain one row per write and no later request reads them back. Holding them in
 * store.json made json_decode and json_encode the dominant cost of a request, so
 * latency grew with the length of the run — decoding alone reached 8.5 ms once the
 * store passed 144 KB. They live in audit.log and outbox.log instead, one JSON
 * object per line, appended with FILE_APPEND and never rewritten, which is the O(1)
 * append every other implementation in this tier gets from a list in memory. Only
 * GET /audit and GET /outbox read them back; /stats and /metrics want counts alone,
 * and store.json keeps those in auditCount, outboxCount and outboxDelivered.
 *
 * An append-only file cannot revisit a row it has written, so outbox.log omits the
 * delivered flag. POST /outbox/flush moves a watermark instead — it stores the
 * highest seq it covered in outboxFlushedThrough — and a read derives delivered as
 * seq <= outboxFlushedThrough. The watermark is exact because seq only ever rises,
 * so every event written after a flush sorts above it and every event written
 * before sorts below.
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
    /** How many rows audit.log holds, so a count never reads the file. */
    public int $auditCount = 0;
    /** How many rows outbox.log holds. */
    public int $outboxCount = 0;
    /** How many of those rows the last flush delivered. */
    public int $outboxDelivered = 0;
    /** The highest seq the last flush covered. Delivered means seq <= this. */
    public int $outboxFlushedThrough = 0;
    /** @var list<AuditEntry> the rows this request added, appended by save(). */
    private array $newAudit = [];
    /** @var list<OutboxEvent> the rows this request added, appended by save(). */
    private array $newOutbox = [];

    public static function load(): self
    {
        $store = new self();
        if (!file_exists(STORE_PATH)) {
            // A missing store.json means a fresh run. The harness deletes that file
            // alone, so drop whatever a previous run left in the two logs.
            @unlink(AUDIT_PATH);
            @unlink(OUTBOX_PATH);
            $store->seed();
            return $store;
        }
        $raw = json_decode((string) file_get_contents(STORE_PATH), true);
        $store->users = self::hydrate($raw['users'], User::class, 'id');
        $store->sessions = self::hydrate($raw['sessions'], Session::class, 'token');
        $store->projects = self::hydrate($raw['projects'], Project::class, 'id');
        $store->tasks = self::hydrate($raw['tasks'], Task::class, 'id');
        $store->comments = self::hydrate($raw['comments'], Comment::class, 'id');
        $store->idempotency = $raw['idempotency'];
        $store->byStatus = $raw['byStatus'];
        $store->byRoute = $raw['byRoute'];
        $store->requests = $raw['requests'];
        $store->nextProjectId = $raw['nextProjectId'];
        $store->nextTaskId = $raw['nextTaskId'];
        $store->nextCommentId = $raw['nextCommentId'];
        $store->nextUserId = $raw['nextUserId'];
        $store->nextSeq = $raw['nextSeq'];
        $store->auditCount = $raw['auditCount'];
        $store->outboxCount = $raw['outboxCount'];
        $store->outboxDelivered = $raw['outboxDelivered'];
        $store->outboxFlushedThrough = $raw['outboxFlushedThrough'];
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
            'idempotency' => (object) $this->idempotency,
            'byStatus' => (object) $this->byStatus,
            'byRoute' => (object) $this->byRoute,
            'requests' => $this->requests,
            'nextProjectId' => $this->nextProjectId,
            'nextTaskId' => $this->nextTaskId,
            'nextCommentId' => $this->nextCommentId,
            'nextUserId' => $this->nextUserId,
            'nextSeq' => $this->nextSeq,
            'auditCount' => $this->auditCount,
            'outboxCount' => $this->outboxCount,
            'outboxDelivered' => $this->outboxDelivered,
            'outboxFlushedThrough' => $this->outboxFlushedThrough,
        ], JSON_UNESCAPED_SLASHES));
        self::appendLog(AUDIT_PATH, $this->newAudit);
        self::appendLog(OUTBOX_PATH, $this->newOutbox);
        $this->newAudit = [];
        $this->newOutbox = [];
    }

    /**
     * Append the rows one request added, one JSON object per line. A read that
     * wants nothing new writes nothing at all. The outbox leaves the delivered
     * flag out, because outboxEvents() derives it from the flush watermark.
     *
     * @param list<AuditEntry|OutboxEvent> $rows
     */
    private static function appendLog(string $path, array $rows): void
    {
        if ($rows === []) {
            return;
        }
        $lines = '';
        foreach ($rows as $row) {
            $fields = get_object_vars($row);
            unset($fields['delivered']);
            $lines .= json_encode($fields, JSON_UNESCAPED_SLASHES) . "\n";
        }
        file_put_contents($path, $lines, FILE_APPEND);
    }

    /**
     * Decode one log file, oldest line first. Only GET /audit and GET /outbox
     * reach this, so the cost of a long log never lands on a write path.
     *
     * @return list<array<string, mixed>>
     */
    private static function readLog(string $path): array
    {
        if (!file_exists($path)) {
            return [];
        }
        $rows = [];
        foreach (explode("\n", (string) file_get_contents($path)) as $line) {
            if ($line !== '') {
                $rows[] = json_decode($line, true);
            }
        }
        return $rows;
    }

    /** @return list<AuditEntry> every audit row, ascending by seq. */
    public function auditEntries(): array
    {
        $entries = [];
        foreach (self::readLog(AUDIT_PATH) as $row) {
            $entries[] = new AuditEntry(...$row);
        }
        return array_merge($entries, $this->newAudit);
    }

    /**
     * @return list<OutboxEvent> every outbox row, ascending by seq, each carrying
     *     the delivered flag the flush watermark implies.
     */
    public function outboxEvents(): array
    {
        $events = [];
        foreach (self::readLog(OUTBOX_PATH) as $row) {
            $events[] = new OutboxEvent(...$row);
        }
        $events = array_merge($events, $this->newOutbox);
        foreach ($events as $event) {
            $event->delivered = $event->seq <= $this->outboxFlushedThrough;
        }
        return $events;
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

    /** Rebuild one row list out of store.json, keyed by $key. */
    private static function hydrate(array $rows, string $class, string $key): array
    {
        $out = [];
        foreach ($rows as $row) {
            $item = new $class(...$row);
            $out[$item->$key] = $item;
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
        $this->newAudit[] = new AuditEntry($this->takeSeq(), $actorId, $action, $resource,
            $resourceId);
        $this->newOutbox[] = new OutboxEvent($this->takeSeq(), "$resource.$action", $resourceId);
        $this->auditCount += 1;
        $this->outboxCount += 1;
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
        return $this->outboxCount - $this->outboxDelivered;
    }

    /**
     * Deliver every event written so far and return how many the call changed.
     * outbox.log cannot revisit a row, so this moves a watermark rather than
     * rewriting flags: nextSeq - 1 is at or above every seq handed out so far, and
     * below every seq a later write will take.
     */
    public function flushOutbox(): int
    {
        $flushed = $this->outboxPending();
        $this->outboxDelivered = $this->outboxCount;
        $this->outboxFlushedThrough = $this->nextSeq - 1;
        return $flushed;
    }
}
