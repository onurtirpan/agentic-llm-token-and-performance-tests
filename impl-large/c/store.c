/* Task Service, large tier — the in-memory state and its repositories. */

#include "store.h"

#include <stdio.h>
#include <string.h>

User users[MAX_USERS];
Session sessions[MAX_SESSIONS];
Project projects[MAX_PROJECTS];
Task tasks[MAX_TASKS];
Comment comments[MAX_COMMENTS];
AuditEntry audit[MAX_AUDIT];
OutboxEvent outbox[MAX_OUTBOX];
Slot idempotency[MAX_SLOTS];
RouteCount byRoute[MAX_ROUTES];
StatusCount byStatus[MAX_CODES];

int userTotal = 0;
int sessionTotal = 0;
int projectTotal = 0;
int taskTotal = 0;
int commentTotal = 0;
int auditTotal = 0;
int outboxTotal = 0;
int slotTotal = 0;
int routeTotal = 0;
int codeTotal = 0;

int requests = 0;
int nextProjectId = 1;
int nextTaskId = 1;
int nextCommentId = 1;
int nextUserId = 5;
int nextSeq = 1;

static void addUser(int id, const char *username, const char *password, const char *role,
                    int quota)
{
    User *user = &users[userTotal];
    user->id = id;
    copyText(user->username, sizeof user->username, username);
    copyText(user->password, sizeof user->password, password);
    copyText(user->role, sizeof user->role, role);
    user->quota = quota;
    user->version = 1;
    user->deleted = 0;
    userTotal += 1;
}

void seed(void)
{
    addUser(1, "admin", "admin-secret", "admin", DEFAULT_QUOTA);
    addUser(2, "alice", "alice-secret", "user", DEFAULT_QUOTA);
    addUser(3, "bob", "bob-secret", "user", DEFAULT_QUOTA);
    addUser(4, "probe", "probe-secret", "user", PROBE_QUOTA);
}

int takeSeq(void)
{
    int value = nextSeq;
    nextSeq += 1;
    return value;
}

void record(int actorId, const char *action, const char *resource, int resourceId)
{
    if (auditTotal < MAX_AUDIT) {
        AuditEntry *entry = &audit[auditTotal];
        entry->seq = takeSeq();
        entry->actorId = actorId;
        copyText(entry->action, sizeof entry->action, action);
        copyText(entry->resource, sizeof entry->resource, resource);
        entry->resourceId = resourceId;
        auditTotal += 1;
    }
    if (outboxTotal < MAX_OUTBOX) {
        OutboxEvent *event = &outbox[outboxTotal];
        event->seq = takeSeq();
        sprintf(event->name, "%s.%s", resource, action);
        event->resourceId = resourceId;
        event->delivered = 0;
        outboxTotal += 1;
    }
}

void countRequest(const char *route, int status)
{
    requests += 1;
    for (int index = 0; index < routeTotal; index += 1) {
        if (strcmp(byRoute[index].route, route) == 0) {
            byRoute[index].count += 1;
            route = NULL;
            break;
        }
    }
    if (route != NULL && routeTotal < MAX_ROUTES) {
        copyText(byRoute[routeTotal].route, sizeof byRoute[routeTotal].route, route);
        byRoute[routeTotal].count = 1;
        routeTotal += 1;
    }
    for (int index = 0; index < codeTotal; index += 1) {
        if (byStatus[index].code == status) {
            byStatus[index].count += 1;
            return;
        }
    }
    if (codeTotal < MAX_CODES) {
        byStatus[codeTotal].code = status;
        byStatus[codeTotal].count = 1;
        codeTotal += 1;
    }
}

/* ------------------------------------------------------------------ users */

User *findUser(int userId, int includeDeleted)
{
    for (int index = 0; index < userTotal; index += 1) {
        if (users[index].id != userId) continue;
        if (users[index].deleted && !includeDeleted) return NULL;
        return &users[index];
    }
    return NULL;
}

User *findByUsername(const char *username)
{
    for (int index = 0; index < userTotal; index += 1) {
        if (!users[index].deleted && strcmp(users[index].username, username) == 0) {
            return &users[index];
        }
    }
    return NULL;
}

User *insertUser(const char *username, const char *password, const char *role, int quota)
{
    User *user = NULL;
    if (userTotal >= MAX_USERS) return NULL;
    addUser(nextUserId, username, password, role, quota);
    nextUserId += 1;
    user = &users[userTotal - 1];
    return user;
}

int liveUsers(void)
{
    int total = 0;
    for (int index = 0; index < userTotal; index += 1) {
        if (!users[index].deleted) total += 1;
    }
    return total;
}

/* --------------------------------------------------------------- sessions */

int findSession(const char *token)
{
    for (int index = 0; index < sessionTotal; index += 1) {
        if (strcmp(sessions[index].token, token) == 0) return index;
    }
    return -1;
}

Session *insertSession(const char *token, int userId)
{
    Session *session = NULL;
    if (sessionTotal >= MAX_SESSIONS) return NULL;
    session = &sessions[sessionTotal];
    copyText(session->token, sizeof session->token, token);
    session->userId = userId;
    session->used = 0;
    sessionTotal += 1;
    return session;
}

void removeSession(int index)
{
    memmove(&sessions[index], &sessions[index + 1],
            (size_t)(sessionTotal - index - 1) * sizeof(Session));
    sessionTotal -= 1;
}

/* -------------------------------------------------- projects, tasks, comments */

Project *findProject(int projectId, int includeDeleted)
{
    for (int index = 0; index < projectTotal; index += 1) {
        if (projects[index].id != projectId) continue;
        if (projects[index].deleted && !includeDeleted) return NULL;
        return &projects[index];
    }
    return NULL;
}

Project *insertProject(const char *name, int ownerId)
{
    Project *project = NULL;
    if (projectTotal >= MAX_PROJECTS) return NULL;
    project = &projects[projectTotal];
    project->id = nextProjectId;
    copyText(project->name, sizeof project->name, name);
    project->ownerId = ownerId;
    project->version = 1;
    project->deleted = 0;
    projectTotal += 1;
    nextProjectId += 1;
    return project;
}

Task *findTask(int taskId, int includeDeleted)
{
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].id != taskId) continue;
        if (tasks[index].deleted && !includeDeleted) return NULL;
        return &tasks[index];
    }
    return NULL;
}

Task *insertTask(int projectId, const char *title, int priority, int assigneeId,
                 const char *internalNote)
{
    Task *task = NULL;
    if (taskTotal >= MAX_TASKS) return NULL;
    task = &tasks[taskTotal];
    task->id = nextTaskId;
    task->projectId = projectId;
    copyText(task->title, sizeof task->title, title);
    task->priority = priority;
    copyText(task->status, sizeof task->status, "todo");
    task->assigneeId = assigneeId;
    copyText(task->internalNote, sizeof task->internalNote, internalNote);
    task->version = 1;
    task->deleted = 0;
    taskTotal += 1;
    nextTaskId += 1;
    return task;
}

Comment *findComment(int commentId)
{
    for (int index = 0; index < commentTotal; index += 1) {
        if (comments[index].id == commentId) return &comments[index];
    }
    return NULL;
}

Comment *insertComment(int taskId, int authorId, const char *body)
{
    Comment *comment = NULL;
    if (commentTotal >= MAX_COMMENTS) return NULL;
    comment = &comments[commentTotal];
    comment->id = nextCommentId;
    comment->taskId = taskId;
    comment->authorId = authorId;
    copyText(comment->body, sizeof comment->body, body);
    commentTotal += 1;
    nextCommentId += 1;
    return comment;
}

void dropComment(Comment *comment)
{
    int slot = (int)(comment - comments);
    memmove(&comments[slot], &comments[slot + 1],
            (size_t)(commentTotal - slot - 1) * sizeof(Comment));
    commentTotal -= 1;
}

/* ----------------------------------------------------------------- counters */

int taskCount(int projectId)
{
    int total = 0;
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].projectId == projectId && !tasks[index].deleted) total += 1;
    }
    return total;
}

int liveProjects(void)
{
    int total = 0;
    for (int index = 0; index < projectTotal; index += 1) {
        if (!projects[index].deleted) total += 1;
    }
    return total;
}

int liveTasks(void)
{
    int total = 0;
    for (int index = 0; index < taskTotal; index += 1) {
        if (!tasks[index].deleted) total += 1;
    }
    return total;
}

int outboxPending(void)
{
    int total = 0;
    for (int index = 0; index < outboxTotal; index += 1) {
        if (!outbox[index].delivered) total += 1;
    }
    return total;
}

/* -------------------------------------------------------------- idempotency */

const Slot *findSlot(const char *token, const char *key)
{
    for (int index = 0; index < slotTotal; index += 1) {
        if (strcmp(idempotency[index].token, token) == 0
            && strcmp(idempotency[index].key, key) == 0) {
            return &idempotency[index];
        }
    }
    return NULL;
}

void putSlot(const char *token, const char *key, int status, int etag,
             const char *body, int length)
{
    Slot *slot = NULL;
    if (findSlot(token, key) != NULL || slotTotal >= MAX_SLOTS) return;
    if (length >= RECORD_SIZE) return;
    slot = &idempotency[slotTotal];
    copyText(slot->token, sizeof slot->token, token);
    copyText(slot->key, sizeof slot->key, key);
    slot->status = status;
    slot->etag = etag;
    slot->length = length;
    memcpy(slot->body, body, (size_t)length);
    slot->body[length] = '\0';
    slotTotal += 1;
}
