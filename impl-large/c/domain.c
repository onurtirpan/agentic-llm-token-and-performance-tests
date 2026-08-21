/* Task Service, large tier — domain types, constants and pure rules. */

#include "domain.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const STATUS_NAMES[4] = {"todo", "in_progress", "done", "archived"};
const char *const ROLES[2] = {"admin", "user"};
const char *const GROUP_BYS[3] = {"assignee", "status", "project"};
const char *const PROJECT_SORTS[3] = {"id", "name", "taskCount"};
const char *const TASK_SORTS[5] = {"id", "title", "priority", "score", "status"};
const char *const USER_SORTS[3] = {"id", "username", "role"};
const char *const COMMENT_SORTS[2] = {"id", "authorId"};
const char *const SEQ_SORTS[1] = {"seq"};

static const int STATUS_BONUS[4] = {0, 3, 5, 0};
static const char *const TRANSITIONS[5][2] = {
    {"todo", "in_progress"}, {"todo", "archived"}, {"in_progress", "todo"},
    {"in_progress", "done"}, {"done", "archived"},
};

/* ------------------------------------------------------------------ pure rules */

void copyText(char *out, size_t limit, const char *text)
{
    size_t length = strlen(text);
    if (length >= limit) length = limit - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

int containsIgnoreCase(const char *haystack, const char *needle)
{
    size_t length = strlen(needle);
    if (length == 0) return 1;
    for (const char *cursor = haystack; *cursor != '\0'; cursor += 1) {
        size_t index = 0;
        while (index < length
               && tolower((unsigned char)cursor[index]) == tolower((unsigned char)needle[index])) {
            index += 1;
        }
        if (index == length) return 1;
    }
    return 0;
}

int parseWholeNumber(const char *raw, int *out)
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

int statusIndex(const char *status)
{
    for (int index = 0; index < 4; index += 1) {
        if (strcmp(status, STATUS_NAMES[index]) == 0) return index;
    }
    return -1;
}

int computeScore(int priority, const char *status)
{
    int baseScore = priority * 10;
    int index = statusIndex(status);
    return baseScore + (index < 0 ? 0 : STATUS_BONUS[index]);
}

int allowedTransition(const char *from, const char *to)
{
    for (int index = 0; index < 5; index += 1) {
        if (strcmp(from, TRANSITIONS[index][0]) == 0 && strcmp(to, TRANSITIONS[index][1]) == 0) {
            return 1;
        }
    }
    return 0;
}

int isMember(const char *value, const char *const *allowed, int allowedCount)
{
    for (int index = 0; index < allowedCount; index += 1) {
        if (strcmp(value, allowed[index]) == 0) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- error values */

static int makeError(AppError *err, int status, const char *code, const char *message)
{
    err->status = status;
    err->code = code;
    err->message = message;
    err->details.count = 0;
    return 0;
}

void resetError(AppError *err)
{
    makeError(err, 0, NULL, NULL);
}

int errBadRequest(AppError *err)
{
    return makeError(err, 400, "bad_request", "the request is malformed");
}

int errUnauthorized(AppError *err)
{
    return makeError(err, 401, "unauthorized", "authentication is required");
}

int errInvalidCredentials(AppError *err)
{
    return makeError(err, 401, "invalid_credentials", "the username or password is wrong");
}

int errForbidden(AppError *err)
{
    return makeError(err, 403, "forbidden", "you may not access this resource");
}

int errNotFound(AppError *err)
{
    return makeError(err, 404, "not_found", "the resource does not exist");
}

int errConflict(AppError *err)
{
    return makeError(err, 409, "conflict", "the resource already exists");
}

int errInvalidTransition(AppError *err)
{
    return makeError(err, 409, "invalid_transition", "the status change is not allowed");
}

int errPreconditionFailed(AppError *err)
{
    return makeError(err, 412, "precondition_failed", "the resource has changed");
}

int errPreconditionRequired(AppError *err)
{
    return makeError(err, 428, "precondition_required", "the If-Match header is required");
}

int errQuotaExceeded(AppError *err)
{
    return makeError(err, 429, "quota_exceeded", "the request quota is exhausted");
}

static int compareDetails(const void *left, const void *right)
{
    const Detail *a = left;
    const Detail *b = right;
    int result = strcmp(a->field, b->field);
    return result != 0 ? result : strcmp(a->message, b->message);
}

int errInvalid(AppError *err, Errors *errors)
{
    qsort(errors->items, (size_t)errors->count, sizeof errors->items[0], compareDetails);
    makeError(err, 422, "validation_failed", "the request body is not valid");
    err->details = *errors;
    return 0;
}

int errInvalidField(AppError *err, const char *field, const char *message)
{
    Errors errors;
    errors.count = 0;
    fail(&errors, field, message);
    return errInvalid(err, &errors);
}

/* ------------------------------------------------------------ pure validation */

void fail(Errors *errors, const char *field, const char *message)
{
    Detail *detail = NULL;
    if (errors->count >= MAX_DETAILS) return;
    detail = &errors->items[errors->count];
    copyText(detail->field, sizeof detail->field, field);
    copyText(detail->message, sizeof detail->message, message);
    errors->count += 1;
}

void checkString(const char *value, const char *fieldName, size_t maxLength, Errors *errors)
{
    char message[96];
    if (value[0] == '\0') {
        sprintf(message, "%s is required", fieldName);
        fail(errors, fieldName, message);
    } else if (strlen(value) > maxLength) {
        sprintf(message, "%s is too long", fieldName);
        fail(errors, fieldName, message);
    }
}

void checkPriority(int value, int isNull, Errors *errors)
{
    if (isNull || value < MIN_PRIORITY || value > MAX_PRIORITY) {
        fail(errors, "priority", "priority is out of range");
    }
}

void checkStatus(const char *value, int isString, Errors *errors)
{
    if (!isString || statusIndex(value) < 0) fail(errors, "status", "status is not valid");
}

void checkRole(const char *value, int isString, Errors *errors)
{
    if (!isString || !isMember(value, ROLES, COUNT(ROLES))) {
        fail(errors, "role", "role is not valid");
    }
}

void checkQuota(int value, int isInt, Errors *errors)
{
    if (!isInt || value < 0) fail(errors, "quota", "quota is out of range");
}

/* ------------------------------------------------------------------ JSON read */

const char *skipSpace(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') cursor += 1;
    return cursor;
}

const char *parseJsonString(const char *cursor, char *out, size_t limit)
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

const char *parseJsonValue(const char *cursor, JsonField *field)
{
    field->kind = JSON_OTHER;
    field->number = 0;
    field->text[0] = '\0';
    field->raw = cursor;
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
    if (*cursor == '{' || *cursor == '[') {
        field->kind = (*cursor == '{') ? JSON_OBJECT : JSON_ARRAY;
        return skipJsonNested(cursor);
    }
    return NULL;
}

const char *parseJsonObject(const char *cursor, JsonBody *body)
{
    body->count = 0;
    body->kind = JSON_OBJECT;
    cursor = skipSpace(cursor);
    if (*cursor != '{') return NULL;
    cursor = skipSpace(cursor + 1);
    if (*cursor == '}') return cursor + 1;
    for (;;) {
        JsonField *field = NULL;
        if (body->count >= MAX_FIELDS) return NULL;
        field = &body->fields[body->count];
        cursor = parseJsonString(cursor, field->key, sizeof field->key);
        if (cursor != NULL) {
            cursor = skipSpace(cursor);
            if (*cursor != ':') cursor = NULL;
        }
        if (cursor != NULL) cursor = parseJsonValue(skipSpace(cursor + 1), field);
        if (cursor == NULL) return NULL;
        body->count += 1;
        cursor = skipSpace(cursor);
        if (*cursor == ',') {
            cursor = skipSpace(cursor + 1);
            continue;
        }
        if (*cursor == '}') return cursor + 1;
        return NULL;
    }
}

static const char *parseJsonElement(const char *cursor, JsonBody *body)
{
    JsonField field;
    const char *end = NULL;
    body->count = 0;
    body->kind = JSON_OTHER;
    if (*cursor == '{') return parseJsonObject(cursor, body);
    end = parseJsonValue(cursor, &field);
    body->kind = field.kind;
    return end;
}

int parseJsonArray(const char *cursor, JsonBody *items, int capacity, int *count)
{
    *count = 0;
    cursor = skipSpace(cursor);
    if (*cursor != '[') return 0;
    cursor = skipSpace(cursor + 1);
    if (*cursor == ']') return 1;
    for (;;) {
        static JsonBody spill;
        JsonBody *slot = (*count < capacity) ? &items[*count] : &spill;
        cursor = parseJsonElement(cursor, slot);
        if (cursor == NULL) return 0;
        *count += 1;
        cursor = skipSpace(cursor);
        if (*cursor == ',') {
            cursor = skipSpace(cursor + 1);
            continue;
        }
        return *cursor == ']';
    }
}

const JsonField *findField(const JsonBody *body, const char *name)
{
    for (int index = 0; index < body->count; index += 1) {
        if (strcmp(body->fields[index].key, name) == 0) return &body->fields[index];
    }
    return NULL;
}

/* ----------------------------------------------------------------- JSON write */

int writeJsonString(char *out, const char *text)
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
