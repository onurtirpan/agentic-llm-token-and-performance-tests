/* Task Service, mid tier — raw winsock implementation. */

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_TITLE_LENGTH 80
#define MAX_NAME_LENGTH 60
#define MIN_PRIORITY 1
#define MAX_PRIORITY 5
#define DEFAULT_LIMIT 20
#define MAX_LIMIT 100
#define PORT 8080

#define MAX_USERS 3
#define MAX_PROJECTS 128
#define MAX_TASKS 512
#define MAX_SESSIONS 128
#define MAX_FIELDS 16
#define MAX_DETAILS 16
#define BUFFER_SIZE 65536
#define TEXT_SIZE 256
#define TOKEN_SIZE 40
#define SORT_SIZE 32
#define MAX_KEEP_ALIVE 1000
#define RECEIVE_TIMEOUT 5000

typedef struct {
    int id;
    char username[TEXT_SIZE];
    char password[TEXT_SIZE];
    char role[8];
} User;

typedef struct {
    char token[TOKEN_SIZE];
    int userId;
} Session;

typedef struct {
    int id;
    char name[TEXT_SIZE];
    int ownerId;
} Project;

typedef struct {
    int id;
    int projectId;
    char title[TEXT_SIZE];
    int priority;
    char status[16];
    int assigneeId;
    int score;
} Task;

typedef struct {
    char title[TEXT_SIZE];
    int priority;
    int assigneeId;
} TaskInput;

typedef struct {
    int status;
    const char *code;
    const char *message;
} AppError;

typedef struct {
    char field[32];
    char message[64];
} Detail;

typedef struct {
    Detail items[MAX_DETAILS];
    int count;
} Errors;

typedef enum { JSON_NULL, JSON_BOOL, JSON_INT, JSON_STRING, JSON_OTHER } JsonKind;

typedef struct {
    char key[32];
    JsonKind kind;
    long number;
    char text[TEXT_SIZE];
} JsonField;

typedef struct {
    JsonField fields[MAX_FIELDS];
    int count;
} JsonBody;

typedef struct {
    int value;
    int isNull;
} IntRef;

typedef struct {
    int limit;
    int offset;
    char sort[SORT_SIZE];
    char order[SORT_SIZE];
} Page;

typedef struct {
    char method[16];
    char path[TEXT_SIZE];
    const char *query;
    const char *headers;
    const char *body;
    char requestId[128];
    int userId;
    int sessionIndex;
    int status;
    int length;
    char out[BUFFER_SIZE];
} Context;

static const char *const STATUS_NAMES[] = {"todo", "in_progress", "done", "archived"};
static const int STATUS_BONUS[] = {0, 3, 5, 0};
static const char *const TRANSITIONS[][2] = {
    {"todo", "in_progress"}, {"todo", "archived"}, {"in_progress", "todo"},
    {"in_progress", "done"}, {"done", "archived"},
};
static const char *const PROJECT_SORTS[] = {"id", "name", "taskCount"};
static const char *const TASK_SORTS[] = {"id", "title", "priority", "score", "status"};

static User users[MAX_USERS] = {
    {1, "admin", "admin-secret", "admin"},
    {2, "alice", "alice-secret", "user"},
    {3, "bob", "bob-secret", "user"},
};
static Session sessions[MAX_SESSIONS];
static Project projects[MAX_PROJECTS];
static Task tasks[MAX_TASKS];
static int sessionTotal = 0;
static int projectTotal = 0;
static int taskTotal = 0;
static int nextProjectId = 1;
static int nextTaskId = 1;

static char sortField[SORT_SIZE];
static int sortDescending;

static void copyText(char *out, size_t limit, const char *text)
{
    size_t length = strlen(text);
    if (length >= limit) length = limit - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static int statusBonus(const char *status)
{
    for (int index = 0; index < 4; index += 1) {
        if (strcmp(status, STATUS_NAMES[index]) == 0) return STATUS_BONUS[index];
    }
    return -1;
}

static int computeScore(int priority, const char *status)
{
    int baseScore = priority * 10;
    return baseScore + statusBonus(status);
}

static int taskCount(int projectId)
{
    int total = 0;
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].projectId == projectId) total += 1;
    }
    return total;
}

static const User *findUser(int id)
{
    for (int index = 0; index < MAX_USERS; index += 1) {
        if (users[index].id == id) return &users[index];
    }
    return NULL;
}

static Project *findProject(int id)
{
    for (int index = 0; index < projectTotal; index += 1) {
        if (projects[index].id == id) return &projects[index];
    }
    return NULL;
}

static Task *findTask(int id)
{
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].id == id) return &tasks[index];
    }
    return NULL;
}

static int findSession(const char *token)
{
    for (int index = 0; index < sessionTotal; index += 1) {
        if (strcmp(sessions[index].token, token) == 0) return index;
    }
    return -1;
}

/* ------------------------------------------------------------------ JSON read */

static const char *skipSpace(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') cursor += 1;
    return cursor;
}

static const char *parseJsonString(const char *cursor, char *out, size_t limit)
{
    size_t length = 0;
    if (*cursor != '"') return NULL;
    cursor += 1;
    while (*cursor != '"') {
        char value = *cursor;
        if (value == '\0') return NULL;
        if (value == '\\') {
            cursor += 1;
            switch (*cursor) {
            case '"': value = '"'; break;
            case '\\': value = '\\'; break;
            case '/': value = '/'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: return NULL;
            }
        }
        if (length + 1 >= limit) return NULL;
        out[length] = value;
        length += 1;
        cursor += 1;
    }
    out[length] = '\0';
    return cursor + 1;
}

static const char *skipJsonNested(const char *cursor)
{
    int depth = 0;
    for (;;) {
        char value = *cursor;
        if (value == '\0') return NULL;
        if (value == '"') {
            cursor += 1;
            while (*cursor != '"') {
                if (*cursor == '\0') return NULL;
                if (*cursor == '\\' && cursor[1] != '\0') cursor += 1;
                cursor += 1;
            }
        } else if (value == '{' || value == '[') {
            depth += 1;
        } else if (value == '}' || value == ']') {
            depth -= 1;
            if (depth == 0) return cursor + 1;
        }
        cursor += 1;
    }
}

static const char *parseJsonValue(const char *cursor, JsonField *field)
{
    field->kind = JSON_OTHER;
    field->number = 0;
    field->text[0] = '\0';
    if (*cursor == '"') {
        field->kind = JSON_STRING;
        return parseJsonString(cursor, field->text, sizeof field->text);
    }
    if (strncmp(cursor, "null", 4) == 0) {
        field->kind = JSON_NULL;
        return cursor + 4;
    }
    if (strncmp(cursor, "true", 4) == 0) {
        field->kind = JSON_BOOL;
        field->number = 1;
        return cursor + 4;
    }
    if (strncmp(cursor, "false", 5) == 0) {
        field->kind = JSON_BOOL;
        return cursor + 5;
    }
    if (*cursor == '-' || isdigit((unsigned char)*cursor)) {
        char *end = NULL;
        field->number = strtol(cursor, &end, 10);
        if (end == cursor) return NULL;
        field->kind = JSON_INT;
        if (*end == '.' || *end == 'e' || *end == 'E') {
            strtod(cursor, &end);
            field->kind = JSON_OTHER;
        }
        return end;
    }
    if (*cursor == '{' || *cursor == '[') return skipJsonNested(cursor);
    return NULL;
}

static const JsonField *findField(const JsonBody *body, const char *name)
{
    for (int index = 0; index < body->count; index += 1) {
        if (strcmp(body->fields[index].key, name) == 0) return &body->fields[index];
    }
    return NULL;
}

/* ----------------------------------------------------------------- JSON write */

static int writeJsonString(char *out, const char *text)
{
    int length = 0;
    out[length] = '"';
    length += 1;
    for (const char *cursor = text; *cursor != '\0'; cursor += 1) {
        switch (*cursor) {
        case '"': length += sprintf(out + length, "\\\""); break;
        case '\\': length += sprintf(out + length, "\\\\"); break;
        case '\n': length += sprintf(out + length, "\\n"); break;
        case '\r': length += sprintf(out + length, "\\r"); break;
        case '\t': length += sprintf(out + length, "\\t"); break;
        default:
            out[length] = *cursor;
            length += 1;
            break;
        }
    }
    out[length] = '"';
    return length + 1;
}

static int serializeProject(char *out, const void *row)
{
    const Project *project = row;
    int length = sprintf(out, "{\"id\":%d,\"name\":", project->id);
    length += writeJsonString(out + length, project->name);
    length += sprintf(out + length, ",\"ownerId\":%d,\"taskCount\":%d}",
                      project->ownerId, taskCount(project->id));
    return length;
}

static int serializeTask(char *out, const void *row)
{
    const Task *task = row;
    int length = sprintf(out, "{\"id\":%d,\"projectId\":%d,\"title\":", task->id, task->projectId);
    length += writeJsonString(out + length, task->title);
    length += sprintf(out + length, ",\"priority\":%d,\"status\":\"%s\",\"assigneeId\":",
                      task->priority, task->status);
    if (task->assigneeId == 0) length += sprintf(out + length, "null");
    else length += sprintf(out + length, "%d", task->assigneeId);
    length += sprintf(out + length, ",\"score\":%d}", task->score);
    return length;
}

/* --------------------------------------------------------------- error bodies */

static void appError(Context *ctx, AppError error, const Errors *details)
{
    int length = sprintf(ctx->out, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"requestId\":",
                         error.code, error.message);
    length += writeJsonString(ctx->out + length, ctx->requestId);
    length += sprintf(ctx->out + length, ",\"details\":[");
    if (details != NULL) {
        for (int index = 0; index < details->count; index += 1) {
            if (index > 0) length += sprintf(ctx->out + length, ",");
            length += sprintf(ctx->out + length, "{\"field\":\"%s\",\"message\":\"%s\"}",
                              details->items[index].field, details->items[index].message);
        }
    }
    length += sprintf(ctx->out + length, "]}}");
    ctx->status = error.status;
    ctx->length = length;
}

static void badRequest(Context *ctx)
{
    appError(ctx, (AppError){400, "bad_request", "the request is malformed"}, NULL);
}

static void notFound(Context *ctx)
{
    appError(ctx, (AppError){404, "not_found", "the resource does not exist"}, NULL);
}

static void forbidden(Context *ctx)
{
    appError(ctx, (AppError){403, "forbidden", "you may not access this resource"}, NULL);
}

static void conflict(Context *ctx)
{
    appError(ctx, (AppError){409, "conflict", "the resource already exists"}, NULL);
}

static int compareDetails(const void *left, const void *right)
{
    const Detail *a = left;
    const Detail *b = right;
    int result = strcmp(a->field, b->field);
    return result != 0 ? result : strcmp(a->message, b->message);
}

static void invalid(Context *ctx, Errors *errors)
{
    qsort(errors->items, (size_t)errors->count, sizeof errors->items[0], compareDetails);
    appError(ctx, (AppError){422, "validation_failed", "the request body is not valid"}, errors);
}

static void fail(Errors *errors, const char *field, const char *message)
{
    Detail *detail = NULL;
    if (errors->count >= MAX_DETAILS) return;
    detail = &errors->items[errors->count];
    copyText(detail->field, sizeof detail->field, field);
    copyText(detail->message, sizeof detail->message, message);
    errors->count += 1;
}

/* ---------------------------------------------------------------- input reads */

static int readBody(Context *ctx, JsonBody *body)
{
    const char *cursor = skipSpace(ctx->body);
    body->count = 0;
    if (*cursor == '\0') return 1;
    if (*cursor != '{') {
        badRequest(ctx);
        return 0;
    }
    cursor = skipSpace(cursor + 1);
    if (*cursor == '}') return 1;
    for (;;) {
        JsonField *field = NULL;
        if (body->count >= MAX_FIELDS) {
            badRequest(ctx);
            return 0;
        }
        field = &body->fields[body->count];
        cursor = parseJsonString(cursor, field->key, sizeof field->key);
        if (cursor != NULL) {
            cursor = skipSpace(cursor);
            if (*cursor != ':') cursor = NULL;
        }
        if (cursor != NULL) cursor = parseJsonValue(skipSpace(cursor + 1), field);
        if (cursor == NULL) {
            badRequest(ctx);
            return 0;
        }
        body->count += 1;
        cursor = skipSpace(cursor);
        if (*cursor == ',') {
            cursor = skipSpace(cursor + 1);
            continue;
        }
        if (*cursor == '}') return 1;
        badRequest(ctx);
        return 0;
    }
}

static int readInt(const JsonBody *body, const char *name, int def, int defNull, IntRef *out)
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
    return 0;
}

static int readString(const JsonBody *body, const char *name, Errors *errors,
                      size_t maxLength, int required, char *out, size_t limit)
{
    const JsonField *field = findField(body, name);
    char message[64];
    out[0] = '\0';
    if (field != NULL) {
        if (field->kind != JSON_STRING) return 0;
        copyText(out, limit, field->text);
    }
    if (out[0] == '\0') {
        if (required) {
            sprintf(message, "%s is required", name);
            fail(errors, name, message);
        }
    } else if (strlen(out) > maxLength) {
        sprintf(message, "%s is too long", name);
        fail(errors, name, message);
    }
    return 1;
}

static int readPriority(const JsonBody *body, Errors *errors, int *out)
{
    IntRef ref;
    if (!readInt(body, "priority", 0, 0, &ref)) return 0;
    if (ref.isNull || ref.value < MIN_PRIORITY || ref.value > MAX_PRIORITY) {
        fail(errors, "priority", "priority is out of range");
    }
    *out = ref.isNull ? 0 : ref.value;
    return 1;
}

static int readUserRef(const JsonBody *body, const char *name, Errors *errors,
                       int def, int defNull, IntRef *out)
{
    char message[64];
    if (!readInt(body, name, def, defNull, out)) return 0;
    if (!out->isNull && findUser(out->value) == NULL) {
        sprintf(message, "%s is not a known user", name);
        fail(errors, name, message);
    }
    return 1;
}

static int readTaskInput(Context *ctx, const JsonBody *body, Errors *errors, TaskInput *input)
{
    IntRef assignee;
    if (!readString(body, "title", errors, MAX_TITLE_LENGTH, 1, input->title, sizeof input->title)
        || !readPriority(body, errors, &input->priority)
        || !readUserRef(body, "assigneeId", errors, 0, 1, &assignee)) {
        badRequest(ctx);
        return 0;
    }
    input->assigneeId = assignee.isNull ? 0 : assignee.value;
    return 1;
}

static int parseId(Context *ctx, const char *raw, int *out)
{
    char *end = NULL;
    long value = 0;
    if (raw[0] == '\0') {
        badRequest(ctx);
        return 0;
    }
    value = strtol(raw, &end, 10);
    if (*end != '\0') {
        badRequest(ctx);
        return 0;
    }
    *out = (int)value;
    return 1;
}

static int queryValue(const char *query, const char *name, char *out, size_t limit)
{
    size_t nameLength = strlen(name);
    const char *cursor = query;
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

static int parseQueryInt(const char *raw, int *out)
{
    const char *cursor = raw;
    if (*cursor == '-') cursor += 1;
    if (*cursor == '\0') return 0;
    while (*cursor != '\0') {
        if (!isdigit((unsigned char)*cursor)) return 0;
        cursor += 1;
    }
    *out = (int)strtol(raw, NULL, 10);
    return 1;
}

static int readPage(Context *ctx, const char *const *allowed, int allowedCount, Page *page)
{
    Errors errors;
    char raw[64];
    int found = 0;
    errors.count = 0;
    page->limit = DEFAULT_LIMIT;
    page->offset = 0;
    copyText(page->sort, sizeof page->sort, "id");
    copyText(page->order, sizeof page->order, "asc");
    if (queryValue(ctx->query, "limit", raw, sizeof raw)) {
        if (!parseQueryInt(raw, &page->limit)) page->limit = -1;
        if (page->limit < 1 || page->limit > MAX_LIMIT) {
            fail(&errors, "limit", "limit is out of range");
        }
    }
    if (queryValue(ctx->query, "offset", raw, sizeof raw)) {
        if (!parseQueryInt(raw, &page->offset)) page->offset = -1;
        if (page->offset < 0) fail(&errors, "offset", "offset is out of range");
    }
    if (queryValue(ctx->query, "sort", raw, sizeof raw)) {
        copyText(page->sort, sizeof page->sort, raw);
    }
    if (queryValue(ctx->query, "order", raw, sizeof raw)) {
        copyText(page->order, sizeof page->order, raw);
    }
    for (int index = 0; index < allowedCount; index += 1) {
        if (strcmp(page->sort, allowed[index]) == 0) found = 1;
    }
    if (!found) fail(&errors, "sort", "sort is not a valid field");
    if (strcmp(page->order, "asc") != 0 && strcmp(page->order, "desc") != 0) {
        fail(&errors, "order", "order must be asc or desc");
    }
    if (errors.count > 0) {
        invalid(ctx, &errors);
        return 0;
    }
    return 1;
}

static int compareProjects(const void *left, const void *right)
{
    const Project *a = *(const Project *const *)left;
    const Project *b = *(const Project *const *)right;
    int result = 0;
    if (strcmp(sortField, "name") == 0) result = strcmp(a->name, b->name);
    else if (strcmp(sortField, "taskCount") == 0) result = taskCount(a->id) - taskCount(b->id);
    else result = a->id - b->id;
    if (sortDescending) result = -result;
    return result != 0 ? result : a->id - b->id;
}

static int compareTasks(const void *left, const void *right)
{
    const Task *a = *(const Task *const *)left;
    const Task *b = *(const Task *const *)right;
    int result = 0;
    if (strcmp(sortField, "title") == 0) result = strcmp(a->title, b->title);
    else if (strcmp(sortField, "priority") == 0) result = a->priority - b->priority;
    else if (strcmp(sortField, "score") == 0) result = a->score - b->score;
    else if (strcmp(sortField, "status") == 0) result = strcmp(a->status, b->status);
    else result = a->id - b->id;
    if (sortDescending) result = -result;
    return result != 0 ? result : a->id - b->id;
}

static void paginate(Context *ctx, void **rows, int total, const Page *page,
                     int (*compare)(const void *, const void *),
                     int (*serialize)(char *, const void *))
{
    int length = 0;
    int shown = 0;
    copyText(sortField, sizeof sortField, page->sort);
    sortDescending = strcmp(page->order, "desc") == 0;
    qsort(rows, (size_t)total, sizeof rows[0], compare);
    length = sprintf(ctx->out, "{\"items\":[");
    for (int index = page->offset; index < total && shown < page->limit; index += 1) {
        if (shown > 0) length += sprintf(ctx->out + length, ",");
        length += serialize(ctx->out + length, rows[index]);
        shown += 1;
    }
    length += sprintf(ctx->out + length, "],\"total\":%d,\"limit\":%d,\"offset\":%d}",
                      total, page->limit, page->offset);
    ctx->status = 200;
    ctx->length = length;
}

/* --------------------------------------------------------------------- guards */

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

static const User *authenticate(Context *ctx)
{
    char header[TEXT_SIZE];
    int index = -1;
    if (headerValue(ctx->headers, "Authorization", header, sizeof header)
        && strncmp(header, "Bearer ", 7) == 0) {
        index = findSession(header + 7);
    }
    if (index < 0) {
        appError(ctx, (AppError){401, "unauthorized", "authentication is required"}, NULL);
        return NULL;
    }
    ctx->sessionIndex = index;
    ctx->userId = sessions[index].userId;
    return findUser(sessions[index].userId);
}

static int requireAdmin(Context *ctx, const User *user)
{
    if (strcmp(user->role, "admin") != 0) {
        forbidden(ctx);
        return 0;
    }
    return 1;
}

static Project *reachableProject(Context *ctx, int projectId, const User *user)
{
    Project *project = findProject(projectId);
    if (project == NULL) {
        notFound(ctx);
        return NULL;
    }
    if (strcmp(user->role, "admin") != 0 && project->ownerId != user->id) {
        forbidden(ctx);
        return NULL;
    }
    return project;
}

static Task *reachableTask(Context *ctx, int taskId, const User *user)
{
    Task *task = findTask(taskId);
    if (task == NULL) {
        notFound(ctx);
        return NULL;
    }
    if (reachableProject(ctx, task->projectId, user) == NULL) return NULL;
    return task;
}

/* ------------------------------------------------------------------- handlers */

static void getHealth(Context *ctx)
{
    ctx->status = 200;
    ctx->length = sprintf(ctx->out, "{\"status\":\"ok\",\"projects\":%d,\"tasks\":%d}",
                          projectTotal, taskTotal);
}

static void makeToken(char *out)
{
    for (int index = 0; index < TOKEN_SIZE - 8; index += 1) {
        out[index] = "0123456789abcdef"[rand() % 16];
    }
    out[TOKEN_SIZE - 8] = '\0';
}

static void login(Context *ctx)
{
    JsonBody body;
    Errors errors;
    char username[TEXT_SIZE];
    char password[TEXT_SIZE];
    const User *user = NULL;
    Session *session = NULL;
    if (!readBody(ctx, &body)) return;
    errors.count = 0;
    if (!readString(&body, "username", &errors, MAX_NAME_LENGTH, 1, username, sizeof username)
        || !readString(&body, "password", &errors, MAX_NAME_LENGTH, 1, password, sizeof password)) {
        badRequest(ctx);
        return;
    }
    if (errors.count > 0) {
        invalid(ctx, &errors);
        return;
    }
    for (int index = 0; index < MAX_USERS; index += 1) {
        if (strcmp(users[index].username, username) == 0
            && strcmp(users[index].password, password) == 0) {
            user = &users[index];
            break;
        }
    }
    if (user == NULL) {
        appError(ctx, (AppError){401, "invalid_credentials", "the username or password is wrong"},
                 NULL);
        return;
    }
    if (sessionTotal >= MAX_SESSIONS) {
        conflict(ctx);
        return;
    }
    session = &sessions[sessionTotal];
    makeToken(session->token);
    session->userId = user->id;
    sessionTotal += 1;
    ctx->status = 200;
    ctx->length = sprintf(ctx->out, "{\"token\":\"%s\",\"userId\":%d,\"role\":\"%s\"}",
                          session->token, user->id, user->role);
}

static void logout(Context *ctx)
{
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    memmove(&sessions[ctx->sessionIndex], &sessions[ctx->sessionIndex + 1],
            (size_t)(sessionTotal - ctx->sessionIndex - 1) * sizeof(Session));
    sessionTotal -= 1;
    ctx->status = 204;
    ctx->length = 0;
}

static void getMe(Context *ctx)
{
    const User *user = authenticate(ctx);
    int length = 0;
    if (user == NULL) return;
    length = sprintf(ctx->out, "{\"userId\":%d,\"username\":", user->id);
    length += writeJsonString(ctx->out + length, user->username);
    length += sprintf(ctx->out + length, ",\"role\":\"%s\"}", user->role);
    ctx->status = 200;
    ctx->length = length;
}

static void listProjects(Context *ctx)
{
    void *rows[MAX_PROJECTS];
    Page page;
    const User *user = authenticate(ctx);
    int total = 0;
    if (user == NULL) return;
    if (!readPage(ctx, PROJECT_SORTS, 3, &page)) return;
    for (int index = 0; index < projectTotal; index += 1) {
        if (strcmp(user->role, "admin") == 0 || projects[index].ownerId == user->id) {
            rows[total] = &projects[index];
            total += 1;
        }
    }
    paginate(ctx, rows, total, &page, compareProjects, serializeProject);
}

static void createProject(Context *ctx)
{
    JsonBody body;
    Errors errors;
    char name[TEXT_SIZE];
    IntRef owner;
    Project *project = NULL;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!requireAdmin(ctx, user)) return;
    if (!readBody(ctx, &body)) return;
    errors.count = 0;
    if (!readString(&body, "name", &errors, MAX_NAME_LENGTH, 1, name, sizeof name)
        || !readUserRef(&body, "ownerId", &errors, user->id, 0, &owner)) {
        badRequest(ctx);
        return;
    }
    if (errors.count > 0) {
        invalid(ctx, &errors);
        return;
    }
    for (int index = 0; index < projectTotal; index += 1) {
        if (projects[index].ownerId == owner.value && strcmp(projects[index].name, name) == 0) {
            conflict(ctx);
            return;
        }
    }
    if (projectTotal >= MAX_PROJECTS) {
        conflict(ctx);
        return;
    }
    project = &projects[projectTotal];
    project->id = nextProjectId;
    copyText(project->name, sizeof project->name, name);
    project->ownerId = owner.value;
    projectTotal += 1;
    nextProjectId += 1;
    ctx->status = 201;
    ctx->length = serializeProject(ctx->out, project);
}

static void getProject(Context *ctx, const char *raw)
{
    const Project *project = NULL;
    int id = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    project = reachableProject(ctx, id, user);
    if (project == NULL) return;
    ctx->status = 200;
    ctx->length = serializeProject(ctx->out, project);
}

static void updateProject(Context *ctx, const char *raw)
{
    JsonBody body;
    Errors errors;
    char name[TEXT_SIZE];
    Project *project = NULL;
    int id = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!requireAdmin(ctx, user)) return;
    if (!parseId(ctx, raw, &id)) return;
    project = reachableProject(ctx, id, user);
    if (project == NULL) return;
    if (!readBody(ctx, &body)) return;
    if (findField(&body, "name") != NULL) {
        errors.count = 0;
        if (!readString(&body, "name", &errors, MAX_NAME_LENGTH, 1, name, sizeof name)) {
            badRequest(ctx);
            return;
        }
        if (errors.count > 0) {
            invalid(ctx, &errors);
            return;
        }
        for (int index = 0; index < projectTotal; index += 1) {
            if (projects[index].ownerId == project->ownerId && projects[index].id != project->id
                && strcmp(projects[index].name, name) == 0) {
                conflict(ctx);
                return;
            }
        }
        copyText(project->name, sizeof project->name, name);
    }
    ctx->status = 200;
    ctx->length = serializeProject(ctx->out, project);
}

static void deleteProject(Context *ctx, const char *raw)
{
    Project *project = NULL;
    int id = 0;
    int index = 0;
    int slot = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!requireAdmin(ctx, user)) return;
    if (!parseId(ctx, raw, &id)) return;
    project = reachableProject(ctx, id, user);
    if (project == NULL) return;
    while (index < taskTotal) {
        if (tasks[index].projectId == project->id) {
            memmove(&tasks[index], &tasks[index + 1],
                    (size_t)(taskTotal - index - 1) * sizeof(Task));
            taskTotal -= 1;
        } else {
            index += 1;
        }
    }
    slot = (int)(project - projects);
    memmove(&projects[slot], &projects[slot + 1],
            (size_t)(projectTotal - slot - 1) * sizeof(Project));
    projectTotal -= 1;
    ctx->status = 204;
    ctx->length = 0;
}

static void listTasks(Context *ctx, const char *raw)
{
    void *rows[MAX_TASKS];
    Page page;
    const Project *project = NULL;
    int id = 0;
    int total = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    project = reachableProject(ctx, id, user);
    if (project == NULL) return;
    if (!readPage(ctx, TASK_SORTS, 5, &page)) return;
    for (int index = 0; index < taskTotal; index += 1) {
        if (tasks[index].projectId == project->id) {
            rows[total] = &tasks[index];
            total += 1;
        }
    }
    paginate(ctx, rows, total, &page, compareTasks, serializeTask);
}

static void createTask(Context *ctx, const char *raw)
{
    JsonBody body;
    Errors errors;
    TaskInput input;
    const Project *project = NULL;
    Task *task = NULL;
    int id = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    project = reachableProject(ctx, id, user);
    if (project == NULL) return;
    if (!readBody(ctx, &body)) return;
    errors.count = 0;
    if (!readTaskInput(ctx, &body, &errors, &input)) return;
    if (errors.count > 0) {
        invalid(ctx, &errors);
        return;
    }
    if (taskTotal >= MAX_TASKS) {
        conflict(ctx);
        return;
    }
    task = &tasks[taskTotal];
    task->id = nextTaskId;
    task->projectId = project->id;
    copyText(task->title, sizeof task->title, input.title);
    task->priority = input.priority;
    copyText(task->status, sizeof task->status, "todo");
    task->assigneeId = input.assigneeId;
    task->score = computeScore(input.priority, "todo");
    taskTotal += 1;
    nextTaskId += 1;
    ctx->status = 201;
    ctx->length = serializeTask(ctx->out, task);
}

static void getTask(Context *ctx, const char *raw)
{
    const Task *task = NULL;
    int id = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    task = reachableTask(ctx, id, user);
    if (task == NULL) return;
    ctx->status = 200;
    ctx->length = serializeTask(ctx->out, task);
}

static void replaceTask(Context *ctx, const char *raw)
{
    JsonBody body;
    Errors errors;
    TaskInput input;
    Task *task = NULL;
    int id = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    task = reachableTask(ctx, id, user);
    if (task == NULL) return;
    if (!readBody(ctx, &body)) return;
    errors.count = 0;
    if (!readTaskInput(ctx, &body, &errors, &input)) return;
    if (errors.count > 0) {
        invalid(ctx, &errors);
        return;
    }
    copyText(task->title, sizeof task->title, input.title);
    task->priority = input.priority;
    task->assigneeId = input.assigneeId;
    task->score = computeScore(input.priority, task->status);
    ctx->status = 200;
    ctx->length = serializeTask(ctx->out, task);
}

static void updateStatus(Context *ctx, const char *raw)
{
    JsonBody body;
    Errors errors;
    const JsonField *field = NULL;
    Task *task = NULL;
    int id = 0;
    int allowed = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    task = reachableTask(ctx, id, user);
    if (task == NULL) return;
    if (!readBody(ctx, &body)) return;
    field = findField(&body, "status");
    if (field == NULL || field->kind != JSON_STRING || statusBonus(field->text) < 0) {
        errors.count = 0;
        fail(&errors, "status", "status is not valid");
        invalid(ctx, &errors);
        return;
    }
    for (int index = 0; index < 5; index += 1) {
        if (strcmp(task->status, TRANSITIONS[index][0]) == 0
            && strcmp(field->text, TRANSITIONS[index][1]) == 0) {
            allowed = 1;
        }
    }
    if (!allowed) {
        appError(ctx, (AppError){409, "invalid_transition", "the status change is not allowed"},
                 NULL);
        return;
    }
    copyText(task->status, sizeof task->status, field->text);
    task->score = computeScore(task->priority, task->status);
    ctx->status = 200;
    ctx->length = serializeTask(ctx->out, task);
}

static void deleteTask(Context *ctx, const char *raw)
{
    Task *task = NULL;
    int id = 0;
    int slot = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!parseId(ctx, raw, &id)) return;
    task = reachableTask(ctx, id, user);
    if (task == NULL) return;
    slot = (int)(task - tasks);
    memmove(&tasks[slot], &tasks[slot + 1], (size_t)(taskTotal - slot - 1) * sizeof(Task));
    taskTotal -= 1;
    ctx->status = 204;
    ctx->length = 0;
}

static void getStats(Context *ctx)
{
    const Project *best = NULL;
    double avgScore = 0.0;
    int byStatus[4] = {0, 0, 0, 0};
    int sumScore = 0;
    int length = 0;
    const User *user = authenticate(ctx);
    if (user == NULL) return;
    if (!requireAdmin(ctx, user)) return;
    for (int index = 0; index < taskTotal; index += 1) {
        for (int slot = 0; slot < 4; slot += 1) {
            if (strcmp(tasks[index].status, STATUS_NAMES[slot]) == 0) byStatus[slot] += 1;
        }
        sumScore += tasks[index].score;
    }
    if (taskTotal > 0) avgScore = round((double)sumScore / taskTotal * 100.0) / 100.0;
    for (int index = 0; index < projectTotal; index += 1) {
        if (best == NULL || taskCount(projects[index].id) > taskCount(best->id)) {
            best = &projects[index];
        }
    }
    length = sprintf(ctx->out,
                     "{\"projects\":%d,\"tasks\":%d,\"users\":%d,\"sessions\":%d,"
                     "\"byStatus\":{\"todo\":%d,\"in_progress\":%d,\"done\":%d,\"archived\":%d},"
                     "\"avgScore\":%g,\"topProjectName\":",
                     projectTotal, taskTotal, MAX_USERS, sessionTotal,
                     byStatus[0], byStatus[1], byStatus[2], byStatus[3], avgScore);
    if (best == NULL) length += sprintf(ctx->out + length, "null");
    else length += writeJsonString(ctx->out + length, best->name);
    length += sprintf(ctx->out + length, "}");
    ctx->status = 200;
    ctx->length = length;
}

/* -------------------------------------------------------------------- routing */

static int matchPath(const char *path, const char *pattern, char *out, size_t limit)
{
    size_t length = 0;
    out[0] = '\0';
    while (*pattern != '\0') {
        if (*pattern == '%') {
            while (*path != '\0' && *path != '/') {
                if (length + 1 >= limit) return 0;
                out[length] = *path;
                length += 1;
                path += 1;
            }
            out[length] = '\0';
            pattern += 1;
            continue;
        }
        if (*path != *pattern) return 0;
        path += 1;
        pattern += 1;
    }
    return *path == '\0';
}

static void handleRequest(Context *ctx)
{
    char raw[64];
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/health") == 0) {
        getHealth(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/auth/login") == 0) {
        login(ctx);
        return;
    }
    if (strcmp(ctx->method, "POST") == 0 && strcmp(ctx->path, "/auth/logout") == 0) {
        logout(ctx);
        return;
    }
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/me") == 0) {
        getMe(ctx);
        return;
    }
    if (strcmp(ctx->method, "GET") == 0 && strcmp(ctx->path, "/stats") == 0) {
        getStats(ctx);
        return;
    }
    if (strcmp(ctx->path, "/projects") == 0) {
        if (strcmp(ctx->method, "GET") == 0) {
            listProjects(ctx);
            return;
        }
        if (strcmp(ctx->method, "POST") == 0) {
            createProject(ctx);
            return;
        }
    }
    if (matchPath(ctx->path, "/projects/%", raw, sizeof raw)) {
        if (strcmp(ctx->method, "GET") == 0) {
            getProject(ctx, raw);
            return;
        }
        if (strcmp(ctx->method, "PATCH") == 0) {
            updateProject(ctx, raw);
            return;
        }
        if (strcmp(ctx->method, "DELETE") == 0) {
            deleteProject(ctx, raw);
            return;
        }
    }
    if (matchPath(ctx->path, "/projects/%/tasks", raw, sizeof raw)) {
        if (strcmp(ctx->method, "GET") == 0) {
            listTasks(ctx, raw);
            return;
        }
        if (strcmp(ctx->method, "POST") == 0) {
            createTask(ctx, raw);
            return;
        }
    }
    if (matchPath(ctx->path, "/tasks/%", raw, sizeof raw)) {
        if (strcmp(ctx->method, "GET") == 0) {
            getTask(ctx, raw);
            return;
        }
        if (strcmp(ctx->method, "PUT") == 0) {
            replaceTask(ctx, raw);
            return;
        }
        if (strcmp(ctx->method, "DELETE") == 0) {
            deleteTask(ctx, raw);
            return;
        }
    }
    if (matchPath(ctx->path, "/tasks/%/status", raw, sizeof raw)
        && strcmp(ctx->method, "PATCH") == 0) {
        updateStatus(ctx, raw);
        return;
    }
    notFound(ctx);
}

/* ----------------------------------------------------------------- transport */

/* The accept loop serves one connection at a time, so one flag is enough. */
static int keepAlive = 1;

static void sendResponse(SOCKET client, int status, const char *requestId,
                         const char *body, int length)
{
    const char *reason = "OK";
    char header[512];
    int headerLength = 0;
    switch (status) {
    case 201: reason = "Created"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 401: reason = "Unauthorized"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 409: reason = "Conflict"; break;
    case 422: reason = "Unprocessable Entity"; break;
    default: break;
    }
    headerLength = sprintf(header,
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "X-Request-Id: %s\r\n"
                           "Connection: %s\r\n\r\n",
                           status, reason, length, requestId,
                           keepAlive ? "keep-alive" : "close");
    send(client, header, headerLength, 0);
    if (length > 0) send(client, body, length, 0);
}

static void makeRequestId(char *out)
{
    static unsigned long counter = 0;
    counter += 1;
    sprintf(out, "%04lx%04x%04x", counter & 0xffffUL, (unsigned)rand() & 0xffffU,
            (unsigned)rand() & 0xffffU);
}

static void observe(SOCKET client, char *request)
{
    static Context ctx;
    const char *level = "info";
    char line[2048];
    char *target = NULL;
    char *rest = NULL;
    char *space = NULL;
    char *marker = NULL;
    clock_t started = clock();
    int duration = 0;
    int length = 0;
    copyText(ctx.method, sizeof ctx.method, "GET");
    copyText(ctx.path, sizeof ctx.path, "/");
    ctx.query = "";
    ctx.headers = "";
    ctx.body = "";
    ctx.requestId[0] = '\0';
    ctx.userId = 0;
    ctx.sessionIndex = -1;
    ctx.status = 400;
    ctx.length = 0;
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
    if (rest == NULL) badRequest(&ctx);
    else handleRequest(&ctx);
    duration = (int)((clock() - started) * 1000 / CLOCKS_PER_SEC);
    if (duration < 0) duration = 0;
    if (ctx.status >= 500) level = "error";
    else if (ctx.status >= 400) level = "warn";
    length = sprintf(line, "{\"level\":\"%s\",\"requestId\":", level);
    length += writeJsonString(line + length, ctx.requestId);
    length += sprintf(line + length, ",\"method\":");
    length += writeJsonString(line + length, ctx.method);
    length += sprintf(line + length, ",\"path\":");
    length += writeJsonString(line + length, ctx.path);
    length += sprintf(line + length, ",\"status\":%d,\"durationMs\":%d,\"userId\":",
                      ctx.status, duration);
    if (ctx.userId == 0) length += sprintf(line + length, "null");
    else length += sprintf(line + length, "%d", ctx.userId);
    length += sprintf(line + length, "}\n");
    fwrite(line, 1, (size_t)length, stdout);
    fflush(stdout);
    sendResponse(client, ctx.status, ctx.requestId, ctx.out, ctx.length);
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
    static char buffer[BUFFER_SIZE];
    int reuse = 1;
    int timeout = RECEIVE_TIMEOUT;
    int nodelay = 1;
    srand((unsigned)time(NULL));
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
        serveConnection(client, buffer, BUFFER_SIZE);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}
