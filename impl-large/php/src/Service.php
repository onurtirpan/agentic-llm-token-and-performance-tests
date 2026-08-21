<?php
// Task Service, large tier — business rules, authorization and audit emission.

declare(strict_types=1);

// ------------------------------------------------------------------ serializers

function serializeUser(User $user): array
{
    return ['id' => $user->id, 'username' => $user->username, 'role' => $user->role,
        'quota' => $user->quota, 'version' => $user->version, 'deleted' => $user->deleted];
}

function serializeProject(Store $store, Project $project): array
{
    return ['id' => $project->id, 'name' => $project->name, 'ownerId' => $project->ownerId,
        'taskCount' => $store->taskCount($project->id), 'version' => $project->version,
        'deleted' => $project->deleted];
}

function serializeTask(Task $task, string $role): array
{
    $body = ['id' => $task->id, 'projectId' => $task->projectId, 'title' => $task->title,
        'priority' => $task->priority, 'status' => $task->status,
        'assigneeId' => $task->assigneeId];
    if ($role === 'admin') {
        $body['internalNote'] = $task->internalNote;
    }
    $body['version'] = $task->version;
    $body['deleted'] = $task->deleted;
    $body['score'] = computeScore($task->priority, $task->status);
    return $body;
}

function serializeComment(Comment $comment): array
{
    return ['id' => $comment->id, 'taskId' => $comment->taskId,
        'authorId' => $comment->authorId, 'body' => $comment->body];
}

function serializeAudit(AuditEntry $entry): array
{
    return ['seq' => $entry->seq, 'actorId' => $entry->actorId, 'action' => $entry->action,
        'resource' => $entry->resource, 'resourceId' => $entry->resourceId];
}

function serializeOutbox(OutboxEvent $event): array
{
    return ['seq' => $event->seq, 'name' => $event->name, 'resourceId' => $event->resourceId,
        'delivered' => $event->delivered];
}

// ----------------------------------------------------------------- access rules

/** @return array{User, Session} */
function authenticate(Store $store, string $header): array
{
    $token = str_starts_with($header, 'Bearer ') ? substr($header, 7) : '';
    $session = $store->sessions[$token] ?? null;
    if ($session === null) {
        throw unauthorized();
    }
    $user = $store->findUser($session->userId);
    if ($user === null) {
        throw unauthorized();
    }
    return [$user, $session];
}

function chargeQuota(User $user, Session $session): int
{
    if ($session->used >= $user->quota) {
        throw quotaExceeded();
    }
    $session->used += 1;
    return max($user->quota - $session->used, 0);
}

function requireAdmin(User $user): void
{
    if ($user->role !== 'admin') {
        throw forbidden();
    }
}

function reachableProject(Store $store, ?int $projectId, User $user,
    bool $includeDeleted = false): Project
{
    $project = $store->findProject($projectId, $includeDeleted);
    if ($project === null) {
        throw notFound();
    }
    if ($user->role !== 'admin' && $project->ownerId !== $user->id) {
        throw forbidden();
    }
    return $project;
}

function reachableTask(Store $store, ?int $taskId, User $user,
    bool $includeDeleted = false): Task
{
    $task = $store->findTask($taskId, $includeDeleted);
    if ($task === null) {
        throw notFound();
    }
    reachableProject($store, $task->projectId, $user, true);
    return $task;
}

function checkIfMatch(string $header, int $version): void
{
    if ($header === '') {
        throw preconditionRequired();
    }
    if ($header !== (string) $version) {
        throw preconditionFailed();
    }
}

function checkIncludeDeleted(mixed $raw, User $user): bool
{
    if ($raw === null) {
        return false;
    }
    if ($user->role !== 'admin') {
        throw forbidden();
    }
    return $raw === 'true';
}

// ------------------------------------------------------------------- pagination

/** Sort by the tiebreak first, then stably by the requested field. */
function paginate(array $rows, int $limit, int $offset, string $sort, string $order): array
{
    $rows = array_values($rows);
    $tiebreak = $rows !== [] && array_key_exists('seq', $rows[0]) ? 'seq' : 'id';
    usort($rows, fn (array $a, array $b) => $a[$tiebreak] <=> $b[$tiebreak]);
    usort($rows, fn (array $a, array $b) => $order === 'desc'
        ? $b[$sort] <=> $a[$sort]
        : $a[$sort] <=> $b[$sort]);
    return ['items' => array_values(array_slice($rows, $offset, $limit)),
        'total' => count($rows), 'limit' => $limit, 'offset' => $offset];
}

// ------------------------------------------------------------------------- auth

function login(Store $store, string $username, string $password, string $token): User
{
    $user = $store->findByUsername($username);
    if ($user === null || $user->password !== $password) {
        throw invalidCredentials();
    }
    $store->sessions[$token] = new Session($token, $user->id);
    return $user;
}

// --------------------------------------------------------------------- projects

function createProject(Store $store, User $actor, string $name, ?int $ownerId): Project
{
    $errors = [];
    checkString($name, 'name', MAX_NAME_LENGTH, $errors);
    if ($store->findUser($ownerId) === null) {
        $errors[] = fail('ownerId', 'ownerId is not a known user');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    foreach ($store->projects as $other) {
        if ($other->ownerId === $ownerId && $other->name === $name && !$other->deleted) {
            throw conflict();
        }
    }
    $project = $store->insertProject($name, (int) $ownerId);
    $store->record($actor->id, 'create', 'project', $project->id);
    return $project;
}

function renameProject(Store $store, User $actor, Project $project, string $name): Project
{
    $errors = [];
    checkString($name, 'name', MAX_NAME_LENGTH, $errors);
    if ($errors !== []) {
        throw invalid($errors);
    }
    foreach ($store->projects as $other) {
        if ($other->ownerId === $project->ownerId && $other->name === $name
            && $other->id !== $project->id && !$other->deleted) {
            throw conflict();
        }
    }
    $project->name = $name;
    $project->version += 1;
    $store->record($actor->id, 'update', 'project', $project->id);
    return $project;
}

function deleteProject(Store $store, User $actor, Project $project): Project
{
    $project->deleted = true;
    $project->version += 1;
    $store->record($actor->id, 'delete', 'project', $project->id);
    foreach ($store->liveTasksOf($project->id) as $task) {
        $task->deleted = true;
        $task->version += 1;
        $store->record($actor->id, 'delete', 'task', $task->id);
    }
    return $project;
}

function restoreProject(Store $store, User $actor, Project $project): Project
{
    if (!$project->deleted) {
        throw conflict();
    }
    $project->deleted = false;
    $project->version += 1;
    $store->record($actor->id, 'restore', 'project', $project->id);
    return $project;
}

// ------------------------------------------------------------------------ tasks

function readNote(User $actor, array $body, array &$errors, string $current): string
{
    if (!array_key_exists('internalNote', $body)) {
        return $current;
    }
    if ($actor->role !== 'admin') {
        throw forbidden();
    }
    $note = $body['internalNote'];
    if (!is_string($note)) {
        throw badRequest();
    }
    if (mb_strlen($note) > MAX_TITLE_LENGTH) {
        $errors[] = fail('internalNote', 'internalNote is too long');
    }
    return $note;
}

function createTask(Store $store, User $actor, Project $project, string $title, ?int $priority,
    ?int $assigneeId, string $note, array $errors): Task
{
    checkString($title, 'title', MAX_TITLE_LENGTH, $errors);
    checkPriority($priority, $errors);
    if ($assigneeId !== null && $store->findUser($assigneeId) === null) {
        $errors[] = fail('assigneeId', 'assigneeId is not a known user');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    $task = $store->insertTask($project->id, $title, (int) $priority, $assigneeId, $note);
    $store->record($actor->id, 'create', 'task', $task->id);
    return $task;
}

function replaceTask(Store $store, User $actor, Task $task, string $title, ?int $priority,
    ?int $assigneeId, string $note, array $errors): Task
{
    checkString($title, 'title', MAX_TITLE_LENGTH, $errors);
    checkPriority($priority, $errors);
    if ($assigneeId !== null && $store->findUser($assigneeId) === null) {
        $errors[] = fail('assigneeId', 'assigneeId is not a known user');
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    $task->title = $title;
    $task->priority = (int) $priority;
    $task->assigneeId = $assigneeId;
    $task->internalNote = $note;
    $task->version += 1;
    $store->record($actor->id, 'update', 'task', $task->id);
    return $task;
}

function moveStatus(Store $store, User $actor, Task $task, mixed $status): Task
{
    $errors = [];
    checkStatus($status, $errors);
    if ($errors !== []) {
        throw invalid($errors);
    }
    if (!in_array("{$task->status}->{$status}", TRANSITIONS, true)) {
        throw invalidTransition();
    }
    $task->status = $status;
    $task->version += 1;
    $store->record($actor->id, 'update', 'task', $task->id);
    return $task;
}

function deleteTask(Store $store, User $actor, Task $task): Task
{
    $task->deleted = true;
    $task->version += 1;
    $store->record($actor->id, 'delete', 'task', $task->id);
    return $task;
}

function restoreTask(Store $store, User $actor, Task $task): Task
{
    if (!$task->deleted) {
        throw conflict();
    }
    $task->deleted = false;
    $task->version += 1;
    $store->record($actor->id, 'restore', 'task', $task->id);
    return $task;
}

// --------------------------------------------------------------------- comments

function createComment(Store $store, User $actor, Task $task, string $body): Comment
{
    $errors = [];
    checkString($body, 'body', MAX_COMMENT_LENGTH, $errors);
    if ($errors !== []) {
        throw invalid($errors);
    }
    $comment = $store->insertComment($task->id, $actor->id, $body);
    $store->record($actor->id, 'create', 'comment', $comment->id);
    return $comment;
}

function removeComment(Store $store, User $actor, Comment $comment): void
{
    if ($actor->role !== 'admin' && $comment->authorId !== $actor->id) {
        throw forbidden();
    }
    unset($store->comments[$comment->id]);
    $store->record($actor->id, 'delete', 'comment', $comment->id);
}

// ------------------------------------------------------------------------ users

function createUser(Store $store, User $actor, string $username, string $password, mixed $role,
    mixed $quota): User
{
    $errors = [];
    checkString($username, 'username', MAX_NAME_LENGTH, $errors);
    checkString($password, 'password', MAX_NAME_LENGTH, $errors);
    checkRole($role, $errors);
    checkQuota($quota, $errors);
    if ($errors !== []) {
        throw invalid($errors);
    }
    if ($store->findByUsername($username) !== null) {
        throw conflict();
    }
    $user = $store->insertUser($username, $password, $role, $quota);
    $store->record($actor->id, 'create', 'user', $user->id);
    return $user;
}

function updateUser(Store $store, User $actor, User $user, array $body): User
{
    $errors = [];
    if (array_key_exists('role', $body)) {
        checkRole($body['role'], $errors);
    }
    if (array_key_exists('quota', $body)) {
        checkQuota($body['quota'], $errors);
    }
    if ($errors !== []) {
        throw invalid($errors);
    }
    if (array_key_exists('role', $body)) {
        $user->role = $body['role'];
    }
    if (array_key_exists('quota', $body)) {
        $user->quota = $body['quota'];
    }
    $user->version += 1;
    $store->record($actor->id, 'update', 'user', $user->id);
    return $user;
}

function deleteUser(Store $store, User $actor, User $user): User
{
    if ($user->id === $actor->id) {
        throw conflict();
    }
    $user->deleted = true;
    $user->version += 1;
    $store->record($actor->id, 'delete', 'user', $user->id);
    return $user;
}

// ---------------------------------------------------------- queries and reports

/** @return array<int, Project> */
function visibleProjects(Store $store, User $user, bool $includeDeleted): array
{
    return array_filter($store->projects, fn (Project $project)
        => ($includeDeleted || !$project->deleted)
        && ($user->role === 'admin' || $project->ownerId === $user->id));
}

/** @return array<int, Task> */
function visibleTasks(Store $store, User $user, bool $includeDeleted): array
{
    $allowed = visibleProjects($store, $user, true);
    return array_filter($store->tasks, fn (Task $task) => isset($allowed[$task->projectId])
        && ($includeDeleted || !$task->deleted));
}

function search(Store $store, User $user, string $query): array
{
    $needle = mb_strtolower($query);
    $results = [];
    foreach (visibleProjects($store, $user, false) as $project) {
        if (str_contains(mb_strtolower($project->name), $needle)) {
            $results[] = ['type' => 'project', 'id' => $project->id, 'label' => $project->name];
        }
    }
    foreach (visibleTasks($store, $user, false) as $task) {
        if (str_contains(mb_strtolower($task->title), $needle)) {
            $results[] = ['type' => 'task', 'id' => $task->id, 'label' => $task->title];
        }
    }
    return ['results' => $results, 'total' => count($results)];
}

/** @param array<int, Task> $picked */
function groupOf(string $key, array $picked): array
{
    $totalScore = 0;
    foreach ($picked as $task) {
        $totalScore += computeScore($task->priority, $task->status);
    }
    return ['key' => $key, 'tasks' => count($picked), 'totalScore' => $totalScore];
}

function workload(Store $store, User $user, string $groupBy): array
{
    $rows = visibleTasks($store, $user, false);
    $groups = [];
    if ($groupBy === 'status') {
        foreach (STATUSES as $status) {
            $groups[] = groupOf($status,
                array_filter($rows, fn (Task $task) => $task->status === $status));
        }
    } elseif ($groupBy === 'assignee') {
        $named = [];
        foreach ($rows as $task) {
            if ($task->assigneeId !== null) {
                $named[$task->assigneeId] = true;
            }
        }
        ksort($named);
        foreach (array_keys($named) as $assignee) {
            $groups[] = groupOf((string) $assignee,
                array_filter($rows, fn (Task $task) => $task->assigneeId === $assignee));
        }
        $loose = array_filter($rows, fn (Task $task) => $task->assigneeId === null);
        if ($loose !== []) {
            $groups[] = groupOf('unassigned', $loose);
        }
    } else {
        foreach (visibleProjects($store, $user, false) as $project) {
            $groups[] = groupOf($project->name,
                array_filter($rows, fn (Task $task) => $task->projectId === $project->id));
        }
    }
    return ['groupBy' => $groupBy, 'groups' => $groups];
}

function flushOutbox(Store $store): int
{
    $flushed = 0;
    foreach ($store->outbox as $event) {
        if (!$event->delivered) {
            $event->delivered = true;
            $flushed += 1;
        }
    }
    return $flushed;
}

function metrics(Store $store): array
{
    $byStatus = $store->byStatus;
    ksort($byStatus, SORT_NUMERIC);
    $byRoute = $store->byRoute;
    ksort($byRoute, SORT_STRING);
    $routes = [];
    foreach ($byRoute as $route => $count) {
        $routes[] = ['route' => $route, 'count' => $count];
    }
    return ['requests' => $store->requests, 'byStatus' => (object) $byStatus,
        'byRoute' => $routes, 'auditEntries' => count($store->audit),
        'outboxPending' => $store->outboxPending()];
}

function stats(Store $store): array
{
    $byStatus = ['todo' => 0, 'in_progress' => 0, 'done' => 0, 'archived' => 0];
    $sumScore = 0;
    $total = 0;
    foreach ($store->tasks as $task) {
        if ($task->deleted) {
            continue;
        }
        $byStatus[$task->status] += 1;
        $sumScore += computeScore($task->priority, $task->status);
        $total += 1;
    }
    $best = null;
    foreach ($store->projects as $project) {
        if ($project->deleted) {
            continue;
        }
        if ($best === null || $store->taskCount($project->id) > $store->taskCount($best->id)) {
            $best = $project;
        }
    }
    return [
        'projects' => count(array_filter($store->projects,
            fn (Project $project) => !$project->deleted)),
        'tasks' => $total,
        'users' => count(array_filter($store->users, fn (User $user) => !$user->deleted)),
        'sessions' => count($store->sessions),
        'comments' => count($store->comments),
        'byStatus' => $byStatus,
        'avgScore' => $total === 0 ? 0.0 : round($sumScore / $total, 2),
        'topProjectName' => $best?->name,
        'auditEntries' => count($store->audit),
        'outboxPending' => $store->outboxPending(),
    ];
}

function checkBulkSize(mixed $operations): void
{
    if (!is_array($operations) || count($operations) < 1
        || count($operations) > MAX_BULK_ITEMS) {
        throw invalid([fail('operations', 'operations is out of range')]);
    }
}
