// Task Service — raw winsock implementation in modern C++.

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

constexpr std::size_t MAX_TITLE_LENGTH = 80;
constexpr int MIN_PRIORITY = 1;
constexpr int MAX_PRIORITY = 5;
constexpr unsigned short PORT = 8080;
constexpr int MAX_REQUESTS_PER_CONNECTION = 1000;
constexpr int RECEIVE_TIMEOUT_MS = 5000;

struct Task {
    int id{};
    std::string title;
    int priority{};
    bool done{};
    int score{};
};

struct TaskInput {
    std::string title;
    int priority{};
    bool done{};
};

std::map<int, Task> tasks;
int nextId = 1;

// HTTP/1.1 reuses the connection. The Connection header of the reply follows this.
bool keepConnection = false;

int computeScore(int priority, bool done)
{
    const int baseScore = priority * 10;
    return done ? baseScore : baseScore + 5;
}

std::optional<std::string> validate(const std::string &title, int priority)
{
    if (title.empty()) return "title is required";
    if (title.size() > MAX_TITLE_LENGTH) return "title is too long";
    if (priority < MIN_PRIORITY || priority > MAX_PRIORITY) return "priority is out of range";
    return std::nullopt;
}

std::optional<int> parseId(std::string_view raw)
{
    int value = 0;
    if (raw.empty()) return std::nullopt;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return std::nullopt;
    return value;
}

std::size_t skipSpace(std::string_view text, std::size_t at)
{
    while (at < text.size()
           && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        at += 1;
    }
    return at;
}

std::optional<std::size_t> parseJsonString(std::string_view text, std::size_t at, std::string &out)
{
    if (at >= text.size() || text[at] != '"') return std::nullopt;
    at += 1;
    out.clear();
    while (at < text.size() && text[at] != '"') {
        char value = text[at];
        if (value == '\\') {
            at += 1;
            if (at >= text.size()) return std::nullopt;
            switch (text[at]) {
            case '"': value = '"'; break;
            case '\\': value = '\\'; break;
            case '/': value = '/'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: return std::nullopt;
            }
        }
        out += value;
        at += 1;
    }
    if (at >= text.size()) return std::nullopt;
    return at + 1;
}

std::optional<TaskInput> readInput(std::string_view body)
{
    TaskInput input;
    std::size_t at = skipSpace(body, 0);
    if (at >= body.size() || body[at] != '{') return std::nullopt;
    at = skipSpace(body, at + 1);
    if (at < body.size() && body[at] == '}') return input;
    for (;;) {
        std::string key;
        auto next = parseJsonString(body, at, key);
        if (!next) return std::nullopt;
        at = skipSpace(body, *next);
        if (at >= body.size() || body[at] != ':') return std::nullopt;
        at = skipSpace(body, at + 1);
        if (key == "title") {
            next = parseJsonString(body, at, input.title);
            if (!next) return std::nullopt;
            at = *next;
        } else if (key == "priority") {
            const auto result =
                std::from_chars(body.data() + at, body.data() + body.size(), input.priority);
            if (result.ec != std::errc{}) return std::nullopt;
            at = static_cast<std::size_t>(result.ptr - body.data());
        } else if (key == "done") {
            if (body.substr(at).starts_with("true")) {
                input.done = true;
                at += 4;
            } else if (body.substr(at).starts_with("false")) {
                input.done = false;
                at += 5;
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        at = skipSpace(body, at);
        if (at >= body.size()) return std::nullopt;
        if (body[at] == ',') {
            at = skipSpace(body, at + 1);
            continue;
        }
        if (body[at] == '}') return input;
        return std::nullopt;
    }
}

std::string writeJsonString(std::string_view text)
{
    std::string out = "\"";
    for (const char value : text) {
        switch (value) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += value; break;
        }
    }
    out += '"';
    return out;
}

std::string writeTask(const Task &task)
{
    return std::format(R"({{"id":{},"title":{},"priority":{},"done":{},"score":{}}})", task.id,
                       writeJsonString(task.title), task.priority, task.done ? "true" : "false",
                       task.score);
}

// A persistent connection cannot survive a short write, so send the whole reply.
void sendAll(SOCKET client, const std::string &data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int wrote = send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (wrote <= 0) return;
        sent += static_cast<std::size_t>(wrote);
    }
}

void sendResponse(SOCKET client, int status, const std::string &body)
{
    std::string_view reason = "OK";
    if (status == 201) reason = "Created";
    else if (status == 204) reason = "No Content";
    else if (status == 400) reason = "Bad Request";
    else if (status == 404) reason = "Not Found";
    const std::string response = std::format("HTTP/1.1 {} {}\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: {}\r\n"
                                             "Connection: {}\r\n\r\n{}",
                                             status, reason, body.size(),
                                             keepConnection ? "keep-alive" : "close", body);
    sendAll(client, response);
}

void fail(SOCKET client, int status, std::string_view message)
{
    sendResponse(client, status, std::format(R"({{"error":"{}"}})", message));
}

void getHealth(SOCKET client)
{
    sendResponse(client, 200, std::format(R"({{"status":"ok","count":{}}})", tasks.size()));
}

void listTasks(SOCKET client, std::string_view query)
{
    std::optional<bool> filter;
    if (query.starts_with("done=")) {
        const std::string_view value = query.substr(5);
        if (value == "true") filter = true;
        else if (value == "false") filter = false;
        else return fail(client, 400, "done must be true or false");
    }
    std::vector<const Task *> selected;
    for (const auto &[id, task] : tasks) {
        if (!filter || task.done == *filter) selected.push_back(&task);
    }
    std::ranges::sort(selected, [](const Task *a, const Task *b) {
        return a->score != b->score ? a->score > b->score : a->id < b->id;
    });
    std::string body = R"({"tasks":[)";
    for (std::size_t index = 0; index < selected.size(); index += 1) {
        if (index > 0) body += ',';
        body += writeTask(*selected[index]);
    }
    body += std::format(R"(],"total":{}}})", selected.size());
    sendResponse(client, 200, body);
}

void getTask(SOCKET client, std::string_view raw)
{
    const auto id = parseId(raw);
    if (!id) return fail(client, 400, "invalid id");
    const auto found = tasks.find(*id);
    if (found == tasks.end()) return fail(client, 404, "task not found");
    sendResponse(client, 200, writeTask(found->second));
}

void createTask(SOCKET client, std::string_view rawBody)
{
    const auto input = readInput(rawBody);
    if (!input) return fail(client, 400, "invalid json");
    if (const auto error = validate(input->title, input->priority)) {
        return fail(client, 400, *error);
    }
    const Task task{nextId, input->title, input->priority, false,
                    computeScore(input->priority, false)};
    tasks[nextId] = task;
    nextId += 1;
    sendResponse(client, 201, writeTask(task));
}

void updateTask(SOCKET client, std::string_view raw, std::string_view rawBody)
{
    const auto id = parseId(raw);
    if (!id) return fail(client, 400, "invalid id");
    const auto found = tasks.find(*id);
    if (found == tasks.end()) return fail(client, 404, "task not found");
    const auto input = readInput(rawBody);
    if (!input) return fail(client, 400, "invalid json");
    if (const auto error = validate(input->title, input->priority)) {
        return fail(client, 400, *error);
    }
    Task &task = found->second;
    task.title = input->title;
    task.priority = input->priority;
    task.done = input->done;
    task.score = computeScore(input->priority, input->done);
    sendResponse(client, 200, writeTask(task));
}

void deleteTask(SOCKET client, std::string_view raw)
{
    const auto id = parseId(raw);
    if (!id) return fail(client, 400, "invalid id");
    if (tasks.erase(*id) == 0) return fail(client, 404, "task not found");
    sendResponse(client, 204, "");
}

void getStats(SOCKET client)
{
    const auto total = static_cast<int>(tasks.size());
    int doneCount = 0;
    int sumScore = 0;
    const Task *best = nullptr;
    for (const auto &[id, task] : tasks) {
        if (task.done) doneCount += 1;
        sumScore += task.score;
        if (!task.done && (best == nullptr || task.priority > best->priority)) best = &task;
    }
    const double avgScore =
        total == 0 ? 0.0 : std::round(static_cast<double>(sumScore) / total * 100.0) / 100.0;
    sendResponse(client, 200,
                 std::format(R"({{"total":{},"doneCount":{},"openCount":{},)"
                             R"("avgScore":{},"topOpenTitle":{}}})",
                             total, doneCount, total - doneCount, avgScore,
                             best == nullptr ? "null" : writeJsonString(best->title)));
}

void handleRequest(SOCKET client, std::string_view request)
{
    const auto firstSpace = request.find(' ');
    if (firstSpace == std::string_view::npos) return fail(client, 400, "invalid json");
    const auto secondSpace = request.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) return fail(client, 400, "invalid json");
    const std::string_view method = request.substr(0, firstSpace);
    std::string_view target = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const auto bodyStart = request.find("\r\n\r\n");
    const std::string_view body =
        bodyStart == std::string_view::npos ? std::string_view{} : request.substr(bodyStart + 4);
    std::string_view query;
    if (const auto mark = target.find('?'); mark != std::string_view::npos) {
        query = target.substr(mark + 1);
        target = target.substr(0, mark);
    }
    if (method == "GET" && target == "/health") return getHealth(client);
    if (method == "GET" && target == "/stats") return getStats(client);
    if (target == "/tasks") {
        if (method == "GET") return listTasks(client, query);
        if (method == "POST") return createTask(client, body);
    }
    if (target.starts_with("/tasks/")) {
        const std::string_view raw = target.substr(7);
        if (method == "GET") return getTask(client, raw);
        if (method == "PUT") return updateTask(client, raw, body);
        if (method == "DELETE") return deleteTask(client, raw);
    }
    fail(client, 404, "not found");
}

std::string lower(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char value : text) {
        out += static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value);
    }
    return out;
}

std::size_t skipBlank(const std::string &text, std::size_t at)
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) at += 1;
    return at;
}

// The request line and the header lines, lowercased, without the closing blank line.
std::string headerBlock(const std::string &request)
{
    const auto marker = request.find("\r\n\r\n");
    return lower(std::string_view{request}.substr(
        0, marker == std::string::npos ? request.size() : marker + 2));
}

// Read one header out of a lowercased header block. The name carries its own colon.
std::string headerValue(const std::string &head, std::string_view name)
{
    const auto at = head.find(name);
    if (at == std::string::npos) return {};
    const std::size_t start = skipBlank(head, at + name.size());
    const auto end = head.find("\r\n", start);
    return head.substr(start, end == std::string::npos ? end : end - start);
}

std::size_t contentLength(const std::string &head)
{
    std::size_t total = 0;
    for (const char digit : headerValue(head, "\r\ncontent-length:")) {
        if (digit < '0' || digit > '9') break;
        total = total * 10 + static_cast<std::size_t>(digit - '0');
    }
    return total;
}

// HTTP/1.1 keeps the connection open unless the request asks to close it.
bool wantsKeepAlive(const std::string &head)
{
    const std::string value = headerValue(head, "\r\nconnection:");
    if (value.find("close") != std::string::npos) return false;
    const auto lineEnd = head.find("\r\n");
    const std::string_view line{head.data(), lineEnd == std::string::npos ? head.size() : lineEnd};
    if (line.find("http/1.0") != std::string_view::npos) {
        return value.find("keep-alive") != std::string::npos;
    }
    return true;
}

// Take exactly one request out of the connection buffer and keep any surplus bytes.
std::optional<std::string> receiveRequest(SOCKET client, std::string &buffer)
{
    char chunk[8192];
    for (;;) {
        if (const auto marker = buffer.find("\r\n\r\n"); marker != std::string::npos) {
            const std::size_t end = marker + 4 + contentLength(headerBlock(buffer));
            if (buffer.size() >= end) {
                std::string request = buffer.substr(0, end);
                buffer.erase(0, end);
                return request;
            }
        }
        const int received = recv(client, chunk, sizeof chunk, 0);
        if (received <= 0) return std::nullopt;
        buffer.append(chunk, static_cast<std::size_t>(received));
    }
}

void serveConnection(SOCKET client)
{
    std::string buffer;
    for (int served = 0; served < MAX_REQUESTS_PER_CONNECTION; served += 1) {
        const auto request = receiveRequest(client, buffer);
        if (!request) return;
        keepConnection = wantsKeepAlive(headerBlock(*request))
                         && served + 1 < MAX_REQUESTS_PER_CONNECTION;
        handleRequest(client, *request);
        if (!keepConnection) return;
    }
}

int main()
{
    WSADATA wsaData{};
    sockaddr_in address{};
    const int reuse = 1;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
    const SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) return 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof reuse);
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) return 1;
    if (listen(server, SOMAXCONN) != 0) return 1;
    for (;;) {
        const SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        // The accept loop serves one connection at a time, so an idle client must time out.
        const DWORD timeout = RECEIVE_TIMEOUT_MS;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof timeout);
        serveConnection(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}
