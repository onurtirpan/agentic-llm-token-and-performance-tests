/* Task Service, large tier — HTTP routing, middleware and the entry point. */

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "domain.h"
#include "service.h"
#include "store.h"

#define MAX_KEEP_ALIVE 1000
#define RECEIVE_TIMEOUT 5000

typedef struct {
    char method[16];
    char path[TEXT_SIZE];
    const char *query;
    const char *headers;
    const char *body;
    char requestId[128];
    char token[TOKEN_SIZE];
    const char *pattern;
    int sessionIndex;
    int userId;
    int quotaRemaining;
    int charged;
    int replayed;
    int etag;
    int status;
    int length;
    char out[RESPONSE_SIZE];
} Context;

/* ------------------------------------------------------------- HTTP parsing */

static int equalsIgnoreCase(const char *left, const char *right, size_t length)
{
    for (size_t index = 0; index < length; index += 1) {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) return 0;
    }
    return 1;
}

static int headerValue(const char *headers, const char *name, char *out, size_t limit)
{
    size_t nameLength = strlen(name);
    const char *line = headers;
    out[0] = '\0';
    while (*line != '\0' && strncmp(line, "\r\n", 2) != 0) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL) end = line + strlen(line);
        if ((size_t)(end - line) > nameLength && equalsIgnoreCase(line, name, nameLength)
            && line[nameLength] == ':') {
            const char *value = line + nameLength + 1;
            size_t length = 0;
            while (*value == ' ' || *value == '\t') value += 1;
            length = (size_t)(end - value);
            if (length >= limit) length = limit - 1;
            memcpy(out, value, length);
            out[length] = '\0';
            return 1;
        }
        if (*end == '\0') break;
        line = end + 2;
    }
    return 0;
}

static int queryValue(const char *query, const char *name, char *out, size_t limit)
{
    size_t nameLength = strlen(name);
    const char *cursor = query;
    out[0] = '\0';
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        if (end == NULL) end = cursor + strlen(cursor);
        if ((size_t)(end - cursor) > nameLength && strncmp(cursor, name, nameLength) == 0
            && cursor[nameLength] == '=') {
            size_t length = (size_t)(end - cursor) - nameLength - 1;
            if (length >= limit) length = limit - 1;
            memcpy(out, cursor + nameLength + 1, length);
            out[length] = '\0';
            return 1;
        }
        cursor = (*end == '\0') ? end : end + 1;
    }
    return 0;
}

static int matchPath(const char *path, const char *pattern, char *out, size_t limit)
{
    size_t length = 0;
    if (out != NULL) out[0] = '\0';
    while (*pattern != '\0') {
        if (*pattern == '%') {
            while (*path != '\0' && *path != '/') {
                if (out == NULL || length + 1 >= limit) return 0;
                out[length] = *path;
                length += 1;
                path += 1;
            }
            if (out != NULL) out[length] = '\0';
            pattern += 1;
            continue;
        }
        if (*path != *pattern) return 0;
        path += 1;
        pattern += 1;
    }
    return *path == '\0';
}

static void routeLabel(char *out, size_t limit, const char *method, const char *pattern)
{
    size_t length = (size_t)sprintf(out, "%s ", method);
    for (const char *cursor = pattern; *cursor != '\0'; cursor += 1) {
        if (*cursor == '%') {
            if (length + 4 < limit) {
                memcpy(out + length, "{id}", 4);
                length += 4;
            }
        } else if (length + 1 < limit) {
            out[length] = *cursor;
            length += 1;
        }
    }
    out[length] = '\0';
}

/* ------------------------------------------------------------- request reads */

static int ok(Context *ctx, int status, int length, int etag)
{
    ctx->status = status;
    ctx->length = length;
    ctx->etag = etag;
    return 1;
}

static int readBody(Context *ctx, JsonBody *body, AppError *err)
{
    const char *cursor = skipSpace(ctx->body);
    body->count = 0;
    body->kind = JSON_OBJECT;
    if (*cursor == '\0') return 1;
    if (parseJsonObject(cursor, body) == NULL) return errBadRequest(err);
    return 1;
}

static int readString(const JsonBody *body, const char *name, char *out, size_t limit,
                      AppError *err)
{
    const JsonField *field = findField(body, name);
    out[0] = '\0';
    if (field == NULL) return 1;
    if (field->kind != JSON_STRING) return errBadRequest(err);
    copyText(out, limit, field->text);
    return 1;
}

static int readInt(const JsonBody *body, const char *name, int def, int defNull, IntRef *out,
                   AppError *err)
{
    const JsonField *field = findField(body, name);
    out->value = def;
    out->isNull = defNull;
    if (field == NULL) return 1;
    if (field->kind == JSON_NULL) {
        out->value = 0;
        out->isNull = 1;
        return 1;
    }
    if (field->kind == JSON_INT) {
        out->value = (int)field->number;
        out->isNull = 0;
        return 1;
    }
    return errBadRequest(err);
}

static int readTaskInput(const JsonBody *body, const User *actor, const char *current,
                         TaskInput *input, Errors *errors, AppError *err)
{
    const JsonField *note = findField(body, "internalNote");
    IntRef priority;
    IntRef assignee;
    copyText(input->internalNote, sizeof input->internalNote, current);
    input->hasNote = note != NULL;
    if (note != NULL) {
        if (strcmp(actor->role, "admin") != 0) return errForbidden(err);
        if (note->kind != JSON_STRING) return errBadRequest(err);
        copyText(input->internalNote, sizeof input->internalNote, note->text);
        if (strlen(note->text) > MAX_TITLE_LENGTH) {
            fail(errors, "internalNote", "internalNote is too long");
        }
    }
    if (!readString(body, "title", input->title, sizeof input->title, err)) return 0;
    if (!readInt(body, "priority", 0, 0, &priority, err)) return 0;
    if (!readInt(body, "assigneeId", 0, 1, &assignee, err)) return 0;
    input->priority = priority.isNull ? 0 : priority.value;
    input->priorityNull = priority.isNull;
    input->assigneeId = assignee.isNull ? 0 : assignee.value;
    return 1;
}

static int readUserInput(const JsonBody *body, UserInput *input, AppError *err)
{
    const JsonField *role = findField(body, "role");
    const JsonField *quota = findField(body, "quota");
    if (!readString(body, "username", input->username, sizeof input->username, err)) return 0;
    if (!readString(body, "password", input->password, sizeof input->password, err)) return 0;
    copyText(input->role, sizeof input->role, "user");
    input->hasRole = role != NULL;
    input->roleIsString = 1;
    if (role != NULL) {
        input->roleIsString = role->kind == JSON_STRING;
        copyText(input->role, sizeof input->role, role->kind == JSON_STRING ? role->text : "");
    }
    input->quota = DEFAULT_QUOTA;
    input->hasQuota = quota != NULL;
    input->quotaIsInt = 1;
    if (quota != NULL) {
        input->quotaIsInt = quota->kind == JSON_INT;
        input->quota = quota->kind == JSON_INT ? (int)quota->number : 0;
    }
    return 1;
}

static int parseIdValue(const char *raw, int *out, AppError *err)
{
    char *end = NULL;
    long value = 0;
    if (raw[0] == '\0') return errBadRequest(err);
    value = strtol(raw, &end, 10);
    if (*end != '\0') return errBadRequest(err);
    *out = (int)value;
    return 1;
}

static int readPage(Context *ctx, const char *const *allowed, int allowedCount, Page *page,
                    AppError *err)
{
    Errors errors;
    char raw[64];
    errors.count = 0;
    page->limit = DEFAULT_LIMIT;
    page->offset = 0;
    copyText(page->sort, sizeof page->sort, allowed[0]);
    copyText(page->order, sizeof page->order, "asc");
    if (queryValue(ctx->query, "limit", raw, sizeof raw)) {
        if (!parseWholeNumber(raw, &page->limit)) page->limit = -1;
        if (page->limit < 1 || page->limit > MAX_LIMIT) {
            fail(&errors, "limit", "limit is out of range");
        }
    }
    if (queryValue(ctx->query, "offset", raw, sizeof raw)) {
        if (!parseWholeNumber(raw, &page->offset)) page->offset = -1;
        if (page->offset < 0) fail(&errors, "offset", "offset is out of range");
    }
    if (queryValue(ctx->query, "sort", raw, sizeof raw)) {
        copyText(page->sort, sizeof page->sort, raw);
    }
    if (queryValue(ctx->query, "order", raw, sizeof raw)) {
        copyText(page->order, sizeof page->order, raw);
    }
    if (!isMember(page->sort, allowed, allowedCount)) {
        fail(&errors, "sort", "sort is not a valid field");
    }
    if (strcmp(page->order, "asc") != 0 && strcmp(page->order, "desc") != 0) {
        fail(&errors, "order", "order must be asc or desc");
    }
    if (errors.count > 0) return errInvalid(err, &errors);
    if (page->limit < 0) page->limit = 0;
    if (page->offset < 0) page->offset = 0;
    return 1;
}

static int readIfMatch(Context *ctx, int version, AppError *err)
{
    char header[64];
    int present = headerValue(ctx->headers, "If-Match", header, sizeof header);
    return checkIfMatch(header, present, version, err);
}

static int readIncludeDeleted(Context *ctx, const User *user, int *out, AppError *err)
{
    char raw[32];
    int present = queryValue(ctx->query, "includeDeleted", raw, sizeof raw);
    return checkIncludeDeleted(raw, present, user, out, err);
}

/* ------------------------------------------------------------ middleware bits */

static int writeError(Context *ctx, const AppError *err)
{
    int length = sprintf(ctx->out, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"requestId\":",
                         err->code, err->message);
    length += writeJsonString(ctx->out + length, ctx->requestId);
    length += sprintf(ctx->out + length, ",\"details\":[");
    for (int index = 0; index < err->details.count; index += 1) {
        if (index > 0) length += sprintf(ctx->out + length, ",");
        length += sprintf(ctx->out + length, "{\"field\":\"%s\",\"message\":\"%s\"}",
                          err->details.items[index].field, err->details.items[index].message);
    }
    length += sprintf(ctx->out + length, "]}}");
    ctx->status = err->status;
    ctx->length = length;
    ctx->etag = -1;
    return 0;
}

static int replayed(Context *ctx)
{
    char key[KEY_SIZE];
    const Slot *slot = NULL;
    if (strcmp(ctx->method, "POST") != 0) return 0;
    if (!headerValue(ctx->headers, "Idempotency-Key", key, sizeof key) || key[0] == '\0') return 0;
    slot = findSlot(ctx->token, key);
    if (slot == NULL) return 0;
    memcpy(ctx->out, slot->body, (size_t)slot->length);
    ctx->status = slot->status;
    ctx->length = slot->length;
    ctx->etag = slot->etag;
    ctx->replayed = 1;
    return 1;
}

static void keepResponse(Context *ctx)
{
    char key[KEY_SIZE];
    if (!ctx->charged || ctx->replayed) return;
    if (strcmp(ctx->method, "POST") != 0) return;
    if (!headerValue(ctx->headers, "Idempotency-Key", key, sizeof key) || key[0] == '\0') return;
    putSlot(ctx->token, key, ctx->status, ctx->etag, ctx->out, ctx->length);
}

static int begin(Context *ctx, int admin, User **outUser, AppError *err)
{
    char header[TEXT_SIZE];
    User *user = NULL;
    int index = -1;
    int remaining = 0;
    headerValue(ctx->headers, "Authorization", header, sizeof header);
    user = authenticate(header, &index, err);
    if (user == NULL) return 0;
    ctx->sessionIndex = index;
    ctx->userId = user->id;
    copyText(ctx->token, sizeof ctx->token, sessions[index].token);
    remaining = chargeQuota(user, &sessions[index], err);
    if (remaining < 0) return 0;
    ctx->quotaRemaining = remaining;
    ctx->charged = 1;
    if (replayed(ctx)) return 0;
    if (admin && !requireAdmin(user, err)) return 0;
    *outUser = user;
    return 1;
}

static int isAdmin(const User *user)
{
    return strcmp(user->role, "admin") == 0;
}

/* --------------------------------------------------------------- health, auth */

static int handleHealth(Context *ctx, AppError *err)
{
    (void)err;
    return ok(ctx, 200, sprintf(ctx->out,
                                "{\"status\":\"ok\",\"projects\":%d,\"tasks\":%d,\"comments\":%d}",
                                liveProjects(), liveTasks(), commentTotal), -1);
}

static void makeToken(char *out)
{
    for (int index = 0; index < TOKEN_SIZE - 8; index += 1) {
        out[index] = "0123456789abcdef"[rand() % 16];
    }
    out[TOKEN_SIZE - 8] = '\0';
}

static int handleLogin(Context *ctx, AppError *err)
{
    JsonBody body;
    Errors errors;
    char username[TEXT_SIZE];
    char password[TEXT_SIZE];
    char token[TOKEN_SIZE];
    const User *user = NULL;
    if (!readBody(ctx, &body, err)) return 0;
    if (!readString(&body, "username", username, sizeof username, err)) return 0;
    if (!readString(&body, "password", password, sizeof password, err)) return 0;
    errors.count = 0;
    if (username[0] == '\0') fail(&errors, "username", "username is required");
    if (password[0] == '\0') fail(&errors, "password", "password is required");
    if (errors.count > 0) return errInvalid(err, &errors);
    makeToken(token);
    user = login(username, password, token, err);
    if (user == NULL) return 0;
    return ok(ctx, 200, sprintf(ctx->out, "{\"token\":\"%s\",\"userId\":%d,\"role\":\"%s\"}",
                                token, user->id, user->role), -1);
}

static int handleLogout(Context *ctx, AppError *err)
{
    User *actor = NULL;
    if (!begin(ctx, 0, &actor, err)) return 0;
    removeSession(ctx->sessionIndex);
    return ok(ctx, 204, 0, -1);
}

static int handleMe(Context *ctx, AppError *err)
{
    User *actor = NULL;
    int length = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    length = sprintf(ctx->out, "{\"userId\":%d,\"username\":", actor->id);
    length += writeJsonString(ctx->out + length, actor->username);
    length += sprintf(ctx->out + length, ",\"role\":\"%s\"}", actor->role);
    return ok(ctx, 200, length, -1);
}

/* ---------------------------------------------------------------------- users */

static int handleListUsers(Context *ctx, AppError *err)
{
    static void *rows[MAX_USERS];
    User *actor = NULL;
    Page page;
    int total = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!readPage(ctx, USER_SORTS, COUNT(USER_SORTS), &page, err)) return 0;
    for (int index = 0; index < userTotal; index += 1) {
        if (users[index].deleted) continue;
        rows[total] = &users[index];
        total += 1;
    }
    return ok(ctx, 200,
              paginate(ctx->out, rows, total, &page, compareUsers, serializeUser, 1), -1);
}

static int handleCreateUser(Context *ctx, AppError *err)
{
    JsonBody body;
    UserInput input;
    User *actor = NULL;
    const User *created = NULL;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    if (!readUserInput(&body, &input, err)) return 0;
    created = createUser(actor, &input, err);
    if (created == NULL) return 0;
    return ok(ctx, 201, serializeUser(ctx->out, created, 1), created->version);
}

static int handleGetUser(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    const User *target = NULL;
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    target = findUser(id, 0);
    if (target == NULL) return errNotFound(err);
    return ok(ctx, 200, serializeUser(ctx->out, target, 1), target->version);
}

static int handlePatchUser(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    UserInput input;
    User *actor = NULL;
    User *target = NULL;
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    target = findUser(id, 0);
    if (target == NULL) return errNotFound(err);
    if (!readIfMatch(ctx, target->version, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    if (!readUserInput(&body, &input, err)) return 0;
    if (!updateUser(actor, target, &input, err)) return 0;
    return ok(ctx, 200, serializeUser(ctx->out, target, 1), target->version);
}

static int handleDeleteUser(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    User *target = NULL;
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    target = findUser(id, 0);
    if (target == NULL) return errNotFound(err);
    if (!readIfMatch(ctx, target->version, err)) return 0;
    if (!deleteUser(actor, target, err)) return 0;
    return ok(ctx, 200, serializeUser(ctx->out, target, 1), target->version);
}

/* ------------------------------------------------------------------- projects */

static int handleListProjects(Context *ctx, AppError *err)
{
    static void *rows[MAX_PROJECTS];
    User *actor = NULL;
    Page page;
    int include = 0;
    int total = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!readIncludeDeleted(ctx, actor, &include, err)) return 0;
    if (!readPage(ctx, PROJECT_SORTS, COUNT(PROJECT_SORTS), &page, err)) return 0;
    total = visibleProjects(actor, include, rows);
    return ok(ctx, 200,
              paginate(ctx->out, rows, total, &page, compareProjects, serializeProject, 1), -1);
}

static int handleCreateProject(Context *ctx, AppError *err)
{
    JsonBody body;
    User *actor = NULL;
    const Project *project = NULL;
    char name[TEXT_SIZE];
    IntRef owner;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    if (!readString(&body, "name", name, sizeof name, err)) return 0;
    if (!readInt(&body, "ownerId", actor->id, 0, &owner, err)) return 0;
    project = createProject(actor, name, owner.isNull ? 0 : owner.value, err);
    if (project == NULL) return 0;
    return ok(ctx, 201, serializeProject(ctx->out, project, 1), project->version);
}

static int handleGetProject(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    const Project *project = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 0, err);
    if (project == NULL) return 0;
    return ok(ctx, 200, serializeProject(ctx->out, project, 1), project->version);
}

static int handlePatchProject(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    User *actor = NULL;
    Project *project = NULL;
    char name[TEXT_SIZE];
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 0, err);
    if (project == NULL) return 0;
    if (!readIfMatch(ctx, project->version, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    if (findField(&body, "name") != NULL) {
        if (!readString(&body, "name", name, sizeof name, err)) return 0;
        if (!renameProject(actor, project, name, err)) return 0;
    }
    return ok(ctx, 200, serializeProject(ctx->out, project, 1), project->version);
}

static int handleDeleteProject(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    Project *project = NULL;
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 0, err);
    if (project == NULL) return 0;
    if (!readIfMatch(ctx, project->version, err)) return 0;
    deleteProject(actor, project);
    return ok(ctx, 200, serializeProject(ctx->out, project, 1), project->version);
}

static int handleRestoreProject(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    Project *project = NULL;
    int id = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 1, err);
    if (project == NULL) return 0;
    if (!readIfMatch(ctx, project->version, err)) return 0;
    if (!restoreProject(actor, project, err)) return 0;
    return ok(ctx, 200, serializeProject(ctx->out, project, 1), project->version);
}

/* ---------------------------------------------------------------------- tasks */

static int taskFilters(Context *ctx, void **rows, int *total, AppError *err)
{
    Errors errors;
    char status[32];
    char raw[32];
    int hasStatus = queryValue(ctx->query, "status", status, sizeof status);
    int hasAssignee = queryValue(ctx->query, "assigneeId", raw, sizeof raw);
    int assignee = 0;
    int kept = 0;
    errors.count = 0;
    if (hasStatus && statusIndex(status) < 0) fail(&errors, "status", "status is not valid");
    if (hasAssignee && !parseWholeNumber(raw, &assignee)) {
        fail(&errors, "assigneeId", "assigneeId is not a known user");
    }
    if (errors.count > 0) return errInvalid(err, &errors);
    for (int index = 0; index < *total; index += 1) {
        const Task *task = rows[index];
        if (hasStatus && strcmp(task->status, status) != 0) continue;
        if (hasAssignee && task->assigneeId != assignee) continue;
        rows[kept] = rows[index];
        kept += 1;
    }
    *total = kept;
    return 1;
}

static int handleListTasks(Context *ctx, AppError *err)
{
    static void *rows[MAX_TASKS];
    User *actor = NULL;
    Page page;
    int include = 0;
    int total = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!readIncludeDeleted(ctx, actor, &include, err)) return 0;
    if (!readPage(ctx, TASK_SORTS, COUNT(TASK_SORTS), &page, err)) return 0;
    total = visibleTasks(actor, include, rows);
    if (!taskFilters(ctx, rows, &total, err)) return 0;
    return ok(ctx, 200, paginate(ctx->out, rows, total, &page, compareTasks, serializeTask,
                                 isAdmin(actor)), -1);
}

static int handleProjectTasks(Context *ctx, const char *raw, AppError *err)
{
    static void *rows[MAX_TASKS];
    User *actor = NULL;
    const Project *project = NULL;
    Page page;
    int id = 0;
    int total = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 0, err);
    if (project == NULL) return 0;
    if (!readPage(ctx, TASK_SORTS, COUNT(TASK_SORTS), &page, err)) return 0;
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].projectId != project->id || tasks[index].deleted) continue;
        rows[total] = &tasks[index];
        total += 1;
    }
    return ok(ctx, 200, paginate(ctx->out, rows, total, &page, compareTasks, serializeTask,
                                 isAdmin(actor)), -1);
}

static int handleCreateTask(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    TaskInput input;
    Errors errors;
    User *actor = NULL;
    const Project *project = NULL;
    const Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    project = reachableProject(id, actor, 0, err);
    if (project == NULL) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    errors.count = 0;
    if (!readTaskInput(&body, actor, "", &input, &errors, err)) return 0;
    task = createTask(actor, project, &input, &errors, err);
    if (task == NULL) return 0;
    return ok(ctx, 201, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

static int handleGetTask(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    const Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    return ok(ctx, 200, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

static int handlePutTask(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    TaskInput input;
    Errors errors;
    User *actor = NULL;
    Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    if (!readIfMatch(ctx, task->version, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    errors.count = 0;
    if (!readTaskInput(&body, actor, task->internalNote, &input, &errors, err)) return 0;
    if (!replaceTask(actor, task, &input, &errors, err)) return 0;
    return ok(ctx, 200, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

static int handleTaskStatus(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    const JsonField *field = NULL;
    User *actor = NULL;
    Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    if (!readIfMatch(ctx, task->version, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    field = findField(&body, "status");
    if (!moveStatus(actor, task, field == NULL ? "" : field->text,
                    field != NULL && field->kind == JSON_STRING, err)) {
        return 0;
    }
    return ok(ctx, 200, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

static int handleDeleteTask(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    if (!readIfMatch(ctx, task->version, err)) return 0;
    deleteTask(actor, task);
    return ok(ctx, 200, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

static int handleRestoreTask(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    Task *task = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 1, err);
    if (task == NULL) return 0;
    if (!readIfMatch(ctx, task->version, err)) return 0;
    if (!restoreTask(actor, task, err)) return 0;
    return ok(ctx, 200, serializeTask(ctx->out, task, isAdmin(actor)), task->version);
}

/* ---------------------------------------------------------------- bulk writes */

static int bulkCreate(const User *actor, const JsonBody *item, int *id, AppError *err)
{
    TaskInput input;
    Errors errors;
    IntRef reference;
    const Project *project = NULL;
    const Task *task = NULL;
    errors.count = 0;
    memset(&input, 0, sizeof input);
    if (!readInt(item, "projectId", 0, 0, &reference, err)) return err->status;
    project = reachableProject(reference.isNull ? 0 : reference.value, actor, 0, err);
    if (project == NULL) return err->status;
    if (!readString(item, "title", input.title, sizeof input.title, err)) return err->status;
    if (!readInt(item, "priority", 0, 0, &reference, err)) return err->status;
    input.priority = reference.isNull ? 0 : reference.value;
    input.priorityNull = reference.isNull;
    task = createTask(actor, project, &input, &errors, err);
    if (task == NULL) return err->status;
    *id = task->id;
    return 201;
}

static int bulkWrite(const User *actor, const JsonBody *item, int move, int *id, AppError *err)
{
    const JsonField *version = findField(item, "version");
    const JsonField *field = NULL;
    IntRef reference;
    Task *task = NULL;
    char raw[32];
    if (!readInt(item, "id", 0, 0, &reference, err)) return err->status;
    task = reachableTask(reference.isNull ? 0 : reference.value, actor, 0, err);
    if (task == NULL) return err->status;
    if (version != NULL && version->kind == JSON_INT) sprintf(raw, "%ld", version->number);
    else copyText(raw, sizeof raw, "none");
    if (!checkIfMatch(raw, 1, task->version, err)) return err->status;
    if (move) {
        field = findField(item, "status");
        if (!moveStatus(actor, task, field == NULL ? "" : field->text,
                        field != NULL && field->kind == JSON_STRING, err)) {
            return err->status;
        }
    } else {
        deleteTask(actor, task);
    }
    *id = task->id;
    return 200;
}

static int applyBulk(const User *actor, const JsonBody *item, int *id, AppError *err)
{
    const JsonField *op = findField(item, "op");
    const char *name = (op != NULL && op->kind == JSON_STRING) ? op->text : "";
    if (item->kind != JSON_OBJECT) {
        errBadRequest(err);
        return err->status;
    }
    if (strcmp(name, "create") == 0) return bulkCreate(actor, item, id, err);
    if (strcmp(name, "status") == 0) return bulkWrite(actor, item, 1, id, err);
    if (strcmp(name, "delete") == 0) return bulkWrite(actor, item, 0, id, err);
    errInvalidField(err, "op", "op is not valid");
    return err->status;
}

static int handleBulk(Context *ctx, AppError *err)
{
    static JsonBody items[MAX_BULK_ITEMS + 1];
    JsonBody body;
    User *actor = NULL;
    const JsonField *field = NULL;
    int count = 0;
    int length = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    field = findField(&body, "operations");
    if (field == NULL || field->kind != JSON_ARRAY) {
        return errInvalidField(err, "operations", "operations is out of range");
    }
    if (!parseJsonArray(field->raw, items, MAX_BULK_ITEMS + 1, &count)) return errBadRequest(err);
    if (!checkBulkSize(count, err)) return 0;
    length = sprintf(ctx->out, "{\"results\":[");
    for (int index = 0; index < count; index += 1) {
        AppError itemError;
        int id = 0;
        int status = 0;
        resetError(&itemError);
        status = applyBulk(actor, &items[index], &id, &itemError);
        if (index > 0) length += sprintf(ctx->out + length, ",");
        length += sprintf(ctx->out + length, "{\"index\":%d,\"status\":%d,\"id\":", index, status);
        if (id == 0) length += sprintf(ctx->out + length, "null");
        else length += sprintf(ctx->out + length, "%d", id);
        if (status >= 400) {
            length += sprintf(ctx->out + length, ",\"error\":\"%s\"}", itemError.code);
        } else {
            length += sprintf(ctx->out + length, ",\"error\":null}");
        }
    }
    length += sprintf(ctx->out + length, "]}");
    return ok(ctx, 200, length, -1);
}

/* ------------------------------------------------------------------- comments */

static int handleListComments(Context *ctx, const char *raw, AppError *err)
{
    static void *rows[MAX_COMMENTS];
    User *actor = NULL;
    const Task *task = NULL;
    Page page;
    int id = 0;
    int total = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    if (!readPage(ctx, COMMENT_SORTS, COUNT(COMMENT_SORTS), &page, err)) return 0;
    for (int index = 0; index < commentTotal; index += 1) {
        if (comments[index].taskId != task->id) continue;
        rows[total] = &comments[index];
        total += 1;
    }
    return ok(ctx, 200,
              paginate(ctx->out, rows, total, &page, compareComments, serializeComment, 1), -1);
}

static int handleCreateComment(Context *ctx, const char *raw, AppError *err)
{
    JsonBody body;
    User *actor = NULL;
    const Task *task = NULL;
    const Comment *comment = NULL;
    char text[TEXT_SIZE];
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    task = reachableTask(id, actor, 0, err);
    if (task == NULL) return 0;
    if (!readBody(ctx, &body, err)) return 0;
    if (!readString(&body, "body", text, sizeof text, err)) return 0;
    comment = createComment(actor, task, text, err);
    if (comment == NULL) return 0;
    return ok(ctx, 201, serializeComment(ctx->out, comment, 1), -1);
}

static int handleDeleteComment(Context *ctx, const char *raw, AppError *err)
{
    User *actor = NULL;
    Comment *comment = NULL;
    int id = 0;
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!parseIdValue(raw, &id, err)) return 0;
    comment = findComment(id);
    if (comment == NULL) return errNotFound(err);
    if (reachableTask(comment->taskId, actor, 1, err) == NULL) return 0;
    if (!removeComment(actor, comment, err)) return 0;
    return ok(ctx, 204, 0, -1);
}

/* --------------------------------------------------- search, reports, telemetry */

static int handleSearch(Context *ctx, AppError *err)
{
    User *actor = NULL;
    char query[TEXT_SIZE];
    if (!begin(ctx, 0, &actor, err)) return 0;
    queryValue(ctx->query, "q", query, sizeof query);
    if (query[0] == '\0') return errInvalidField(err, "q", "q is required");
    return ok(ctx, 200, search(actor, query, ctx->out), -1);
}

static int handleWorkload(Context *ctx, AppError *err)
{
    User *actor = NULL;
    char groupBy[32];
    if (!begin(ctx, 0, &actor, err)) return 0;
    if (!queryValue(ctx->query, "groupBy", groupBy, sizeof groupBy)) {
        copyText(groupBy, sizeof groupBy, "status");
    }
    if (!isMember(groupBy, GROUP_BYS, COUNT(GROUP_BYS))) {
        return errInvalidField(err, "groupBy", "groupBy is not valid");
    }
    return ok(ctx, 200, workload(actor, groupBy, ctx->out), -1);
}

static int handleAudit(Context *ctx, AppError *err)
{
    static void *rows[MAX_AUDIT];
    User *actor = NULL;
    Page page;
    char actorId[32];
    char resource[32];
    char action[32];
    int hasActor = 0;
    int hasResource = 0;
    int hasAction = 0;
    int total = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!readPage(ctx, SEQ_SORTS, COUNT(SEQ_SORTS), &page, err)) return 0;
    hasActor = queryValue(ctx->query, "actorId", actorId, sizeof actorId);
    hasResource = queryValue(ctx->query, "resource", resource, sizeof resource);
    hasAction = queryValue(ctx->query, "action", action, sizeof action);
    for (int index = 0; index < auditTotal; index += 1) {
        AuditEntry *entry = &audit[index];
        char raw[16];
        sprintf(raw, "%d", entry->actorId);
        if (hasActor && strcmp(raw, actorId) != 0) continue;
        if (hasResource && strcmp(entry->resource, resource) != 0) continue;
        if (hasAction && strcmp(entry->action, action) != 0) continue;
        rows[total] = entry;
        total += 1;
    }
    return ok(ctx, 200,
              paginate(ctx->out, rows, total, &page, compareSeq, serializeAudit, 1), -1);
}

static int handleOutbox(Context *ctx, AppError *err)
{
    static void *rows[MAX_OUTBOX];
    User *actor = NULL;
    Page page;
    char wanted[32];
    int hasWanted = 0;
    int total = 0;
    if (!begin(ctx, 1, &actor, err)) return 0;
    if (!readPage(ctx, SEQ_SORTS, COUNT(SEQ_SORTS), &page, err)) return 0;
    hasWanted = queryValue(ctx->query, "delivered", wanted, sizeof wanted);
    for (int index = 0; index < outboxTotal; index += 1) {
        OutboxEvent *event = &outbox[index];
        if (hasWanted && event->delivered != (strcmp(wanted, "true") == 0)) continue;
        rows[total] = event;
        total += 1;
    }
    return ok(ctx, 200,
              paginate(ctx->out, rows, total, &page, compareSeq, serializeOutbox, 1), -1);
}

static int handleFlush(Context *ctx, AppError *err)
{
    User *actor = NULL;
    if (!begin(ctx, 1, &actor, err)) return 0;
    return ok(ctx, 200, sprintf(ctx->out, "{\"flushed\":%d}", flushOutbox()), -1);
}

static int handleMetrics(Context *ctx, AppError *err)
{
    User *actor = NULL;
    if (!begin(ctx, 1, &actor, err)) return 0;
    return ok(ctx, 200, metrics(ctx->out), -1);
}

static int handleStats(Context *ctx, AppError *err)
{
    User *actor = NULL;
    if (!begin(ctx, 1, &actor, err)) return 0;
    return ok(ctx, 200, stats(ctx->out), -1);
}

/* -------------------------------------------------------------------- routing */

static int at(Context *ctx, const char *method, const char *pattern, char *raw, size_t limit)
{
    if (strcmp(ctx->method, method) != 0) return 0;
    if (raw == NULL) {
        if (strcmp(ctx->path, pattern) != 0) return 0;
    } else if (!matchPath(ctx->path, pattern, raw, limit)) {
        return 0;
    }
    ctx->pattern = pattern;
    return 1;
}

static int route(Context *ctx, AppError *err)
{
    char raw[64];
    size_t size = sizeof raw;
    if (at(ctx, "GET", "/health", NULL, 0)) return handleHealth(ctx, err);
    if (at(ctx, "POST", "/auth/login", NULL, 0)) return handleLogin(ctx, err);
    if (at(ctx, "POST", "/auth/logout", NULL, 0)) return handleLogout(ctx, err);
    if (at(ctx, "GET", "/me", NULL, 0)) return handleMe(ctx, err);
    if (at(ctx, "GET", "/users", NULL, 0)) return handleListUsers(ctx, err);
    if (at(ctx, "POST", "/users", NULL, 0)) return handleCreateUser(ctx, err);
    if (at(ctx, "GET", "/users/%", raw, size)) return handleGetUser(ctx, raw, err);
    if (at(ctx, "PATCH", "/users/%", raw, size)) return handlePatchUser(ctx, raw, err);
    if (at(ctx, "DELETE", "/users/%", raw, size)) return handleDeleteUser(ctx, raw, err);
    if (at(ctx, "GET", "/projects", NULL, 0)) return handleListProjects(ctx, err);
    if (at(ctx, "POST", "/projects", NULL, 0)) return handleCreateProject(ctx, err);
    if (at(ctx, "GET", "/projects/%", raw, size)) return handleGetProject(ctx, raw, err);
    if (at(ctx, "PATCH", "/projects/%", raw, size)) return handlePatchProject(ctx, raw, err);
    if (at(ctx, "DELETE", "/projects/%", raw, size)) return handleDeleteProject(ctx, raw, err);
    if (at(ctx, "POST", "/projects/%/restore", raw, size)) {
        return handleRestoreProject(ctx, raw, err);
    }
    if (at(ctx, "GET", "/projects/%/tasks", raw, size)) return handleProjectTasks(ctx, raw, err);
    if (at(ctx, "POST", "/projects/%/tasks", raw, size)) return handleCreateTask(ctx, raw, err);
    if (at(ctx, "GET", "/tasks", NULL, 0)) return handleListTasks(ctx, err);
    if (at(ctx, "POST", "/tasks/bulk", NULL, 0)) return handleBulk(ctx, err);
    if (at(ctx, "GET", "/tasks/%", raw, size)) return handleGetTask(ctx, raw, err);
    if (at(ctx, "PUT", "/tasks/%", raw, size)) return handlePutTask(ctx, raw, err);
    if (at(ctx, "DELETE", "/tasks/%", raw, size)) return handleDeleteTask(ctx, raw, err);
    if (at(ctx, "PATCH", "/tasks/%/status", raw, size)) return handleTaskStatus(ctx, raw, err);
    if (at(ctx, "POST", "/tasks/%/restore", raw, size)) return handleRestoreTask(ctx, raw, err);
    if (at(ctx, "GET", "/tasks/%/comments", raw, size)) return handleListComments(ctx, raw, err);
    if (at(ctx, "POST", "/tasks/%/comments", raw, size)) return handleCreateComment(ctx, raw, err);
    if (at(ctx, "DELETE", "/comments/%", raw, size)) return handleDeleteComment(ctx, raw, err);
    if (at(ctx, "GET", "/search", NULL, 0)) return handleSearch(ctx, err);
    if (at(ctx, "GET", "/reports/workload", NULL, 0)) return handleWorkload(ctx, err);
    if (at(ctx, "GET", "/audit", NULL, 0)) return handleAudit(ctx, err);
    if (at(ctx, "GET", "/outbox", NULL, 0)) return handleOutbox(ctx, err);
    if (at(ctx, "POST", "/outbox/flush", NULL, 0)) return handleFlush(ctx, err);
    if (at(ctx, "GET", "/metrics", NULL, 0)) return handleMetrics(ctx, err);
    if (at(ctx, "GET", "/stats", NULL, 0)) return handleStats(ctx, err);
    return errNotFound(err);
}

/* ------------------------------------------------------------------ transport */

static const char *reasonOf(int status)
{
    switch (status) {
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 422: return "Unprocessable Entity";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    default: return "OK";
    }
}

/* The accept loop serves one connection at a time, so one flag is enough. */
static int keepAlive = 1;

static void sendResponse(SOCKET client, const Context *ctx)
{
    char header[1024];
    int length = sprintf(header,
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %d\r\n"
                         "X-Request-Id: %s\r\n",
                         ctx->status, reasonOf(ctx->status), ctx->length, ctx->requestId);
    if (ctx->etag >= 0) length += sprintf(header + length, "ETag: %d\r\n", ctx->etag);
    if (ctx->quotaRemaining >= 0) {
        length += sprintf(header + length, "X-Quota-Remaining: %d\r\n", ctx->quotaRemaining);
    }
    if (ctx->replayed) length += sprintf(header + length, "Idempotency-Replayed: true\r\n");
    length += sprintf(header + length, "Connection: %s\r\n\r\n",
                      keepAlive ? "keep-alive" : "close");
    send(client, header, length, 0);
    if (ctx->length > 0) send(client, ctx->out, ctx->length, 0);
}

static void makeRequestId(char *out)
{
    static unsigned long counter = 0;
    counter += 1;
    sprintf(out, "%04lx%04x%04x", counter & 0xffffUL, (unsigned)rand() & 0xffffU,
            (unsigned)rand() & 0xffffU);
}

static void writeLog(const Context *ctx, int duration, int auditSeq)
{
    char line[2048];
    const char *level = "info";
    int length = 0;
    if (ctx->status >= 500) level = "error";
    else if (ctx->status >= 400) level = "warn";
    length = sprintf(line, "{\"level\":\"%s\",\"requestId\":", level);
    length += writeJsonString(line + length, ctx->requestId);
    length += sprintf(line + length, ",\"method\":");
    length += writeJsonString(line + length, ctx->method);
    length += sprintf(line + length, ",\"path\":");
    length += writeJsonString(line + length, ctx->path);
    length += sprintf(line + length, ",\"status\":%d,\"durationMs\":%d,\"userId\":",
                      ctx->status, duration);
    if (ctx->userId == 0) length += sprintf(line + length, "null");
    else length += sprintf(line + length, "%d", ctx->userId);
    length += sprintf(line + length, ",\"quotaRemaining\":");
    if (ctx->quotaRemaining < 0) length += sprintf(line + length, "null");
    else length += sprintf(line + length, "%d", ctx->quotaRemaining);
    length += sprintf(line + length, ",\"auditSeq\":%d}\n", auditSeq);
    fwrite(line, 1, (size_t)length, stdout);
    fflush(stdout);
}

static void observe(SOCKET client, char *request)
{
    static Context ctx;
    AppError err;
    char label[TEXT_SIZE + 32];
    char *target = NULL;
    char *rest = NULL;
    char *space = NULL;
    char *marker = NULL;
    clock_t started = clock();
    int before = auditTotal;
    int duration = 0;
    copyText(ctx.method, sizeof ctx.method, "GET");
    copyText(ctx.path, sizeof ctx.path, "/");
    ctx.query = "";
    ctx.headers = "";
    ctx.body = "";
    ctx.requestId[0] = '\0';
    ctx.token[0] = '\0';
    ctx.pattern = "/{path}";
    ctx.sessionIndex = -1;
    ctx.userId = 0;
    ctx.quotaRemaining = -1;
    ctx.charged = 0;
    ctx.replayed = 0;
    ctx.etag = -1;
    ctx.status = 400;
    ctx.length = 0;
    resetError(&err);
    space = strchr(request, ' ');
    if (space != NULL) {
        *space = '\0';
        copyText(ctx.method, sizeof ctx.method, request);
        target = space + 1;
        space = strchr(target, ' ');
        if (space != NULL) {
            *space = '\0';
            rest = space + 1;
            marker = strstr(rest, "\r\n");
            ctx.headers = (marker == NULL) ? "" : marker + 2;
            marker = strstr(rest, "\r\n\r\n");
            ctx.body = (marker == NULL) ? "" : marker + 4;
            space = strchr(target, '?');
            if (space != NULL) {
                *space = '\0';
                ctx.query = space + 1;
            }
            copyText(ctx.path, sizeof ctx.path, target);
        }
    }
    if (!headerValue(ctx.headers, "X-Request-Id", ctx.requestId, sizeof ctx.requestId)
        || ctx.requestId[0] == '\0') {
        makeRequestId(ctx.requestId);
    }
    if (rest == NULL) errBadRequest(&err);
    else if (route(&ctx, &err)) resetError(&err);
    if (err.status != 0) writeError(&ctx, &err);
    keepResponse(&ctx);
    duration = (int)((clock() - started) * 1000 / CLOCKS_PER_SEC);
    if (duration < 0) duration = 0;
    routeLabel(label, sizeof label, ctx.method, ctx.pattern);
    countRequest(label, ctx.status);
    writeLog(&ctx, duration, auditTotal - before);
    sendResponse(client, &ctx);
}

/* Reads one header value out of the first limit bytes of the request. The scan
   stops at the empty line, so it never reads the body. */
static int requestHeader(const char *request, int limit, const char *name,
                         char *out, size_t outLimit)
{
    size_t nameLength = strlen(name);
    int line = 0;
    while (line + 1 < limit && !(request[line] == '\r' && request[line + 1] == '\n')) line += 1;
    line += 2;
    while (line < limit) {
        int end = line;
        while (end + 1 < limit && !(request[end] == '\r' && request[end + 1] == '\n')) end += 1;
        if (end == line) break;
        if ((size_t)(end - line) > nameLength && equalsIgnoreCase(request + line, name, nameLength)
            && request[line + nameLength] == ':') {
            int start = line + (int)nameLength + 1;
            size_t length = 0;
            while (start < end && (request[start] == ' ' || request[start] == '\t')) start += 1;
            length = (size_t)(end - start);
            if (length >= outLimit) length = outLimit - 1;
            memcpy(out, request + start, length);
            out[length] = '\0';
            return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int contentLengthOf(const char *request, int limit)
{
    char value[32];
    int length = 0;
    if (!requestHeader(request, limit, "Content-Length", value, sizeof value)) return 0;
    length = atoi(value);
    return length > 0 ? length : 0;
}

static int hasToken(const char *value, const char *token)
{
    size_t length = strlen(token);
    for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
        if (equalsIgnoreCase(cursor, token, length)) return 1;
    }
    return 0;
}

/* HTTP/1.1 keeps the connection open unless the client asks to close it. */
static int wantsKeepAlive(const char *request, int limit)
{
    char value[128];
    int line = 0;
    while (line + 1 < limit && !(request[line] == '\r' && request[line + 1] == '\n')) line += 1;
    if (requestHeader(request, limit, "Connection", value, sizeof value)) {
        if (hasToken(value, "close")) return 0;
        if (hasToken(value, "keep-alive")) return 1;
    }
    return line >= 8 && strncmp(request + line - 8, "HTTP/1.0", 8) != 0;
}

/* Takes one whole request out of the connection buffer and returns its length.
   Returns -1 when the connection ends. *held keeps the bytes that arrived
   after that request, which belong to the next one. */
static int receiveRequest(SOCKET client, char *buffer, int capacity, int *held)
{
    for (;;) {
        const char *marker = NULL;
        int received = 0;
        buffer[*held] = '\0';
        marker = strstr(buffer, "\r\n\r\n");
        if (marker != NULL) {
            int headerEnd = (int)(marker - buffer) + 4;
            int total = headerEnd + contentLengthOf(buffer, headerEnd);
            if (*held >= total) return total;
            if (total >= capacity) return -1;
        }
        if (*held >= capacity - 1) return -1;
        received = recv(client, buffer + *held, capacity - *held - 1, 0);
        if (received <= 0) return -1;
        *held += received;
    }
}

static void serveConnection(SOCKET client, char *buffer, int capacity)
{
    int held = 0;
    int served = 0;
    for (;;) {
        int length = receiveRequest(client, buffer, capacity, &held);
        char saved = '\0';
        if (length < 0) return;
        /* Cut the request off from the bytes of the next one. */
        saved = buffer[length];
        buffer[length] = '\0';
        keepAlive = wantsKeepAlive(buffer, length) && served + 1 < MAX_KEEP_ALIVE;
        observe(client, buffer);
        buffer[length] = saved;
        held -= length;
        memmove(buffer, buffer + length, (size_t)held);
        served += 1;
        if (!keepAlive) return;
    }
}

int main(void)
{
    WSADATA wsaData;
    struct sockaddr_in address;
    SOCKET server = INVALID_SOCKET;
    static char buffer[REQUEST_SIZE];
    int reuse = 1;
    int timeout = RECEIVE_TIMEOUT;
    int nodelay = 1;
    srand((unsigned)time(NULL));
    seed();
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) return 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(server, (struct sockaddr *)&address, sizeof address) != 0) return 1;
    if (listen(server, SOMAXCONN) != 0) return 1;
    for (;;) {
        SOCKET client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        /* The loop is single threaded, so an idle client must never block it. */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
        serveConnection(client, buffer, REQUEST_SIZE);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}
