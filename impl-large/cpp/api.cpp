// Task Service, large tier — the winsock loop, the middleware, the routes and main.

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "domain.hpp"
#include "service.hpp"
#include "store.hpp"

struct Request {
    std::string method;
    std::string path;
    Query params;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string requestId;
};

// Everything the response headers and the log line need, even on the error path.
struct Ctx {
    std::string requestId;
    std::optional<int> userId;
    std::optional<int> quotaRemaining;
    bool replayed = false;
    bool keepAlive = false;
};

constexpr int MAX_REQUESTS_PER_CONNECTION = 1000;
constexpr int RECEIVE_TIMEOUT_MS = 5000;

struct Reply {
    int status{};
    std::string body;
    std::string etag;
};

// ------------------------------------------------------------------------ helpers

std::string newToken()
{
    static std::mt19937 engine{std::random_device{}()};
    std::string token;
    for (int index = 0; index < 32; index += 1) {
        token += "0123456789abcdef"[engine() % 16];
    }
    return token;
}

std::optional<std::string> header(const Request &request, const std::string &name)
{
    const auto found = request.headers.find(name);
    if (found == request.headers.end()) return std::nullopt;
    return found->second;
}

std::optional<std::string> param(const Request &request, const std::string &name)
{
    const auto found = request.params.find(name);
    if (found == request.params.end()) return std::nullopt;
    return found->second;
}

std::string envelope(const AppError &error, const std::string &requestId)
{
    std::string details;
    for (const Detail &entry : error.details) {
        if (!details.empty()) details += ',';
        details += std::format(R"({{"field":{},"message":{}}})", writeJsonString(entry.field),
                               writeJsonString(entry.message));
    }
    return std::format(R"({{"error":{{"code":{},"message":{},"requestId":{},"details":[{}]}}}})",
                       writeJsonString(error.code), writeJsonString(error.message),
                       writeJsonString(requestId), details);
}

Json readBody(const std::string &raw)
{
    Json body;
    body.kind = Json::Kind::Object;
    if (skipSpace(raw, 0) >= raw.size()) return body;
    const auto parsed = parseJson(raw);
    if (!parsed || parsed->kind != Json::Kind::Object) throw badRequest();
    return *parsed;
}

std::optional<int> readInt(const Json &body, std::string_view field, std::optional<int> fallback)
{
    const Json *found = body.find(field);
    if (found == nullptr) return fallback;
    if (found->kind == Json::Kind::Null) return std::nullopt;
    if (found->kind == Json::Kind::Number && found->integral) {
        return static_cast<int>(found->number);
    }
    throw badRequest();
}

std::string readText(const Json &body, std::string_view field, const std::string &fallback)
{
    const Json *found = body.find(field);
    if (found == nullptr) return fallback;
    if (found->kind != Json::Kind::String) throw badRequest();
    return found->text;
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
    page.sort = std::string{allowed.front()};
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

// Authenticate, charge the quota, then check the role. This order is fixed.
Caller begin(const Request &request, Ctx &ctx, bool admin = false)
{
    const Caller caller = authenticate(header(request, "authorization").value_or(""));
    ctx.userId = caller.user->id;
    ctx.quotaRemaining = chargeQuota(*caller.user, *caller.session);
    if (admin) requireAdmin(*caller.user);
    return caller;
}

// Run produce once per Idempotency-Key, then replay the recorded outcome verbatim.
template <typename Produce>
Reply idempotent(const Request &request, Ctx &ctx, const std::string &token, Produce produce)
{
    const auto key = header(request, "idempotency-key");
    if (!key) return produce();
    const Slot slot{token, *key};
    if (const auto found = idempotency.find(slot); found != idempotency.end()) {
        ctx.replayed = true;
        return {found->second.status, found->second.body, found->second.etag};
    }
    try {
        const Reply reply = produce();
        idempotency.emplace(slot, Recorded{reply.status, reply.body, reply.etag});
        return reply;
    } catch (const AppError &error) {
        idempotency.emplace(slot, Recorded{error.status, envelope(error, ctx.requestId), ""});
        throw;
    }
}

std::vector<const Task *> taskFilters(const Query &params, std::vector<const Task *> rows)
{
    Details errors;
    const auto wantedStatus = params.find("status");
    const auto wantedAssignee = params.find("assigneeId");
    std::optional<int> assignee;
    if (wantedStatus != params.end() && !statusBonus(wantedStatus->second)) {
        errors.push_back(fail("status", "status is not valid"));
    }
    if (wantedAssignee != params.end()) {
        const std::string &raw = wantedAssignee->second;
        int value = 0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) {
            errors.push_back(fail("assigneeId", "assigneeId is not a known user"));
        } else {
            assignee = value;
        }
    }
    if (!errors.empty()) throw invalid(std::move(errors));
    if (wantedStatus != params.end()) {
        std::erase_if(rows,
                      [&](const Task *task) { return task->status != wantedStatus->second; });
    }
    if (assignee) {
        std::erase_if(rows, [&](const Task *task) { return task->assigneeId != assignee; });
    }
    return rows;
}

// ------------------------------------------------------------------- health, auth

Reply getHealth()
{
    return {200,
            std::format(R"({{"status":"ok","projects":{},"tasks":{},"comments":{}}})",
                        liveProjects(), liveTasks(), comments.size()),
            ""};
}

Reply postLogin(const Request &request)
{
    const Json body = readBody(request.body);
    Details errors;
    const std::string username = readText(body, "username", "");
    const std::string password = readText(body, "password", "");
    if (username.empty()) errors.push_back(fail("username", "username is required"));
    if (password.empty()) errors.push_back(fail("password", "password is required"));
    if (!errors.empty()) throw invalid(std::move(errors));
    const std::string token = newToken();
    const User &user = login(username, password, token);
    return {200,
            std::format(R"({{"token":{},"userId":{},"role":{}}})", writeJsonString(token), user.id,
                        writeJsonString(user.role)),
            ""};
}

Reply postLogout(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    sessions.erase(caller.session->token);
    return {204, "", ""};
}

Reply getMe(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    return {200,
            std::format(R"({{"userId":{},"username":{},"role":{}}})", caller.user->id,
                        writeJsonString(caller.user->username),
                        writeJsonString(caller.user->role)),
            ""};
}

// -------------------------------------------------------------------------- users

Reply getUsers(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    const Page page = readPage(request.params, USER_SORTS);
    std::vector<const User *> rows;
    for (const auto &[id, user] : users) {
        if (!user.deleted) rows.push_back(&user);
    }
    return {200, paginate(std::move(rows), page, lessUser, serializeUser), ""};
}

Reply postUsers(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx, true);
    const Json body = readBody(request.body);
    return idempotent(request, ctx, caller.session->token, [&] {
        const std::string username = readText(body, "username", "");
        const std::string password = readText(body, "password", "");
        const User &user =
            createUser(*caller.user, username, password, body.find("role"), body.find("quota"));
        return Reply{201, serializeUser(user), std::to_string(user.version)};
    });
}

Reply getUserById(const Request &request, Ctx &ctx, const std::string &raw)
{
    begin(request, ctx, true);
    const User *user = findUser(parseId(raw));
    if (user == nullptr) throw notFound();
    return {200, serializeUser(*user), std::to_string(user->version)};
}

Reply patchUserById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx, true);
    User *user = findUser(parseId(raw));
    if (user == nullptr) throw notFound();
    checkIfMatch(header(request, "if-match"), user->version);
    updateUser(*caller.user, *user, readBody(request.body));
    return {200, serializeUser(*user), std::to_string(user->version)};
}

Reply deleteUserById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx, true);
    User *user = findUser(parseId(raw));
    if (user == nullptr) throw notFound();
    checkIfMatch(header(request, "if-match"), user->version);
    deleteUser(*caller.user, *user);
    return {200, serializeUser(*user), std::to_string(user->version)};
}

// ----------------------------------------------------------------------- projects

Reply getProjects(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    const bool include = checkIncludeDeleted(param(request, "includeDeleted"), *caller.user);
    const Page page = readPage(request.params, PROJECT_SORTS);
    return {200,
            paginate(visibleProjects(*caller.user, include), page, lessProject, serializeProject),
            ""};
}

Reply postProjects(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx, true);
    const Json body = readBody(request.body);
    return idempotent(request, ctx, caller.session->token, [&] {
        const std::string name = readText(body, "name", "");
        const std::optional<int> ownerId = readInt(body, "ownerId", caller.user->id);
        const Project &project = createProject(*caller.user, name, ownerId);
        return Reply{201, serializeProject(project), std::to_string(project.version)};
    });
}

Reply getProjectById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Project &project = reachableProject(parseId(raw), *caller.user);
    return {200, serializeProject(project), std::to_string(project.version)};
}

Reply patchProjectById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx, true);
    Project &project = reachableProject(parseId(raw), *caller.user);
    checkIfMatch(header(request, "if-match"), project.version);
    const Json body = readBody(request.body);
    if (body.contains("name")) renameProject(*caller.user, project, readText(body, "name", ""));
    return {200, serializeProject(project), std::to_string(project.version)};
}

Reply deleteProjectById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx, true);
    Project &project = reachableProject(parseId(raw), *caller.user);
    checkIfMatch(header(request, "if-match"), project.version);
    deleteProject(*caller.user, project);
    return {200, serializeProject(project), std::to_string(project.version)};
}

Reply postProjectRestore(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx, true);
    Project &project = reachableProject(parseId(raw), *caller.user, true);
    checkIfMatch(header(request, "if-match"), project.version);
    restoreProject(*caller.user, project);
    return {200, serializeProject(project), std::to_string(project.version)};
}

// -------------------------------------------------------------------------- tasks

Reply getTasks(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    const bool include = checkIncludeDeleted(param(request, "includeDeleted"), *caller.user);
    const Page page = readPage(request.params, TASK_SORTS);
    const std::string role = caller.user->role;
    return {200,
            paginate(taskFilters(request.params, visibleTasks(*caller.user, include)), page,
                     lessTask, [&role](const Task &task) { return serializeTask(task, role); }),
            ""};
}

Reply getProjectTasks(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Project &project = reachableProject(parseId(raw), *caller.user);
    const Page page = readPage(request.params, TASK_SORTS);
    std::vector<const Task *> rows;
    for (const auto &[id, task] : tasks) {
        if (task.projectId == project.id && !task.deleted) rows.push_back(&task);
    }
    const std::string role = caller.user->role;
    return {200,
            paginate(std::move(rows), page, lessTask,
                     [&role](const Task &task) { return serializeTask(task, role); }),
            ""};
}

Reply postProjectTasks(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Project &project = reachableProject(parseId(raw), *caller.user);
    const Json body = readBody(request.body);
    return idempotent(request, ctx, caller.session->token, [&] {
        Details errors;
        const std::string note = readNote(*caller.user, body, errors, "");
        const std::string title = readText(body, "title", "");
        const std::optional<int> priority = readInt(body, "priority", 0);
        const std::optional<int> assigneeId = readInt(body, "assigneeId", std::nullopt);
        const Task &task =
            createTask(*caller.user, project, title, priority, assigneeId, note, errors);
        return Reply{201, serializeTask(task, caller.user->role), std::to_string(task.version)};
    });
}

Reply getTaskById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Task &task = reachableTask(parseId(raw), *caller.user);
    return {200, serializeTask(task, caller.user->role), std::to_string(task.version)};
}

Reply putTaskById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    Task &task = reachableTask(parseId(raw), *caller.user);
    checkIfMatch(header(request, "if-match"), task.version);
    const Json body = readBody(request.body);
    Details errors;
    const std::string note = readNote(*caller.user, body, errors, task.internalNote);
    const std::string title = readText(body, "title", "");
    const std::optional<int> priority = readInt(body, "priority", 0);
    const std::optional<int> assigneeId = readInt(body, "assigneeId", std::nullopt);
    replaceTask(*caller.user, task, title, priority, assigneeId, note, errors);
    return {200, serializeTask(task, caller.user->role), std::to_string(task.version)};
}

Reply patchTaskStatus(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    Task &task = reachableTask(parseId(raw), *caller.user);
    checkIfMatch(header(request, "if-match"), task.version);
    const Json body = readBody(request.body);
    moveStatus(*caller.user, task, body.find("status"));
    return {200, serializeTask(task, caller.user->role), std::to_string(task.version)};
}

Reply deleteTaskById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    Task &task = reachableTask(parseId(raw), *caller.user);
    checkIfMatch(header(request, "if-match"), task.version);
    deleteTask(*caller.user, task);
    return {200, serializeTask(task, caller.user->role), std::to_string(task.version)};
}

Reply postTaskRestore(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    Task &task = reachableTask(parseId(raw), *caller.user, true);
    checkIfMatch(header(request, "if-match"), task.version);
    restoreTask(*caller.user, task);
    return {200, serializeTask(task, caller.user->role), std::to_string(task.version)};
}

// The item's own version stands in for If-Match. An absent one can never match.
std::string bulkVersion(const Json &item)
{
    const Json *value = item.find("version");
    if (value == nullptr) return "None";
    if (value->kind == Json::Kind::Number && value->integral) {
        return std::to_string(static_cast<int>(value->number));
    }
    if (value->kind == Json::Kind::String) return value->text;
    return "None";
}

std::pair<int, int> runBulk(const User &actor, const Json &item)
{
    const Json *op = item.find("op");
    const std::string name = op != nullptr && op->kind == Json::Kind::String ? op->text : "";
    if (name == "create") {
        const Project &project = reachableProject(readInt(item, "projectId", 0).value_or(0), actor);
        Details errors;
        const std::string title = readText(item, "title", "");
        const std::string note;
        const std::optional<int> priority = readInt(item, "priority", 0);
        const Task &task = createTask(actor, project, title, priority, std::nullopt, note, errors);
        return {201, task.id};
    }
    if (name == "status") {
        Task &task = reachableTask(readInt(item, "id", 0).value_or(0), actor);
        checkIfMatch(bulkVersion(item), task.version);
        moveStatus(actor, task, item.find("status"));
        return {200, task.id};
    }
    if (name == "delete") {
        Task &task = reachableTask(readInt(item, "id", 0).value_or(0), actor);
        checkIfMatch(bulkVersion(item), task.version);
        deleteTask(actor, task);
        return {200, task.id};
    }
    throw invalid({fail("op", "op is not valid")});
}

Reply postTasksBulk(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    const Json body = readBody(request.body);
    const Json *operations = body.find("operations");
    checkBulkSize(operations);
    std::string results;
    for (std::size_t index = 0; index < operations->items.size(); index += 1) {
        if (!results.empty()) results += ',';
        try {
            const Json &item = operations->items[index];
            if (item.kind != Json::Kind::Object) throw badRequest();
            const auto [status, id] = runBulk(*caller.user, item);
            results += std::format(R"({{"index":{},"status":{},"id":{},"error":null}})", index,
                                   status, id);
        } catch (const AppError &error) {
            results += std::format(R"({{"index":{},"status":{},"id":null,"error":{}}})", index,
                                   error.status, writeJsonString(error.code));
        }
    }
    return {200, std::format(R"({{"results":[{}]}})", results), ""};
}

// ----------------------------------------------------------------------- comments

Reply getTaskComments(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Task &task = reachableTask(parseId(raw), *caller.user);
    const Page page = readPage(request.params, COMMENT_SORTS);
    std::vector<const Comment *> rows;
    for (const auto &[id, comment] : comments) {
        if (comment.taskId == task.id) rows.push_back(&comment);
    }
    return {200, paginate(std::move(rows), page, lessComment, serializeComment), ""};
}

Reply postTaskComments(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Task &task = reachableTask(parseId(raw), *caller.user);
    const Json body = readBody(request.body);
    return idempotent(request, ctx, caller.session->token, [&] {
        const std::string content = readText(body, "body", "");
        const Comment &comment = createComment(*caller.user, task, content);
        return Reply{201, serializeComment(comment), ""};
    });
}

Reply deleteCommentById(const Request &request, Ctx &ctx, const std::string &raw)
{
    const Caller caller = begin(request, ctx);
    const Comment *comment = findComment(parseId(raw));
    if (comment == nullptr) throw notFound();
    reachableTask(comment->taskId, *caller.user, true);
    removeComment(*caller.user, *comment);
    return {204, "", ""};
}

// ---------------------------------------------------- search, reports, telemetry

Reply getSearch(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    const std::string query = param(request, "q").value_or("");
    if (query.empty()) throw invalid({fail("q", "q is required")});
    return {200, search(*caller.user, query), ""};
}

Reply getWorkload(const Request &request, Ctx &ctx)
{
    const Caller caller = begin(request, ctx);
    const std::string groupBy = param(request, "groupBy").value_or("status");
    if (std::ranges::find(GROUP_BYS, groupBy) == GROUP_BYS.end()) {
        throw invalid({fail("groupBy", "groupBy is not valid")});
    }
    return {200, workload(*caller.user, groupBy), ""};
}

Reply getAudit(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    const Page page = readPage(request.params, SEQ_SORTS);
    const auto actorId = param(request, "actorId");
    const auto resource = param(request, "resource");
    const auto action = param(request, "action");
    std::vector<const AuditEntry *> rows;
    for (const AuditEntry &entry : audit) {
        if (actorId && std::to_string(entry.actorId) != *actorId) continue;
        if (resource && entry.resource != *resource) continue;
        if (action && entry.action != *action) continue;
        rows.push_back(&entry);
    }
    return {200, paginate(std::move(rows), page, lessAudit, serializeAudit), ""};
}

Reply getOutbox(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    const Page page = readPage(request.params, SEQ_SORTS);
    const auto delivered = param(request, "delivered");
    std::vector<const OutboxEvent *> rows;
    for (const OutboxEvent &event : outbox) {
        if (delivered && event.delivered != (*delivered == "true")) continue;
        rows.push_back(&event);
    }
    return {200, paginate(std::move(rows), page, lessOutbox, serializeOutbox), ""};
}

Reply postOutboxFlush(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    return {200, std::format(R"({{"flushed":{}}})", flushOutbox()), ""};
}

Reply getMetrics(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    return {200, metrics(), ""};
}

Reply getStats(const Request &request, Ctx &ctx)
{
    begin(request, ctx, true);
    return {200, stats(), ""};
}

// ------------------------------------------------------------------------ routing

std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at < path.size()) {
        const auto slash = path.find('/', at);
        std::string part =
            path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (!part.empty()) parts.push_back(std::move(part));
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return parts;
}

Reply dispatch(const Request &request, Ctx &ctx, std::string &pattern)
{
    const std::string &method = request.method;
    const std::vector<std::string> parts = splitPath(request.path);
    const auto mark = [&](std::string_view shape) {
        pattern = std::format("{} {}", method, shape);
    };
    if (parts.size() == 1) {
        const std::string &head = parts[0];
        if (head == "health" && method == "GET") {
            mark("/health");
            return getHealth();
        }
        if (head == "me" && method == "GET") {
            mark("/me");
            return getMe(request, ctx);
        }
        if (head == "users") {
            if (method == "GET") {
                mark("/users");
                return getUsers(request, ctx);
            }
            if (method == "POST") {
                mark("/users");
                return postUsers(request, ctx);
            }
        }
        if (head == "projects") {
            if (method == "GET") {
                mark("/projects");
                return getProjects(request, ctx);
            }
            if (method == "POST") {
                mark("/projects");
                return postProjects(request, ctx);
            }
        }
        if (head == "tasks" && method == "GET") {
            mark("/tasks");
            return getTasks(request, ctx);
        }
        if (head == "search" && method == "GET") {
            mark("/search");
            return getSearch(request, ctx);
        }
        if (head == "audit" && method == "GET") {
            mark("/audit");
            return getAudit(request, ctx);
        }
        if (head == "outbox" && method == "GET") {
            mark("/outbox");
            return getOutbox(request, ctx);
        }
        if (head == "metrics" && method == "GET") {
            mark("/metrics");
            return getMetrics(request, ctx);
        }
        if (head == "stats" && method == "GET") {
            mark("/stats");
            return getStats(request, ctx);
        }
    }
    if (parts.size() == 2) {
        const std::string &head = parts[0];
        const std::string &tail = parts[1];
        if (head == "auth" && tail == "login" && method == "POST") {
            mark("/auth/login");
            return postLogin(request);
        }
        if (head == "auth" && tail == "logout" && method == "POST") {
            mark("/auth/logout");
            return postLogout(request, ctx);
        }
        if (head == "reports" && tail == "workload" && method == "GET") {
            mark("/reports/workload");
            return getWorkload(request, ctx);
        }
        if (head == "outbox" && tail == "flush" && method == "POST") {
            mark("/outbox/flush");
            return postOutboxFlush(request, ctx);
        }
        if (head == "tasks" && tail == "bulk" && method == "POST") {
            mark("/tasks/bulk");
            return postTasksBulk(request, ctx);
        }
        if (head == "users") {
            if (method == "GET") {
                mark("/users/{id}");
                return getUserById(request, ctx, tail);
            }
            if (method == "PATCH") {
                mark("/users/{id}");
                return patchUserById(request, ctx, tail);
            }
            if (method == "DELETE") {
                mark("/users/{id}");
                return deleteUserById(request, ctx, tail);
            }
        }
        if (head == "projects") {
            if (method == "GET") {
                mark("/projects/{id}");
                return getProjectById(request, ctx, tail);
            }
            if (method == "PATCH") {
                mark("/projects/{id}");
                return patchProjectById(request, ctx, tail);
            }
            if (method == "DELETE") {
                mark("/projects/{id}");
                return deleteProjectById(request, ctx, tail);
            }
        }
        if (head == "tasks") {
            if (method == "GET") {
                mark("/tasks/{id}");
                return getTaskById(request, ctx, tail);
            }
            if (method == "PUT") {
                mark("/tasks/{id}");
                return putTaskById(request, ctx, tail);
            }
            if (method == "DELETE") {
                mark("/tasks/{id}");
                return deleteTaskById(request, ctx, tail);
            }
        }
        if (head == "comments" && method == "DELETE") {
            mark("/comments/{id}");
            return deleteCommentById(request, ctx, tail);
        }
    }
    if (parts.size() == 3) {
        const std::string &head = parts[0];
        const std::string &raw = parts[1];
        const std::string &leaf = parts[2];
        if (head == "projects" && leaf == "tasks") {
            if (method == "GET") {
                mark("/projects/{id}/tasks");
                return getProjectTasks(request, ctx, raw);
            }
            if (method == "POST") {
                mark("/projects/{id}/tasks");
                return postProjectTasks(request, ctx, raw);
            }
        }
        if (head == "projects" && leaf == "restore" && method == "POST") {
            mark("/projects/{id}/restore");
            return postProjectRestore(request, ctx, raw);
        }
        if (head == "tasks" && leaf == "status" && method == "PATCH") {
            mark("/tasks/{id}/status");
            return patchTaskStatus(request, ctx, raw);
        }
        if (head == "tasks" && leaf == "restore" && method == "POST") {
            mark("/tasks/{id}/restore");
            return postTaskRestore(request, ctx, raw);
        }
        if (head == "tasks" && leaf == "comments") {
            if (method == "GET") {
                mark("/tasks/{id}/comments");
                return getTaskComments(request, ctx, raw);
            }
            if (method == "POST") {
                mark("/tasks/{id}/comments");
                return postTaskComments(request, ctx, raw);
            }
        }
    }
    mark("/{path}");
    throw notFound();
}

Reply handleRequest(const Request &request, Ctx &ctx, std::string &pattern)
{
    try {
        return dispatch(request, ctx, pattern);
    } catch (const AppError &error) {
        return {error.status, envelope(error, ctx.requestId), ""};
    }
}

// -------------------------------------------------------------------- the transport

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

std::string_view reasonOf(int status)
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

void sendAll(SOCKET client, const std::string &data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int wrote = send(client, data.data() + sent,
                               static_cast<int>(data.size() - sent), 0);
        if (wrote <= 0) return;
        sent += static_cast<std::size_t>(wrote);
    }
}

void sendResponse(SOCKET client, const Reply &reply, const Ctx &ctx)
{
    std::string response = std::format("HTTP/1.1 {} {}\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: {}\r\n"
                                       "X-Request-Id: {}\r\n",
                                       reply.status, reasonOf(reply.status), reply.body.size(),
                                       ctx.requestId);
    if (!reply.etag.empty()) response += std::format("ETag: {}\r\n", reply.etag);
    if (ctx.quotaRemaining) {
        response += std::format("X-Quota-Remaining: {}\r\n", *ctx.quotaRemaining);
    }
    if (ctx.replayed) response += "Idempotency-Replayed: true\r\n";
    response += ctx.keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    response += reply.body;
    sendAll(client, response);
}

// Answer one request. A request line that will not parse leaves the reply unsent.
bool observe(SOCKET client, const std::string &raw, bool keepAlive)
{
    const auto parsed = parseRequest(raw);
    if (!parsed) return false;
    const Request &request = *parsed;
    Ctx ctx;
    ctx.requestId = request.requestId;
    ctx.keepAlive = keepAlive;
    const auto started = std::chrono::steady_clock::now();
    const std::size_t before = audit.size();
    std::string pattern;
    const Reply reply = handleRequest(request, ctx, pattern);
    sendResponse(client, reply, ctx);
    countRequest(pattern, reply.status);
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const std::string_view level =
        reply.status >= 500 ? "error" : reply.status >= 400 ? "warn" : "info";
    std::cout << std::format(
        R"({{"level":"{}","requestId":{},"method":{},"path":{},"status":{},"durationMs":{},)"
        R"("userId":{},"quotaRemaining":{},"auditSeq":{}}})",
        level, writeJsonString(request.requestId), writeJsonString(request.method),
        writeJsonString(request.path), reply.status, durationMs,
        ctx.userId ? std::to_string(*ctx.userId) : std::string{"null"},
        ctx.quotaRemaining ? std::to_string(*ctx.quotaRemaining) : std::string{"null"},
        audit.size() - before)
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
    char chunk[16384];
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
    seed();
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
