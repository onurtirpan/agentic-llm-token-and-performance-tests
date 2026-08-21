/* Task Service — raw winsock implementation. */

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TITLE_LENGTH 80
#define MIN_PRIORITY 1
#define MAX_PRIORITY 5
#define PORT 8080
#define MAX_TASKS 1024
#define BUFFER_SIZE 65536
#define TITLE_SIZE 512
#define MAX_KEEP_ALIVE 1000
#define RECEIVE_TIMEOUT 5000

typedef struct {
    int id;
    char title[TITLE_SIZE];
    int priority;
    int done;
    int score;
} Task;

typedef struct {
    char title[TITLE_SIZE];
    int priority;
    int done;
} TaskInput;

static Task tasks[MAX_TASKS];
static int taskCount = 0;
static int nextId = 1;

static int computeScore(int priority, int done)
{
    int baseScore = priority * 10;
    return done ? baseScore : baseScore + 5;
}

static const char *validate(const char *title, int priority)
{
    if (title[0] == '\0') return "title is required";
    if (strlen(title) > MAX_TITLE_LENGTH) return "title is too long";
    if (priority < MIN_PRIORITY || priority > MAX_PRIORITY) return "priority is out of range";
    return NULL;
}

static Task *findTask(int id)
{
    for (int index = 0; index < taskCount; index += 1) {
        if (tasks[index].id == id) return &tasks[index];
    }
    return NULL;
}

static int parseId(const char *raw, int *out)
{
    char *end = NULL;
    long value = 0;
    if (raw[0] == '\0') return 0;
    value = strtol(raw, &end, 10);
    if (*end != '\0') return 0;
    *out = (int)value;
    return 1;
}

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

static int readInput(const char *body, TaskInput *input)
{
    const char *cursor = skipSpace(body);
    input->title[0] = '\0';
    input->priority = 0;
    input->done = 0;
    if (*cursor != '{') return 0;
    cursor = skipSpace(cursor + 1);
    if (*cursor == '}') return 1;
    for (;;) {
        char key[64];
        cursor = parseJsonString(cursor, key, sizeof key);
        if (cursor == NULL) return 0;
        cursor = skipSpace(cursor);
        if (*cursor != ':') return 0;
        cursor = skipSpace(cursor + 1);
        if (strcmp(key, "title") == 0) {
            cursor = parseJsonString(cursor, input->title, sizeof input->title);
            if (cursor == NULL) return 0;
        } else if (strcmp(key, "priority") == 0) {
            char *end = NULL;
            long value = strtol(cursor, &end, 10);
            if (end == cursor) return 0;
            input->priority = (int)value;
            cursor = end;
        } else if (strcmp(key, "done") == 0) {
            if (strncmp(cursor, "true", 4) == 0) {
                input->done = 1;
                cursor += 4;
            } else if (strncmp(cursor, "false", 5) == 0) {
                input->done = 0;
                cursor += 5;
            } else {
                return 0;
            }
        } else {
            return 0;
        }
        cursor = skipSpace(cursor);
        if (*cursor == ',') {
            cursor = skipSpace(cursor + 1);
            continue;
        }
        if (*cursor == '}') return 1;
        return 0;
    }
}

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

static int writeTask(char *out, const Task *task)
{
    int length = sprintf(out, "{\"id\":%d,\"title\":", task->id);
    length += writeJsonString(out + length, task->title);
    length += sprintf(out + length, ",\"priority\":%d,\"done\":%s,\"score\":%d}",
                      task->priority, task->done ? "true" : "false", task->score);
    return length;
}

/* The accept loop serves one connection at a time, so one flag is enough. */
static int keepAlive = 1;

static void sendResponse(SOCKET client, int status, const char *body, int length)
{
    const char *reason = "OK";
    char header[256];
    int headerLength = 0;
    if (status == 201) reason = "Created";
    else if (status == 204) reason = "No Content";
    else if (status == 400) reason = "Bad Request";
    else if (status == 404) reason = "Not Found";
    headerLength = sprintf(header,
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: %s\r\n\r\n",
                           status, reason, length, keepAlive ? "keep-alive" : "close");
    send(client, header, headerLength, 0);
    if (length > 0) send(client, body, length, 0);
}

static void fail(SOCKET client, int status, const char *message)
{
    char body[256];
    int length = sprintf(body, "{\"error\":\"%s\"}", message);
    sendResponse(client, status, body, length);
}

static int compareTasks(const void *left, const void *right)
{
    const Task *a = *(const Task *const *)left;
    const Task *b = *(const Task *const *)right;
    if (a->score != b->score) return b->score - a->score;
    return a->id - b->id;
}

static void getHealth(SOCKET client)
{
    char body[256];
    int length = sprintf(body, "{\"status\":\"ok\",\"count\":%d}", taskCount);
    sendResponse(client, 200, body, length);
}

static void listTasks(SOCKET client, const char *query)
{
    Task *selected[MAX_TASKS];
    char body[BUFFER_SIZE];
    int hasFilter = 0;
    int wantDone = 0;
    int total = 0;
    int length = 0;
    if (query != NULL && strncmp(query, "done=", 5) == 0) {
        const char *value = query + 5;
        if (strcmp(value, "true") == 0) {
            hasFilter = 1;
            wantDone = 1;
        } else if (strcmp(value, "false") == 0) {
            hasFilter = 1;
            wantDone = 0;
        } else {
            fail(client, 400, "done must be true or false");
            return;
        }
    }
    for (int index = 0; index < taskCount; index += 1) {
        if (!hasFilter || tasks[index].done == wantDone) {
            selected[total] = &tasks[index];
            total += 1;
        }
    }
    qsort(selected, (size_t)total, sizeof selected[0], compareTasks);
    length = sprintf(body, "{\"tasks\":[");
    for (int index = 0; index < total; index += 1) {
        if (index > 0) length += sprintf(body + length, ",");
        length += writeTask(body + length, selected[index]);
    }
    length += sprintf(body + length, "],\"total\":%d}", total);
    sendResponse(client, 200, body, length);
}

static void getTask(SOCKET client, const char *raw)
{
    char body[BUFFER_SIZE];
    const Task *task = NULL;
    int id = 0;
    if (!parseId(raw, &id)) {
        fail(client, 400, "invalid id");
        return;
    }
    task = findTask(id);
    if (task == NULL) {
        fail(client, 404, "task not found");
        return;
    }
    sendResponse(client, 200, body, writeTask(body, task));
}

static void createTask(SOCKET client, const char *rawBody)
{
    char body[BUFFER_SIZE];
    TaskInput input;
    const char *error = NULL;
    Task *task = NULL;
    if (!readInput(rawBody, &input)) {
        fail(client, 400, "invalid json");
        return;
    }
    error = validate(input.title, input.priority);
    if (error != NULL) {
        fail(client, 400, error);
        return;
    }
    task = &tasks[taskCount];
    task->id = nextId;
    strcpy(task->title, input.title);
    task->priority = input.priority;
    task->done = 0;
    task->score = computeScore(input.priority, 0);
    taskCount += 1;
    nextId += 1;
    sendResponse(client, 201, body, writeTask(body, task));
}

static void updateTask(SOCKET client, const char *raw, const char *rawBody)
{
    char body[BUFFER_SIZE];
    TaskInput input;
    const char *error = NULL;
    Task *task = NULL;
    int id = 0;
    if (!parseId(raw, &id)) {
        fail(client, 400, "invalid id");
        return;
    }
    task = findTask(id);
    if (task == NULL) {
        fail(client, 404, "task not found");
        return;
    }
    if (!readInput(rawBody, &input)) {
        fail(client, 400, "invalid json");
        return;
    }
    error = validate(input.title, input.priority);
    if (error != NULL) {
        fail(client, 400, error);
        return;
    }
    strcpy(task->title, input.title);
    task->priority = input.priority;
    task->done = input.done;
    task->score = computeScore(input.priority, input.done);
    sendResponse(client, 200, body, writeTask(body, task));
}

static void deleteTask(SOCKET client, const char *raw)
{
    Task *task = NULL;
    int id = 0;
    int index = 0;
    if (!parseId(raw, &id)) {
        fail(client, 400, "invalid id");
        return;
    }
    task = findTask(id);
    if (task == NULL) {
        fail(client, 404, "task not found");
        return;
    }
    index = (int)(task - tasks);
    memmove(task, task + 1, (size_t)(taskCount - index - 1) * sizeof(Task));
    taskCount -= 1;
    sendResponse(client, 204, NULL, 0);
}

static void getStats(SOCKET client)
{
    char body[BUFFER_SIZE];
    const Task *best = NULL;
    double avgScore = 0.0;
    int doneCount = 0;
    int sumScore = 0;
    int length = 0;
    for (int index = 0; index < taskCount; index += 1) {
        const Task *task = &tasks[index];
        if (task->done) doneCount += 1;
        sumScore += task->score;
        if (!task->done && (best == NULL || task->priority > best->priority)) best = task;
    }
    if (taskCount > 0) avgScore = round((double)sumScore / taskCount * 100.0) / 100.0;
    length = sprintf(body,
                     "{\"total\":%d,\"doneCount\":%d,\"openCount\":%d,"
                     "\"avgScore\":%g,\"topOpenTitle\":",
                     taskCount, doneCount, taskCount - doneCount, avgScore);
    if (best == NULL) length += sprintf(body + length, "null");
    else length += writeJsonString(body + length, best->title);
    length += sprintf(body + length, "}");
    sendResponse(client, 200, body, length);
}

static void handleRequest(SOCKET client, char *request)
{
    char *method = request;
    char *target = NULL;
    char *query = NULL;
    char *body = NULL;
    char *space = strchr(request, ' ');
    if (space == NULL) {
        fail(client, 400, "invalid json");
        return;
    }
    *space = '\0';
    target = space + 1;
    space = strchr(target, ' ');
    if (space == NULL) {
        fail(client, 400, "invalid json");
        return;
    }
    *space = '\0';
    body = strstr(space + 1, "\r\n\r\n");
    body = (body == NULL) ? space + 1 + strlen(space + 1) : body + 4;
    query = strchr(target, '?');
    if (query != NULL) {
        *query = '\0';
        query += 1;
    }
    if (strcmp(target, "/health") == 0 && strcmp(method, "GET") == 0) {
        getHealth(client);
        return;
    }
    if (strcmp(target, "/stats") == 0 && strcmp(method, "GET") == 0) {
        getStats(client);
        return;
    }
    if (strcmp(target, "/tasks") == 0) {
        if (strcmp(method, "GET") == 0) {
            listTasks(client, query);
            return;
        }
        if (strcmp(method, "POST") == 0) {
            createTask(client, body);
            return;
        }
    }
    if (strncmp(target, "/tasks/", 7) == 0) {
        const char *raw = target + 7;
        if (strcmp(method, "GET") == 0) {
            getTask(client, raw);
            return;
        }
        if (strcmp(method, "PUT") == 0) {
            updateTask(client, raw, body);
            return;
        }
        if (strcmp(method, "DELETE") == 0) {
            deleteTask(client, raw);
            return;
        }
    }
    fail(client, 404, "not found");
}

static int equalsIgnoreCase(const char *left, const char *right, size_t length)
{
    for (size_t index = 0; index < length; index += 1) {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) return 0;
    }
    return 1;
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
        handleRequest(client, buffer);
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
