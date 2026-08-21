/* Task Service, large tier — business rules, authorization and audit emission. */

#include "service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char sortField[SORT_SIZE];
static int sortDescending;

/* ---------------------------------------------------------------- serializers */

int serializeUser(char *out, const void *row, int isAdmin)
{
    const User *user = row;
    int length = sprintf(out, "{\"id\":%d,\"username\":", user->id);
    (void)isAdmin;
    length += writeJsonString(out + length, user->username);
    length += sprintf(out + length, ",\"role\":\"%s\",\"quota\":%d,\"version\":%d,\"deleted\":%s}",
                      user->role, user->quota, user->version, user->deleted ? "true" : "false");
    return length;
}

int serializeProject(char *out, const void *row, int isAdmin)
{
    const Project *project = row;
    int length = sprintf(out, "{\"id\":%d,\"name\":", project->id);
    (void)isAdmin;
    length += writeJsonString(out + length, project->name);
    length += sprintf(out + length,
                      ",\"ownerId\":%d,\"taskCount\":%d,\"version\":%d,\"deleted\":%s}",
                      project->ownerId, taskCount(project->id), project->version,
                      project->deleted ? "true" : "false");
    return length;
}

int serializeTask(char *out, const void *row, int isAdmin)
{
    const Task *task = row;
    int length = sprintf(out, "{\"id\":%d,\"projectId\":%d,\"title\":", task->id, task->projectId);
    length += writeJsonString(out + length, task->title);
    length += sprintf(out + length, ",\"priority\":%d,\"status\":\"%s\",\"assigneeId\":",
                      task->priority, task->status);
    if (task->assigneeId == 0) length += sprintf(out + length, "null");
    else length += sprintf(out + length, "%d", task->assigneeId);
    if (isAdmin) {
        length += sprintf(out + length, ",\"internalNote\":");
        length += writeJsonString(out + length, task->internalNote);
    }
    length += sprintf(out + length, ",\"version\":%d,\"deleted\":%s,\"score\":%d}",
                      task->version, task->deleted ? "true" : "false",
                      computeScore(task->priority, task->status));
    return length;
}

int serializeComment(char *out, const void *row, int isAdmin)
{
    const Comment *comment = row;
    int length = sprintf(out, "{\"id\":%d,\"taskId\":%d,\"authorId\":%d,\"body\":",
                         comment->id, comment->taskId, comment->authorId);
    (void)isAdmin;
    length += writeJsonString(out + length, comment->body);
    length += sprintf(out + length, "}");
    return length;
}

int serializeAudit(char *out, const void *row, int isAdmin)
{
    const AuditEntry *entry = row;
    (void)isAdmin;
    return sprintf(out, "{\"seq\":%d,\"actorId\":%d,\"action\":\"%s\",\"resource\":\"%s\","
                        "\"resourceId\":%d}",
                   entry->seq, entry->actorId, entry->action, entry->resource, entry->resourceId);
}

int serializeOutbox(char *out, const void *row, int isAdmin)
{
    const OutboxEvent *event = row;
    (void)isAdmin;
    return sprintf(out, "{\"seq\":%d,\"name\":\"%s\",\"resourceId\":%d,\"delivered\":%s}",
                   event->seq, event->name, event->resourceId,
                   event->delivered ? "true" : "false");
}

/* ---------------------------------------------------------------- comparators */

static int ordered(int result, int leftId, int rightId)
{
    if (sortDescending) result = -result;
    return result != 0 ? result : leftId - rightId;
}

int compareUsers(const void *left, const void *right)
{
    const User *a = *(const User *const *)left;
    const User *b = *(const User *const *)right;
    int result = 0;
    if (strcmp(sortField, "username") == 0) result = strcmp(a->username, b->username);
    else if (strcmp(sortField, "role") == 0) result = strcmp(a->role, b->role);
    else result = a->id - b->id;
    return ordered(result, a->id, b->id);
}

int compareProjects(const void *left, const void *right)
{
    const Project *a = *(const Project *const *)left;
    const Project *b = *(const Project *const *)right;
    int result = 0;
    if (strcmp(sortField, "name") == 0) result = strcmp(a->name, b->name);
    else if (strcmp(sortField, "taskCount") == 0) result = taskCount(a->id) - taskCount(b->id);
    else result = a->id - b->id;
    return ordered(result, a->id, b->id);
}

int compareTasks(const void *left, const void *right)
{
    const Task *a = *(const Task *const *)left;
    const Task *b = *(const Task *const *)right;
    int result = 0;
    if (strcmp(sortField, "title") == 0) result = strcmp(a->title, b->title);
    else if (strcmp(sortField, "priority") == 0) result = a->priority - b->priority;
    else if (strcmp(sortField, "status") == 0) result = strcmp(a->status, b->status);
    else if (strcmp(sortField, "score") == 0) {
        result = computeScore(a->priority, a->status) - computeScore(b->priority, b->status);
    } else {
        result = a->id - b->id;
    }
    return ordered(result, a->id, b->id);
}

int compareComments(const void *left, const void *right)
{
    const Comment *a = *(const Comment *const *)left;
    const Comment *b = *(const Comment *const *)right;
    int result = 0;
    if (strcmp(sortField, "authorId") == 0) result = a->authorId - b->authorId;
    else result = a->id - b->id;
    return ordered(result, a->id, b->id);
}

int compareSeq(const void *left, const void *right)
{
    const int *a = *(const int *const *)left;
    const int *b = *(const int *const *)right;
    return ordered(*a - *b, *a, *b);
}

/* --------------------------------------------------------------- access rules */

User *authenticate(const char *authorization, int *sessionIndex, AppError *err)
{
    User *user = NULL;
    int index = -1;
    if (strncmp(authorization, "Bearer ", 7) == 0) index = findSession(authorization + 7);
    if (index < 0) {
        errUnauthorized(err);
        return NULL;
    }
    user = findUser(sessions[index].userId, 0);
    if (user == NULL) {
        errUnauthorized(err);
        return NULL;
    }
    *sessionIndex = index;
    return user;
}

int chargeQuota(User *user, Session *session, AppError *err)
{
    int remaining = 0;
    if (session->used >= user->quota) {
        errQuotaExceeded(err);
        return -1;
    }
    session->used += 1;
    remaining = user->quota - session->used;
    return remaining < 0 ? 0 : remaining;
}

int requireAdmin(const User *user, AppError *err)
{
    if (strcmp(user->role, "admin") != 0) return errForbidden(err);
    return 1;
}

Project *reachableProject(int projectId, const User *user, int includeDeleted, AppError *err)
{
    Project *project = findProject(projectId, includeDeleted);
    if (project == NULL) {
        errNotFound(err);
        return NULL;
    }
    if (strcmp(user->role, "admin") != 0 && project->ownerId != user->id) {
        errForbidden(err);
        return NULL;
    }
    return project;
}

Task *reachableTask(int taskId, const User *user, int includeDeleted, AppError *err)
{
    Task *task = findTask(taskId, includeDeleted);
    if (task == NULL) {
        errNotFound(err);
        return NULL;
    }
    if (reachableProject(task->projectId, user, 1, err) == NULL) return NULL;
    return task;
}

int checkIfMatch(const char *header, int hasHeader, int version, AppError *err)
{
    char expected[16];
    if (!hasHeader || header[0] == '\0') return errPreconditionRequired(err);
    sprintf(expected, "%d", version);
    if (strcmp(header, expected) != 0) return errPreconditionFailed(err);
    return 1;
}

int checkIncludeDeleted(const char *raw, int hasRaw, const User *user, int *out, AppError *err)
{
    *out = 0;
    if (!hasRaw) return 1;
    if (strcmp(user->role, "admin") != 0) return errForbidden(err);
    *out = strcmp(raw, "true") == 0;
    return 1;
}

/* ------------------------------------------------------------------ pagination */

int paginate(char *out, void **rows, int total, const Page *page,
             int (*compare)(const void *, const void *), Serializer serialize, int isAdmin)
{
    int length = 0;
    int shown = 0;
    copyText(sortField, sizeof sortField, page->sort);
    sortDescending = strcmp(page->order, "desc") == 0;
    qsort(rows, (size_t)total, sizeof rows[0], compare);
    length = sprintf(out, "{\"items\":[");
    for (int index = page->offset; index < total && shown < page->limit; index += 1) {
        if (shown > 0) length += sprintf(out + length, ",");
        length += serialize(out + length, rows[index], isAdmin);
        shown += 1;
    }
    length += sprintf(out + length, "],\"total\":%d,\"limit\":%d,\"offset\":%d}",
                      total, page->limit, page->offset);
    return length;
}

/* ------------------------------------------------------------------------ auth */

User *login(const char *username, const char *password, const char *token, AppError *err)
{
    User *user = findByUsername(username);
    if (user == NULL || strcmp(user->password, password) != 0) {
        errInvalidCredentials(err);
        return NULL;
    }
    if (insertSession(token, user->id) == NULL) {
        errConflict(err);
        return NULL;
    }
    return user;
}

/* -------------------------------------------------------------------- projects */

static int nameTaken(const char *name, int ownerId, int exceptId)
{
    for (int index = 0; index < projectTotal; index += 1) {
        const Project *project = &projects[index];
        if (project->deleted || project->id == exceptId) continue;
        if (project->ownerId == ownerId && strcmp(project->name, name) == 0) return 1;
    }
    return 0;
}

Project *createProject(const User *actor, const char *name, int ownerId, AppError *err)
{
    Errors errors;
    Project *project = NULL;
    errors.count = 0;
    checkString(name, "name", MAX_NAME_LENGTH, &errors);
    if (findUser(ownerId, 0) == NULL) fail(&errors, "ownerId", "ownerId is not a known user");
    if (errors.count > 0) {
        errInvalid(err, &errors);
        return NULL;
    }
    if (nameTaken(name, ownerId, 0)) {
        errConflict(err);
        return NULL;
    }
    project = insertProject(name, ownerId);
    if (project == NULL) {
        errConflict(err);
        return NULL;
    }
    record(actor->id, "create", "project", project->id);
    return project;
}

int renameProject(const User *actor, Project *project, const char *name, AppError *err)
{
    Errors errors;
    errors.count = 0;
    checkString(name, "name", MAX_NAME_LENGTH, &errors);
    if (errors.count > 0) return errInvalid(err, &errors);
    if (nameTaken(name, project->ownerId, project->id)) return errConflict(err);
    copyText(project->name, sizeof project->name, name);
    project->version += 1;
    record(actor->id, "update", "project", project->id);
    return 1;
}

void deleteProject(const User *actor, Project *project)
{
    project->deleted = 1;
    project->version += 1;
    record(actor->id, "delete", "project", project->id);
    for (int index = 0; index < taskTotal; index += 1) {
        Task *task = &tasks[index];
        if (task->projectId != project->id || task->deleted) continue;
        task->deleted = 1;
        task->version += 1;
        record(actor->id, "delete", "task", task->id);
    }
}

int restoreProject(const User *actor, Project *project, AppError *err)
{
    if (!project->deleted) return errConflict(err);
    project->deleted = 0;
    project->version += 1;
    record(actor->id, "restore", "project", project->id);
    return 1;
}

/* ----------------------------------------------------------------------- tasks */

static void checkTaskInput(const TaskInput *input, Errors *errors)
{
    checkString(input->title, "title", MAX_TITLE_LENGTH, errors);
    checkPriority(input->priority, input->priorityNull, errors);
    if (input->assigneeId != 0 && findUser(input->assigneeId, 0) == NULL) {
        fail(errors, "assigneeId", "assigneeId is not a known user");
    }
}

Task *createTask(const User *actor, const Project *project, const TaskInput *input,
                 Errors *errors, AppError *err)
{
    Task *task = NULL;
    checkTaskInput(input, errors);
    if (errors->count > 0) {
        errInvalid(err, errors);
        return NULL;
    }
    task = insertTask(project->id, input->title, input->priority, input->assigneeId,
                      input->internalNote);
    if (task == NULL) {
        errConflict(err);
        return NULL;
    }
    record(actor->id, "create", "task", task->id);
    return task;
}

int replaceTask(const User *actor, Task *task, const TaskInput *input, Errors *errors,
                AppError *err)
{
    checkTaskInput(input, errors);
    if (errors->count > 0) return errInvalid(err, errors);
    copyText(task->title, sizeof task->title, input->title);
    task->priority = input->priority;
    task->assigneeId = input->assigneeId;
    copyText(task->internalNote, sizeof task->internalNote, input->internalNote);
    task->version += 1;
    record(actor->id, "update", "task", task->id);
    return 1;
}

int moveStatus(const User *actor, Task *task, const char *status, int isString, AppError *err)
{
    Errors errors;
    errors.count = 0;
    checkStatus(status, isString, &errors);
    if (errors.count > 0) return errInvalid(err, &errors);
    if (!allowedTransition(task->status, status)) return errInvalidTransition(err);
    copyText(task->status, sizeof task->status, status);
    task->version += 1;
    record(actor->id, "update", "task", task->id);
    return 1;
}

void deleteTask(const User *actor, Task *task)
{
    task->deleted = 1;
    task->version += 1;
    record(actor->id, "delete", "task", task->id);
}

int restoreTask(const User *actor, Task *task, AppError *err)
{
    if (!task->deleted) return errConflict(err);
    task->deleted = 0;
    task->version += 1;
    record(actor->id, "restore", "task", task->id);
    return 1;
}

/* -------------------------------------------------------------------- comments */

Comment *createComment(const User *actor, const Task *task, const char *body, AppError *err)
{
    Errors errors;
    Comment *comment = NULL;
    errors.count = 0;
    checkString(body, "body", MAX_COMMENT_LENGTH, &errors);
    if (errors.count > 0) {
        errInvalid(err, &errors);
        return NULL;
    }
    comment = insertComment(task->id, actor->id, body);
    if (comment == NULL) {
        errConflict(err);
        return NULL;
    }
    record(actor->id, "create", "comment", comment->id);
    return comment;
}

int removeComment(const User *actor, Comment *comment, AppError *err)
{
    int commentId = comment->id;
    if (strcmp(actor->role, "admin") != 0 && comment->authorId != actor->id) {
        return errForbidden(err);
    }
    dropComment(comment);
    record(actor->id, "delete", "comment", commentId);
    return 1;
}

/* ----------------------------------------------------------------------- users */

User *createUser(const User *actor, const UserInput *input, AppError *err)
{
    Errors errors;
    User *user = NULL;
    errors.count = 0;
    checkString(input->username, "username", MAX_NAME_LENGTH, &errors);
    checkString(input->password, "password", MAX_NAME_LENGTH, &errors);
    checkRole(input->role, input->roleIsString, &errors);
    checkQuota(input->quota, input->quotaIsInt, &errors);
    if (errors.count > 0) {
        errInvalid(err, &errors);
        return NULL;
    }
    if (findByUsername(input->username) != NULL) {
        errConflict(err);
        return NULL;
    }
    user = insertUser(input->username, input->password, input->role, input->quota);
    if (user == NULL) {
        errConflict(err);
        return NULL;
    }
    record(actor->id, "create", "user", user->id);
    return user;
}

int updateUser(const User *actor, User *target, const UserInput *input, AppError *err)
{
    Errors errors;
    errors.count = 0;
    if (input->hasRole) checkRole(input->role, input->roleIsString, &errors);
    if (input->hasQuota) checkQuota(input->quota, input->quotaIsInt, &errors);
    if (errors.count > 0) return errInvalid(err, &errors);
    if (input->hasRole) copyText(target->role, sizeof target->role, input->role);
    if (input->hasQuota) target->quota = input->quota;
    target->version += 1;
    record(actor->id, "update", "user", target->id);
    return 1;
}

int deleteUser(const User *actor, User *target, AppError *err)
{
    if (target->id == actor->id) return errConflict(err);
    target->deleted = 1;
    target->version += 1;
    record(actor->id, "delete", "user", target->id);
    return 1;
}

/* ----------------------------------------------------- queries and reports */

static int canReach(const User *user, int projectId)
{
    const Project *project = findProject(projectId, 1);
    if (project == NULL) return 0;
    return strcmp(user->role, "admin") == 0 || project->ownerId == user->id;
}

int visibleProjects(const User *user, int includeDeleted, void **out)
{
    int total = 0;
    for (int index = 0; index < projectTotal; index += 1) {
        Project *project = &projects[index];
        if (project->deleted && !includeDeleted) continue;
        if (strcmp(user->role, "admin") != 0 && project->ownerId != user->id) continue;
        out[total] = project;
        total += 1;
    }
    return total;
}

int visibleTasks(const User *user, int includeDeleted, void **out)
{
    int total = 0;
    for (int index = 0; index < taskTotal; index += 1) {
        Task *task = &tasks[index];
        if (task->deleted && !includeDeleted) continue;
        if (!canReach(user, task->projectId)) continue;
        out[total] = task;
        total += 1;
    }
    return total;
}

int search(const User *user, const char *query, char *out)
{
    static void *rows[MAX_TASKS];
    int length = sprintf(out, "{\"results\":[");
    int total = 0;
    int count = visibleProjects(user, 0, rows);
    for (int index = 0; index < count; index += 1) {
        const Project *project = rows[index];
        if (!containsIgnoreCase(project->name, query)) continue;
        if (total > 0) length += sprintf(out + length, ",");
        length += sprintf(out + length, "{\"type\":\"project\",\"id\":%d,\"label\":", project->id);
        length += writeJsonString(out + length, project->name);
        length += sprintf(out + length, "}");
        total += 1;
    }
    count = visibleTasks(user, 0, rows);
    for (int index = 0; index < count; index += 1) {
        const Task *task = rows[index];
        if (!containsIgnoreCase(task->title, query)) continue;
        if (total > 0) length += sprintf(out + length, ",");
        length += sprintf(out + length, "{\"type\":\"task\",\"id\":%d,\"label\":", task->id);
        length += writeJsonString(out + length, task->title);
        length += sprintf(out + length, "}");
        total += 1;
    }
    length += sprintf(out + length, "],\"total\":%d}", total);
    return length;
}

static int writeGroup(char *out, int first, const char *key, int count, int score)
{
    int length = first ? 0 : sprintf(out, ",");
    length += sprintf(out + length, "{\"key\":");
    length += writeJsonString(out + length, key);
    length += sprintf(out + length, ",\"tasks\":%d,\"totalScore\":%d}", count, score);
    return length;
}

int workload(const User *user, const char *groupBy, char *out)
{
    static void *rows[MAX_TASKS];
    int count = visibleTasks(user, 0, rows);
    int length = sprintf(out, "{\"groupBy\":\"%s\",\"groups\":[", groupBy);
    int groups = 0;
    if (strcmp(groupBy, "status") == 0) {
        for (int slot = 0; slot < 4; slot += 1) {
            int picked = 0;
            int score = 0;
            for (int index = 0; index < count; index += 1) {
                const Task *task = rows[index];
                if (strcmp(task->status, STATUS_NAMES[slot]) != 0) continue;
                picked += 1;
                score += computeScore(task->priority, task->status);
            }
            length += writeGroup(out + length, groups == 0, STATUS_NAMES[slot], picked, score);
            groups += 1;
        }
    } else if (strcmp(groupBy, "assignee") == 0) {
        int named[MAX_USERS];
        int total = 0;
        int loose = 0;
        int looseScore = 0;
        for (int index = 0; index < count; index += 1) {
            const Task *task = rows[index];
            int seen = 0;
            if (task->assigneeId == 0) continue;
            for (int slot = 0; slot < total; slot += 1) {
                if (named[slot] == task->assigneeId) seen = 1;
            }
            if (!seen && total < MAX_USERS) {
                named[total] = task->assigneeId;
                total += 1;
            }
        }
        for (int slot = 0; slot < total; slot += 1) {
            for (int other = slot + 1; other < total; other += 1) {
                if (named[other] < named[slot]) {
                    int swap = named[slot];
                    named[slot] = named[other];
                    named[other] = swap;
                }
            }
        }
        for (int slot = 0; slot < total; slot += 1) {
            char key[16];
            int picked = 0;
            int score = 0;
            for (int index = 0; index < count; index += 1) {
                const Task *task = rows[index];
                if (task->assigneeId != named[slot]) continue;
                picked += 1;
                score += computeScore(task->priority, task->status);
            }
            sprintf(key, "%d", named[slot]);
            length += writeGroup(out + length, groups == 0, key, picked, score);
            groups += 1;
        }
        for (int index = 0; index < count; index += 1) {
            const Task *task = rows[index];
            if (task->assigneeId != 0) continue;
            loose += 1;
            looseScore += computeScore(task->priority, task->status);
        }
        if (loose > 0) {
            length += writeGroup(out + length, groups == 0, "unassigned", loose, looseScore);
            groups += 1;
        }
    } else {
        static void *owned[MAX_PROJECTS];
        int total = visibleProjects(user, 0, owned);
        for (int slot = 0; slot < total; slot += 1) {
            const Project *project = owned[slot];
            int picked = 0;
            int score = 0;
            for (int index = 0; index < count; index += 1) {
                const Task *task = rows[index];
                if (task->projectId != project->id) continue;
                picked += 1;
                score += computeScore(task->priority, task->status);
            }
            length += writeGroup(out + length, groups == 0, project->name, picked, score);
            groups += 1;
        }
    }
    length += sprintf(out + length, "]}");
    return length;
}

int flushOutbox(void)
{
    int flushed = 0;
    for (int index = 0; index < outboxTotal; index += 1) {
        if (outbox[index].delivered) continue;
        outbox[index].delivered = 1;
        flushed += 1;
    }
    return flushed;
}

static int compareCodes(const void *left, const void *right)
{
    const StatusCount *a = left;
    const StatusCount *b = right;
    return a->code - b->code;
}

static int compareRoutes(const void *left, const void *right)
{
    const RouteCount *a = *(const RouteCount *const *)left;
    const RouteCount *b = *(const RouteCount *const *)right;
    int result = strcmp(a->route, b->route);
    return result != 0 ? result : a->count - b->count;
}

int metrics(char *out)
{
    StatusCount codes[MAX_CODES];
    const RouteCount *routes[MAX_ROUTES];
    int length = sprintf(out, "{\"requests\":%d,\"byStatus\":{", requests);
    memcpy(codes, byStatus, (size_t)codeTotal * sizeof codes[0]);
    qsort(codes, (size_t)codeTotal, sizeof codes[0], compareCodes);
    for (int index = 0; index < codeTotal; index += 1) {
        if (index > 0) length += sprintf(out + length, ",");
        length += sprintf(out + length, "\"%d\":%d", codes[index].code, codes[index].count);
    }
    length += sprintf(out + length, "},\"byRoute\":[");
    for (int index = 0; index < routeTotal; index += 1) routes[index] = &byRoute[index];
    qsort(routes, (size_t)routeTotal, sizeof routes[0], compareRoutes);
    for (int index = 0; index < routeTotal; index += 1) {
        if (index > 0) length += sprintf(out + length, ",");
        length += sprintf(out + length, "{\"route\":");
        length += writeJsonString(out + length, routes[index]->route);
        length += sprintf(out + length, ",\"count\":%d}", routes[index]->count);
    }
    length += sprintf(out + length, "],\"auditEntries\":%d,\"outboxPending\":%d}",
                      auditTotal, outboxPending());
    return length;
}

int stats(char *out)
{
    const Project *best = NULL;
    double avgScore = 0.0;
    int counts[4] = {0, 0, 0, 0};
    int sumScore = 0;
    int total = 0;
    int length = 0;
    for (int index = 0; index < taskTotal; index += 1) {
        const Task *task = &tasks[index];
        int slot = statusIndex(task->status);
        if (task->deleted) continue;
        if (slot >= 0) counts[slot] += 1;
        sumScore += computeScore(task->priority, task->status);
        total += 1;
    }
    if (total > 0) avgScore = round((double)sumScore / total * 100.0) / 100.0;
    for (int index = 0; index < projectTotal; index += 1) {
        const Project *project = &projects[index];
        if (project->deleted) continue;
        if (best == NULL || taskCount(project->id) > taskCount(best->id)) best = project;
    }
    length = sprintf(out, "{\"projects\":%d,\"tasks\":%d,\"users\":%d,\"sessions\":%d,"
                          "\"comments\":%d,\"byStatus\":{\"todo\":%d,\"in_progress\":%d,"
                          "\"done\":%d,\"archived\":%d},\"avgScore\":%g,\"topProjectName\":",
                     liveProjects(), total, liveUsers(), sessionTotal, commentTotal,
                     counts[0], counts[1], counts[2], counts[3], avgScore);
    if (best == NULL) length += sprintf(out + length, "null");
    else length += writeJsonString(out + length, best->name);
    length += sprintf(out + length, ",\"auditEntries\":%d,\"outboxPending\":%d}",
                      auditTotal, outboxPending());
    return length;
}

int checkBulkSize(int count, AppError *err)
{
    if (count < 1 || count > MAX_BULK_ITEMS) {
        return errInvalidField(err, "operations", "operations is out of range");
    }
    return 1;
}
