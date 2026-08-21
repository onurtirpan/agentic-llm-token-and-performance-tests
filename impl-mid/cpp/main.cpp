// Task Service, mid tier — raw winsock implementation in modern C++.

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

constexpr std::size_t MAX_TITLE_LENGTH = 80;
constexpr std::size_t MAX_NAME_LENGTH = 60;
constexpr int MIN_PRIORITY = 1;
constexpr int MAX_PRIORITY = 5;
constexpr int DEFAULT_LIMIT = 20;
constexpr int MAX_LIMIT = 100;
constexpr unsigned short PORT = 8080;
constexpr int MAX_REQUESTS_PER_CONNECTION = 1000;
constexpr int RECEIVE_TIMEOUT_MS = 5000;

struct StatusBonus {
    std::string_view name;
    int bonus;
};

constexpr std::array<StatusBonus, 4> STATUS_BONUS{
    {{"todo", 0}, {"in_progress", 3}, {"done", 5}, {"archived", 0}}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 5> TRANSITIONS{
    {{"todo", "in_progress"},
     {"todo", "archived"},
     {"in_progress", "todo"},
     {"in_progress", "done"},
     {"done", "archived"}}};

constexpr std::array<std::string_view, 3> PROJECT_SORTS{"id", "name", "taskCount"};
constexpr std::array<std::string_view, 5> TASK_SORTS{"id", "title", "priority", "score", "status"};

struct User {
    int id{};
    std::string username;
    std::string password;
    std::string role;
};

struct Project {
    int id{};
    std::string name;
    int ownerId{};
};

struct Task {
    int id{};
    int projectId{};
    std::string title;
    int priority{};
    std::string status;
    std::optional<int> assigneeId;
    int score{};
};

struct TaskInput {
    std::string title;
    int priority{};
    std::optional<int> assigneeId;
};

struct Detail {
    std::string field;
    std::string message;
};

using Details = std::vector<Detail>;

struct AppError {
    int status{};
    std::string code;
    std::string message;
    Details details;
};

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    double number{};
    bool integral{};
    std::string text;
};

using Body = std::map<std::string, Json>;
using Query = std::map<std::string, std::string>;

struct Page {
    int limit = DEFAULT_LIMIT;
    int offset = 0;
    std::string sort = "id";
    std::string order = "asc";
};

struct Request {
    std::string method;
    std::string path;
    Query params;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string requestId;
    std::optional<int> userId;
};

struct Reply {
    int status{};
    std::string body;
};

std::map<int, User> users{{1, {1, "admin", "admin-secret", "admin"}},
                          {2, {2, "alice", "alice-secret", "user"}},
                          {3, {3, "bob", "bob-secret", "user"}}};
std::map<std::string, int> sessions;
std::map<int, Project> projects;
std::map<int, Task> tasks;
int nextProjectId = 1;
int nextTaskId = 1;

std::optional<int> statusBonus(std::string_view status)
{
    for (const auto &entry : STATUS_BONUS) {
        if (entry.name == status) return entry.bonus;
    }
    return std::nullopt;
}

int computeScore(int priority, std::string_view status)
{
    const int baseScore = priority * 10;
    return baseScore + statusBonus(status).value_or(0);
}

int taskCount(int projectId)
{
    return static_cast<int>(std::ranges::count_if(
        tasks, [projectId](const auto &entry) { return entry.second.projectId == projectId; }));
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

std::string newToken()
{
    static std::mt19937 engine{std::random_device{}()};
    std::string token;
    for (int index = 0; index < 32; index += 1) {
        token += "0123456789abcdef"[engine() % 16];
    }
    return token;
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

std::optional<std::size_t> parseJsonNumber(std::string_view text, std::size_t at, Json &out)
{
    const std::size_t start = at;
    const auto isDigit = [](char value) { return value >= '0' && value <= '9'; };
    if (at < text.size() && text[at] == '-') at += 1;
    std::size_t mark = at;
    while (at < text.size() && isDigit(text[at])) at += 1;
    if (at == mark) return std::nullopt;
    bool integral = true;
    if (at < text.size() && text[at] == '.') {
        integral = false;
        at += 1;
        mark = at;
        while (at < text.size() && isDigit(text[at])) at += 1;
        if (at == mark) return std::nullopt;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        integral = false;
        at += 1;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) at += 1;
        mark = at;
        while (at < text.size() && isDigit(text[at])) at += 1;
        if (at == mark) return std::nullopt;
    }
    out.kind = Json::Kind::Number;
    out.integral = integral;
    out.number = std::strtod(std::string{text.substr(start, at - start)}.c_str(), nullptr);
    return at;
}

std::optional<std::size_t> parseJsonValue(std::string_view text, std::size_t at, Json &out);

std::optional<std::size_t> parseJsonObject(std::string_view text, std::size_t at, Body *out)
{
    if (at >= text.size() || text[at] != '{') return std::nullopt;
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == '}') return at + 1;
    for (;;) {
        std::string key;
        auto next = parseJsonString(text, at, key);
        if (!next) return std::nullopt;
        at = skipSpace(text, *next);
        if (at >= text.size() || text[at] != ':') return std::nullopt;
        Json value;
        next = parseJsonValue(text, skipSpace(text, at + 1), value);
        if (!next) return std::nullopt;
        if (out != nullptr) out->insert_or_assign(std::move(key), std::move(value));
        at = skipSpace(text, *next);
        if (at >= text.size()) return std::nullopt;
        if (text[at] == ',') {
            at = skipSpace(text, at + 1);
            continue;
        }
        if (text[at] == '}') return at + 1;
        return std::nullopt;
    }
}

std::optional<std::size_t> parseJsonArray(std::string_view text, std::size_t at)
{
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == ']') return at + 1;
    for (;;) {
        Json value;
        const auto next = parseJsonValue(text, at, value);
        if (!next) return std::nullopt;
        at = skipSpace(text, *next);
        if (at >= text.size()) return std::nullopt;
        if (text[at] == ',') {
            at = skipSpace(text, at + 1);
            continue;
        }
        if (text[at] == ']') return at + 1;
        return std::nullopt;
    }
}

std::optional<std::size_t> parseJsonValue(std::string_view text, std::size_t at, Json &out)
{
    if (at >= text.size()) return std::nullopt;
    switch (text[at]) {
    case '"':
        out.kind = Json::Kind::String;
        return parseJsonString(text, at, out.text);
    case '{':
        out.kind = Json::Kind::Object;
        return parseJsonObject(text, at, nullptr);
    case '[':
        out.kind = Json::Kind::Array;
        return parseJsonArray(text, at);
    case 't':
        if (!text.substr(at).starts_with("true")) return std::nullopt;
        out.kind = Json::Kind::Bool;
        return at + 4;
    case 'f':
        if (!text.substr(at).starts_with("false")) return std::nullopt;
        out.kind = Json::Kind::Bool;
        return at + 5;
    case 'n':
        if (!text.substr(at).starts_with("null")) return std::nullopt;
        out.kind = Json::Kind::Null;
        return at + 4;
    default:
        return parseJsonNumber(text, at, out);
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

std::string serializeProject(const Project &project)
{
    return std::format(R"({{"id":{},"name":{},"ownerId":{},"taskCount":{}}})", project.id,
                       writeJsonString(project.name), project.ownerId, taskCount(project.id));
}

std::string serializeTask(const Task &task)
{
    return std::format(R"({{"id":{},"projectId":{},"title":{},"priority":{},)"
                       R"("status":{},"assigneeId":{},"score":{}}})",
                       task.id, task.projectId, writeJsonString(task.title), task.priority,
                       writeJsonString(task.status),
                       task.assigneeId ? std::to_string(*task.assigneeId) : std::string{"null"},
                       task.score);
}

AppError badRequest()
{
    return {400, "bad_request", "the request is malformed", {}};
}

AppError notFound()
{
    return {404, "not_found", "the resource does not exist", {}};
}

AppError forbidden()
{
    return {403, "forbidden", "you may not access this resource", {}};
}

AppError conflict()
{
    return {409, "conflict", "the resource already exists", {}};
}

Detail fail(const std::string &field, const std::string &message)
{
    return {field, message};
}

AppError invalid(Details details)
{
    std::ranges::sort(details, [](const Detail &a, const Detail &b) {
        return a.field != b.field ? a.field < b.field : a.message < b.message;
    });
    return {422, "validation_failed", "the request body is not valid", std::move(details)};
}

Body readBody(std::string_view raw)
{
    Body body;
    const std::size_t at = skipSpace(raw, 0);
    if (at >= raw.size()) return body;
    const auto next = parseJsonObject(raw, at, &body);
    if (!next || skipSpace(raw, *next) != raw.size()) throw badRequest();
    return body;
}

std::optional<int> readInt(const Body &body, const std::string &field, std::optional<int> fallback)
{
    const auto found = body.find(field);
    if (found == body.end()) return fallback;
    if (found->second.kind == Json::Kind::Null) return std::nullopt;
    if (found->second.kind == Json::Kind::Number && found->second.integral) {
        return static_cast<int>(found->second.number);
    }
    throw badRequest();
}

std::string readString(const Body &body, const std::string &field, Details &errors,
                       std::size_t maxLength, bool required)
{
    std::string value;
    if (const auto found = body.find(field); found != body.end()) {
        if (found->second.kind != Json::Kind::String) throw badRequest();
        value = found->second.text;
    }
    if (value.empty()) {
        if (required) errors.push_back(fail(field, field + " is required"));
    } else if (value.size() > maxLength) {
        errors.push_back(fail(field, field + " is too long"));
    }
    return value;
}

int readPriority(const Body &body, Details &errors)
{
    const auto value = readInt(body, "priority", 0);
    if (!value || *value < MIN_PRIORITY || *value > MAX_PRIORITY) {
        errors.push_back(fail("priority", "priority is out of range"));
    }
    return value.value_or(0);
}

std::optional<int> readUserRef(const Body &body, const std::string &field, Details &errors,
                               std::optional<int> fallback)
{
    const auto value = readInt(body, field, fallback);
    if (value && !users.contains(*value)) {
        errors.push_back(fail(field, field + " is not a known user"));
    }
    return value;
}

TaskInput readTaskInput(const Body &body)
{
    Details errors;
    TaskInput input;
    input.title = readString(body, "title", errors, MAX_TITLE_LENGTH, true);
    input.priority = readPriority(body, errors);
    input.assigneeId = readUserRef(body, "assigneeId", errors, std::nullopt);
    if (!errors.empty()) throw invalid(std::move(errors));
    return input;
}

int parseId(const std::string &raw)
{
    int value = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) throw badRequest();
    return value;
}

int readQueryInt(const std::string &raw)
{
    int value = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return -1;
    return value;
}

Page readPage(const Query &query, std::span<const std::string_view> allowed)
{
    Details errors;
    Page page;
    if (const auto found = query.find("sort"); found != query.end()) page.sort = found->second;
    if (const auto found = query.find("order"); found != query.end()) page.order = found->second;
    if (const auto found = query.find("limit"); found != query.end()) {
        page.limit = readQueryInt(found->second);
        if (page.limit < 1 || page.limit > MAX_LIMIT) {
            errors.push_back(fail("limit", "limit is out of range"));
        }
    }
    if (const auto found = query.find("offset"); found != query.end()) {
        page.offset = readQueryInt(found->second);
        if (page.offset < 0) errors.push_back(fail("offset", "offset is out of range"));
    }
    if (std::ranges::find(allowed, page.sort) == allowed.end()) {
        errors.push_back(fail("sort", "sort is not a valid field"));
    }
    if (page.order != "asc" && page.order != "desc") {
        errors.push_back(fail("order", "order must be asc or desc"));
    }
    if (!errors.empty()) throw invalid(std::move(errors));
    return page;
}

template <typename Entity, typename Less, typename Serialize>
std::string paginate(std::vector<const Entity *> rows, const Page &page, Less less,
                     Serialize serialize)
{
    const bool desc = page.order == "desc";
    std::ranges::sort(rows, [&](const Entity *a, const Entity *b) {
        const Entity &first = desc ? *b : *a;
        const Entity &second = desc ? *a : *b;
        if (less(first, second, page.sort)) return true;
        if (less(second, first, page.sort)) return false;
        return a->id < b->id;
    });
    const std::size_t total = rows.size();
    const std::size_t start = std::min(static_cast<std::size_t>(page.offset), total);
    const std::size_t stop = std::min(start + static_cast<std::size_t>(page.limit), total);
    std::string items;
    for (std::size_t index = start; index < stop; index += 1) {
        if (index > start) items += ',';
        items += serialize(*rows[index]);
    }
    return std::format(R"({{"items":[{}],"total":{},"limit":{},"offset":{}}})", items, total,
                       page.limit, page.offset);
}

bool lessProject(const Project &a, const Project &b, const std::string &sort)
{
    if (sort == "name") return a.name < b.name;
    if (sort == "taskCount") return taskCount(a.id) < taskCount(b.id);
    return a.id < b.id;
}

bool lessTask(const Task &a, const Task &b, const std::string &sort)
{
    if (sort == "title") return a.title < b.title;
    if (sort == "priority") return a.priority < b.priority;
    if (sort == "score") return a.score < b.score;
    if (sort == "status") return a.status < b.status;
    return a.id < b.id;
}

User &authenticate(Request &request)
{
    if (const auto header = request.headers.find("authorization");
        header != request.headers.end() && header->second.starts_with("Bearer ")) {
        if (const auto session = sessions.find(header->second.substr(7));
            session != sessions.end()) {
            request.userId = session->second;
            return users.at(session->second);
        }
    }
    throw AppError{401, "unauthorized", "authentication is required", {}};
}

void requireAdmin(const User &user)
{
    if (user.role != "admin") throw forbidden();
}

Project &reachableProject(int projectId, const User &user)
{
    const auto found = projects.find(projectId);
    if (found == projects.end()) throw notFound();
    if (user.role != "admin" && found->second.ownerId != user.id) throw forbidden();
    return found->second;
}

Task &reachableTask(int taskId, const User &user)
{
    const auto found = tasks.find(taskId);
    if (found == tasks.end()) throw notFound();
    reachableProject(found->second.projectId, user);
    return found->second;
}

Reply getHealth()
{
    return {200, std::format(R"({{"status":"ok","projects":{},"tasks":{}}})", projects.size(),
                             tasks.size())};
}

Reply login(Request &request)
{
    const Body body = readBody(request.body);
    Details errors;
    const std::string username = readString(body, "username", errors, MAX_NAME_LENGTH, true);
    const std::string password = readString(body, "password", errors, MAX_NAME_LENGTH, true);
    if (!errors.empty()) throw invalid(std::move(errors));
    for (const auto &[id, user] : users) {
        if (user.username == username && user.password == password) {
            const std::string token = newToken();
            sessions[token] = user.id;
            return {200, std::format(R"({{"token":{},"userId":{},"role":{}}})",
                                     writeJsonString(token), user.id, writeJsonString(user.role))};
        }
    }
    throw AppError{401, "invalid_credentials", "the username or password is wrong", {}};
}

Reply logout(Request &request)
{
    authenticate(request);
    sessions.erase(request.headers.at("authorization").substr(7));
    return {204, ""};
}

Reply getMe(Request &request)
{
    const User &user = authenticate(request);
    return {200, std::format(R"({{"userId":{},"username":{},"role":{}}})", user.id,
                             writeJsonString(user.username), writeJsonString(user.role))};
}

Reply listProjects(Request &request)
{
    const User &user = authenticate(request);
    const Page page = readPage(request.params, PROJECT_SORTS);
    std::vector<const Project *> rows;
    for (const auto &[id, project] : projects) {
        if (user.role == "admin" || project.ownerId == user.id) rows.push_back(&project);
    }
    return {200, paginate(std::move(rows), page, lessProject, serializeProject)};
}

Reply createProject(Request &request)
{
    const User &user = authenticate(request);
    requireAdmin(user);
    const Body body = readBody(request.body);
    Details errors;
    const std::string name = readString(body, "name", errors, MAX_NAME_LENGTH, true);
    const int ownerId = readUserRef(body, "ownerId", errors, user.id).value_or(user.id);
    if (!errors.empty()) throw invalid(std::move(errors));
    for (const auto &[id, other] : projects) {
        if (other.ownerId == ownerId && other.name == name) throw conflict();
    }
    const Project project{nextProjectId, name, ownerId};
    projects[nextProjectId] = project;
    nextProjectId += 1;
    return {201, serializeProject(project)};
}

Reply getProject(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    return {200, serializeProject(reachableProject(parseId(raw), user))};
}

Reply updateProject(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    requireAdmin(user);
    Project &project = reachableProject(parseId(raw), user);
    const Body body = readBody(request.body);
    if (!body.contains("name")) return {200, serializeProject(project)};
    Details errors;
    const std::string name = readString(body, "name", errors, MAX_NAME_LENGTH, true);
    if (!errors.empty()) throw invalid(std::move(errors));
    for (const auto &[id, other] : projects) {
        if (other.ownerId == project.ownerId && other.name == name && other.id != project.id) {
            throw conflict();
        }
    }
    project.name = name;
    return {200, serializeProject(project)};
}

Reply deleteProject(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    requireAdmin(user);
    const int projectId = reachableProject(parseId(raw), user).id;
    std::erase_if(tasks,
                  [projectId](const auto &entry) { return entry.second.projectId == projectId; });
    projects.erase(projectId);
    return {204, ""};
}

Reply listTasks(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    const Project &project = reachableProject(parseId(raw), user);
    const Page page = readPage(request.params, TASK_SORTS);
    std::vector<const Task *> rows;
    for (const auto &[id, task] : tasks) {
        if (task.projectId == project.id) rows.push_back(&task);
    }
    return {200, paginate(std::move(rows), page, lessTask, serializeTask)};
}

Reply createTask(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    const Project &project = reachableProject(parseId(raw), user);
    const TaskInput input = readTaskInput(readBody(request.body));
    const Task task{nextTaskId,       project.id,        input.title, input.priority,
                    "todo",           input.assigneeId,  computeScore(input.priority, "todo")};
    tasks[nextTaskId] = task;
    nextTaskId += 1;
    return {201, serializeTask(task)};
}

Reply getTask(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    return {200, serializeTask(reachableTask(parseId(raw), user))};
}

Reply replaceTask(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    Task &task = reachableTask(parseId(raw), user);
    const TaskInput input = readTaskInput(readBody(request.body));
    task.title = input.title;
    task.priority = input.priority;
    task.assigneeId = input.assigneeId;
    task.score = computeScore(input.priority, task.status);
    return {200, serializeTask(task)};
}

Reply updateStatus(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    Task &task = reachableTask(parseId(raw), user);
    const Body body = readBody(request.body);
    const auto found = body.find("status");
    if (found == body.end() || found->second.kind != Json::Kind::String
        || !statusBonus(found->second.text)) {
        throw invalid({fail("status", "status is not valid")});
    }
    const std::string &status = found->second.text;
    const bool allowed = std::ranges::any_of(TRANSITIONS, [&](const auto &move) {
        return move.first == task.status && move.second == status;
    });
    if (!allowed) throw AppError{409, "invalid_transition", "the status change is not allowed", {}};
    task.status = status;
    task.score = computeScore(task.priority, status);
    return {200, serializeTask(task)};
}

Reply deleteTask(Request &request, const std::string &raw)
{
    const User &user = authenticate(request);
    tasks.erase(reachableTask(parseId(raw), user).id);
    return {204, ""};
}

Reply getStats(Request &request)
{
    const User &user = authenticate(request);
    requireAdmin(user);
    std::string byStatus;
    for (const auto &entry : STATUS_BONUS) {
        const auto count = std::ranges::count_if(
            tasks, [&entry](const auto &row) { return row.second.status == entry.name; });
        if (!byStatus.empty()) byStatus += ',';
        byStatus += std::format(R"("{}":{})", entry.name, count);
    }
    int sumScore = 0;
    for (const auto &[id, task] : tasks) sumScore += task.score;
    const std::size_t total = tasks.size();
    const double avgScore =
        total == 0 ? 0.0
                   : std::round(static_cast<double>(sumScore) / static_cast<double>(total) * 100.0)
                         / 100.0;
    const Project *best = nullptr;
    for (const auto &[id, project] : projects) {
        if (best == nullptr || taskCount(project.id) > taskCount(best->id)) best = &project;
    }
    return {200, std::format(R"({{"projects":{},"tasks":{},"users":{},"sessions":{},)"
                             R"("byStatus":{{{}}},"avgScore":{},"topProjectName":{}}})",
                             projects.size(), total, users.size(), sessions.size(), byStatus,
                             avgScore,
                             best == nullptr ? std::string{"null"} : writeJsonString(best->name))};
}

Reply handleRequest(Request &request)
{
    try {
        const std::string &method = request.method;
        const std::string &path = request.path;
        if (method == "GET" && path == "/health") return getHealth();
        if (method == "POST" && path == "/auth/login") return login(request);
        if (method == "POST" && path == "/auth/logout") return logout(request);
        if (method == "GET" && path == "/me") return getMe(request);
        if (method == "GET" && path == "/stats") return getStats(request);
        if (path == "/projects") {
            if (method == "GET") return listProjects(request);
            if (method == "POST") return createProject(request);
        }
        if (path.starts_with("/projects/")) {
            const std::string rest = path.substr(10);
            const auto slash = rest.find('/');
            if (slash == std::string::npos) {
                if (method == "GET") return getProject(request, rest);
                if (method == "PATCH") return updateProject(request, rest);
                if (method == "DELETE") return deleteProject(request, rest);
            } else if (rest.substr(slash) == "/tasks") {
                if (method == "GET") return listTasks(request, rest.substr(0, slash));
                if (method == "POST") return createTask(request, rest.substr(0, slash));
            }
        }
        if (path.starts_with("/tasks/")) {
            const std::string rest = path.substr(7);
            const auto slash = rest.find('/');
            if (slash == std::string::npos) {
                if (method == "GET") return getTask(request, rest);
                if (method == "PUT") return replaceTask(request, rest);
                if (method == "DELETE") return deleteTask(request, rest);
            } else if (rest.substr(slash) == "/status" && method == "PATCH") {
                return updateStatus(request, rest.substr(0, slash));
            }
        }
        throw notFound();
    } catch (const AppError &error) {
        std::string details;
        for (const Detail &entry : error.details) {
            if (!details.empty()) details += ',';
            details += std::format(R"({{"field":{},"message":{}}})", writeJsonString(entry.field),
                                   writeJsonString(entry.message));
        }
        return {error.status,
                std::format(R"({{"error":{{"code":{},"message":{},"requestId":{},"details":[{}]}}}})",
                            writeJsonString(error.code), writeJsonString(error.message),
                            writeJsonString(request.requestId), details)};
    }
}

Query parseQuery(std::string_view raw)
{
    Query query;
    while (!raw.empty()) {
        const auto separator = raw.find('&');
        const std::string_view pair = raw.substr(0, separator);
        raw = separator == std::string_view::npos ? std::string_view{} : raw.substr(separator + 1);
        if (pair.empty()) continue;
        const auto equals = pair.find('=');
        std::string name{pair.substr(0, equals)};
        std::string value =
            equals == std::string_view::npos ? std::string{} : std::string{pair.substr(equals + 1)};
        query.emplace(std::move(name), std::move(value));
    }
    return query;
}

std::optional<Request> parseRequest(const std::string &raw)
{
    const auto lineEnd = raw.find("\r\n");
    const auto firstSpace = raw.find(' ');
    if (lineEnd == std::string::npos || firstSpace == std::string::npos) return std::nullopt;
    const auto secondSpace = raw.find(' ', firstSpace + 1);
    if (secondSpace == std::string::npos || secondSpace > lineEnd) return std::nullopt;
    Request request;
    request.method = raw.substr(0, firstSpace);
    std::string target = raw.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (const auto mark = target.find('?'); mark != std::string::npos) {
        request.params = parseQuery(std::string_view{target}.substr(mark + 1));
        target = target.substr(0, mark);
    }
    request.path = target;
    for (std::size_t at = lineEnd + 2;;) {
        const auto end = raw.find("\r\n", at);
        if (end == std::string::npos || end == at) break;
        if (const auto colon = raw.find(':', at); colon != std::string::npos && colon < end) {
            std::size_t valueStart = colon + 1;
            while (valueStart < end && raw[valueStart] == ' ') valueStart += 1;
            request.headers.insert_or_assign(lower(std::string_view{raw}.substr(at, colon - at)),
                                             raw.substr(valueStart, end - valueStart));
        }
        at = end + 2;
    }
    if (const auto bodyStart = raw.find("\r\n\r\n"); bodyStart != std::string::npos) {
        request.body = raw.substr(bodyStart + 4);
    }
    const auto given = request.headers.find("x-request-id");
    request.requestId = given != request.headers.end() && !given->second.empty()
                            ? given->second
                            : newToken().substr(0, 12);
    return request;
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

void sendResponse(SOCKET client, int status, const std::string &body, const std::string &requestId,
                  bool keepAlive)
{
    std::string_view reason = "OK";
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
    const std::string response = std::format("HTTP/1.1 {} {}\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: {}\r\n"
                                             "X-Request-Id: {}\r\n"
                                             "Connection: {}\r\n\r\n{}",
                                             status, reason, body.size(), requestId,
                                             keepAlive ? "keep-alive" : "close", body);
    sendAll(client, response);
}

// Answer one request. A request line that will not parse leaves the reply unsent.
bool observe(SOCKET client, const std::string &raw, bool keepAlive)
{
    auto parsed = parseRequest(raw);
    if (!parsed) return false;
    Request &request = *parsed;
    const auto started = std::chrono::steady_clock::now();
    const Reply reply = handleRequest(request);
    sendResponse(client, reply.status, reply.body, request.requestId, keepAlive);
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const std::string_view level =
        reply.status >= 500 ? "error" : reply.status >= 400 ? "warn" : "info";
    std::cout << std::format(R"({{"level":"{}","requestId":{},"method":{},"path":{},)"
                             R"("status":{},"durationMs":{},"userId":{}}})",
                             level, writeJsonString(request.requestId),
                             writeJsonString(request.method), writeJsonString(request.path),
                             reply.status, durationMs,
                             request.userId ? std::to_string(*request.userId) : std::string{"null"})
              << '\n'
              << std::flush;
    return true;
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
        const bool alive = wantsKeepAlive(headerBlock(*request))
                           && served + 1 < MAX_REQUESTS_PER_CONNECTION;
        if (!observe(client, *request, alive) || !alive) return;
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
