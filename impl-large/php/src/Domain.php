<?php
// Task Service, large tier — domain types, constants and pure rules.

declare(strict_types=1);

const MAX_TITLE_LENGTH = 80;
const MAX_NAME_LENGTH = 60;
const MAX_COMMENT_LENGTH = 200;
const MAX_BULK_ITEMS = 20;
const MIN_PRIORITY = 1;
const MAX_PRIORITY = 5;
const DEFAULT_LIMIT = 20;
const MAX_LIMIT = 100;
const DEFAULT_QUOTA = 10000;
const PROBE_QUOTA = 5;

const ROLES = ['admin', 'user'];
const STATUSES = ['todo', 'in_progress', 'done', 'archived'];
const STATUS_BONUS = ['todo' => 0, 'in_progress' => 3, 'done' => 5, 'archived' => 0];
const TRANSITIONS = ['todo->in_progress', 'todo->archived', 'in_progress->todo',
    'in_progress->done', 'done->archived'];
const PROJECT_SORTS = ['id', 'name', 'taskCount'];
const TASK_SORTS = ['id', 'title', 'priority', 'score', 'status'];
const USER_SORTS = ['id', 'username', 'role'];
const COMMENT_SORTS = ['id', 'authorId'];
const SEQ_SORTS = ['seq'];
const GROUP_BYS = ['assignee', 'status', 'project'];

final class User
{
    public function __construct(
        public int $id,
        public string $username,
        public string $password,
        public string $role,
        public int $quota,
        public int $version = 1,
        public bool $deleted = false,
    ) {
    }
}

final class Session
{
    public function __construct(
        public string $token,
        public int $userId,
        public int $used = 0,
    ) {
    }
}

final class Project
{
    public function __construct(
        public int $id,
        public string $name,
        public int $ownerId,
        public int $version = 1,
        public bool $deleted = false,
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
        public string $internalNote = '',
        public int $version = 1,
        public bool $deleted = false,
    ) {
    }
}

final class Comment
{
    public function __construct(
        public int $id,
        public int $taskId,
        public int $authorId,
        public string $body,
    ) {
    }
}

final class AuditEntry
{
    public function __construct(
        public int $seq,
        public int $actorId,
        public string $action,
        public string $resource,
        public int $resourceId,
    ) {
    }
}

final class OutboxEvent
{
    public function __construct(
        public int $seq,
        public string $name,
        public int $resourceId,
        public bool $delivered = false,
    ) {
    }
}

/** Every failure path throws this. The api layer turns it into the envelope. */
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

function badRequest(): AppError
{
    return new AppError(400, 'bad_request', 'the request is malformed');
}

function unauthorized(): AppError
{
    return new AppError(401, 'unauthorized', 'authentication is required');
}

function invalidCredentials(): AppError
{
    return new AppError(401, 'invalid_credentials', 'the username or password is wrong');
}

function forbidden(): AppError
{
    return new AppError(403, 'forbidden', 'you may not access this resource');
}

function notFound(): AppError
{
    return new AppError(404, 'not_found', 'the resource does not exist');
}

function conflict(): AppError
{
    return new AppError(409, 'conflict', 'the resource already exists');
}

function invalidTransition(): AppError
{
    return new AppError(409, 'invalid_transition', 'the status change is not allowed');
}

function preconditionFailed(): AppError
{
    return new AppError(412, 'precondition_failed', 'the resource has changed');
}

function preconditionRequired(): AppError
{
    return new AppError(428, 'precondition_required', 'the If-Match header is required');
}

function quotaExceeded(): AppError
{
    return new AppError(429, 'quota_exceeded', 'the request quota is exhausted');
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

function computeScore(int $priority, string $status): int
{
    $baseScore = $priority * 10;
    return $baseScore + STATUS_BONUS[$status];
}

function checkString(string $value, string $fieldName, int $maxLength, array &$errors): void
{
    if ($value === '') {
        $errors[] = fail($fieldName, "$fieldName is required");
    } elseif (mb_strlen($value) > $maxLength) {
        $errors[] = fail($fieldName, "$fieldName is too long");
    }
}

function checkPriority(?int $value, array &$errors): void
{
    if ($value === null || $value < MIN_PRIORITY || $value > MAX_PRIORITY) {
        $errors[] = fail('priority', 'priority is out of range');
    }
}

function checkStatus(mixed $value, array &$errors): void
{
    if (!is_string($value) || !in_array($value, STATUSES, true)) {
        $errors[] = fail('status', 'status is not valid');
    }
}

function checkRole(mixed $value, array &$errors): void
{
    if (!is_string($value) || !in_array($value, ROLES, true)) {
        $errors[] = fail('role', 'role is not valid');
    }
}

function checkQuota(mixed $value, array &$errors): void
{
    if (!is_int($value) || $value < 0) {
        $errors[] = fail('quota', 'quota is out of range');
    }
}
