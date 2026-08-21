// Task Service, large tier — every rule that decides, mutates and audits.

#include "service.hpp"

#include <cmath>
#include <set>

std::string serializeUser(const User &user)
{
    return std::format(R"({{"id":{},"username":{},"role":{},"quota":{},"version":{},)"
                       R"("deleted":{}}})",
                       user.id, writeJsonString(user.username), writeJsonString(user.role),
                       user.quota, user.version, boolText(user.deleted));
}

std::string serializeProject(const Project &project)
{
    return std::format(R"({{"id":{},"name":{},"ownerId":{},"taskCount":{},"version":{},)"
                       R"("deleted":{}}})",
                       project.id, writeJsonString(project.name), project.ownerId,
                       taskCount(project.id), project.version, boolText(project.deleted));
}

std::string serializeTask(const Task &task, const std::string &role)
{
    const std::string note =
        role == "admin" ? std::format(R"("internalNote":{},)", writeJsonString(task.internalNote))
                        : std::string{};
    return std::format(R"({{"id":{},"projectId":{},"title":{},"priority":{},"status":{},)"
                       R"("assigneeId":{},{}"version":{},"deleted":{},"score":{}}})",
                       task.id, task.projectId, writeJsonString(task.title), task.priority,
                       writeJsonString(task.status),
                       task.assigneeId ? std::to_string(*task.assigneeId) : std::string{"null"},
                       note, task.version, boolText(task.deleted),
                       computeScore(task.priority, task.status));
}

std::string serializeComment(const Comment &comment)
{
    return std::format(R"({{"id":{},"taskId":{},"authorId":{},"body":{}}})", comment.id,
                       comment.taskId, comment.authorId, writeJsonString(comment.body));
}

std::string serializeAudit(const AuditEntry &entry)
{
    return std::format(R"({{"seq":{},"actorId":{},"action":{},"resource":{},"resourceId":{}}})",
                       entry.seq, entry.actorId, writeJsonString(entry.action),
                       writeJsonString(entry.resource), entry.resourceId);
}

std::string serializeOutbox(const OutboxEvent &event)
{
    return std::format(R"({{"seq":{},"name":{},"resourceId":{},"delivered":{}}})", event.seq,
                       writeJsonString(event.name), event.resourceId, boolText(event.delivered));
}

Caller authenticate(const std::string &header)
{
    if (!header.starts_with("Bearer ")) throw unauthorized();
    const auto found = sessions.find(header.substr(7));
    if (found == sessions.end()) throw unauthorized();
    User *user = findUser(found->second.userId);
    if (user == nullptr) throw unauthorized();
    return {user, &found->second};
}

int chargeQuota(User &user, Session &session)
{
    if (session.used >= user.quota) throw quotaExceeded();
    session.used += 1;
    return std::max(user.quota - session.used, 0);
}

void requireAdmin(const User &user)
{
    if (user.role != "admin") throw forbidden();
}

Project &reachableProject(int projectId, const User &user, bool includeDeleted)
{
    Project *project = findProject(projectId, includeDeleted);
    if (project == nullptr) throw notFound();
    if (user.role != "admin" && project->ownerId != user.id) throw forbidden();
    return *project;
}

Task &reachableTask(int taskId, const User &user, bool includeDeleted)
{
    Task *task = findTask(taskId, includeDeleted);
    if (task == nullptr) throw notFound();
    reachableProject(task->projectId, user, true);
    return *task;
}

void checkIfMatch(const std::optional<std::string> &value, int version)
{
    if (!value || value->empty()) throw preconditionRequired();
    if (*value != std::to_string(version)) throw preconditionFailed();
}

bool checkIncludeDeleted(const std::optional<std::string> &raw, const User &user)
{
    if (!raw) return false;
    if (user.role != "admin") throw forbidden();
    return *raw == "true";
}

bool lessUser(const User &a, const User &b, const std::string &sort)
{
    if (sort == "username") return a.username < b.username;
    if (sort == "role") return a.role < b.role;
    return a.id < b.id;
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
    if (sort == "score") {
        return computeScore(a.priority, a.status) < computeScore(b.priority, b.status);
    }
    if (sort == "status") return a.status < b.status;
    return a.id < b.id;
}

bool lessComment(const Comment &a, const Comment &b, const std::string &sort)
{
    if (sort == "authorId") return a.authorId < b.authorId;
    return a.id < b.id;
}

bool lessAudit(const AuditEntry &a, const AuditEntry &b, const std::string &)
{
    return a.seq < b.seq;
}

bool lessOutbox(const OutboxEvent &a, const OutboxEvent &b, const std::string &)
{
    return a.seq < b.seq;
}

const User &login(const std::string &username, const std::string &password,
                  const std::string &token)
{
    const User *user = findByUsername(username);
    if (user == nullptr || user->password != password) throw invalidCredentials();
    sessions.emplace(token, Session{token, user->id, 0});
    return *user;
}

Project &createProject(const User &actor, const std::string &name, std::optional<int> ownerId)
{
    Details errors;
    checkString(name, "name", MAX_NAME_LENGTH, errors);
    if (!ownerId || findUser(*ownerId) == nullptr) {
        errors.push_back(fail("ownerId", "ownerId is not a known user"));
    }
    if (!errors.empty()) throw invalid(std::move(errors));
    for (const auto &[id, other] : projects) {
        if (other.ownerId == *ownerId && other.name == name && !other.deleted) throw conflict();
    }
    Project &project = insertProject(name, *ownerId);
    record(actor.id, "create", "project", project.id);
    return project;
}

void renameProject(const User &actor, Project &project, const std::string &name)
{
    Details errors;
    checkString(name, "name", MAX_NAME_LENGTH, errors);
    if (!errors.empty()) throw invalid(std::move(errors));
    for (const auto &[id, other] : projects) {
        if (other.ownerId == project.ownerId && other.name == name && other.id != project.id
            && !other.deleted) {
            throw conflict();
        }
    }
    project.name = name;
    project.version += 1;
    record(actor.id, "update", "project", project.id);
}

void deleteProject(const User &actor, Project &project)
{
    project.deleted = true;
    project.version += 1;
    record(actor.id, "delete", "project", project.id);
    for (Task *task : liveTasksOf(project.id)) {
        task->deleted = true;
        task->version += 1;
        record(actor.id, "delete", "task", task->id);
    }
}

void restoreProject(const User &actor, Project &project)
{
    if (!project.deleted) throw conflict();
    project.deleted = false;
    project.version += 1;
    record(actor.id, "restore", "project", project.id);
}

std::string readNote(const User &actor, const Json &body, Details &errors,
                     const std::string &current)
{
    const Json *note = body.find("internalNote");
    if (note == nullptr) return current;
    if (actor.role != "admin") throw forbidden();
    if (note->kind != Json::Kind::String) throw badRequest();
    if (note->text.size() > MAX_TITLE_LENGTH) {
        errors.push_back(fail("internalNote", "internalNote is too long"));
    }
    return note->text;
}

namespace {

void checkTaskFields(const std::string &title, std::optional<int> priority,
                     std::optional<int> assigneeId, Details &errors)
{
    checkString(title, "title", MAX_TITLE_LENGTH, errors);
    checkPriority(priority, errors);
    if (assigneeId && findUser(*assigneeId) == nullptr) {
        errors.push_back(fail("assigneeId", "assigneeId is not a known user"));
    }
    if (!errors.empty()) throw invalid(std::move(errors));
}

}  // namespace

Task &createTask(const User &actor, const Project &project, const std::string &title,
                 std::optional<int> priority, std::optional<int> assigneeId,
                 const std::string &note, Details &errors)
{
    checkTaskFields(title, priority, assigneeId, errors);
    Task &task = insertTask(project.id, title, *priority, assigneeId, note);
    record(actor.id, "create", "task", task.id);
    return task;
}

void replaceTask(const User &actor, Task &task, const std::string &title,
                 std::optional<int> priority, std::optional<int> assigneeId,
                 const std::string &note, Details &errors)
{
    checkTaskFields(title, priority, assigneeId, errors);
    task.title = title;
    task.priority = *priority;
    task.assigneeId = assigneeId;
    task.internalNote = note;
    task.version += 1;
    record(actor.id, "update", "task", task.id);
}

void moveStatus(const User &actor, Task &task, const Json *status)
{
    Details errors;
    checkStatus(status, errors);
    if (!errors.empty()) throw invalid(std::move(errors));
    if (!allowedMove(task.status, status->text)) throw invalidTransition();
    task.status = status->text;
    task.version += 1;
    record(actor.id, "update", "task", task.id);
}

void deleteTask(const User &actor, Task &task)
{
    task.deleted = true;
    task.version += 1;
    record(actor.id, "delete", "task", task.id);
}

void restoreTask(const User &actor, Task &task)
{
    if (!task.deleted) throw conflict();
    task.deleted = false;
    task.version += 1;
    record(actor.id, "restore", "task", task.id);
}

Comment &createComment(const User &actor, const Task &task, const std::string &body)
{
    Details errors;
    checkString(body, "body", MAX_COMMENT_LENGTH, errors);
    if (!errors.empty()) throw invalid(std::move(errors));
    Comment &comment = insertComment(task.id, actor.id, body);
    record(actor.id, "create", "comment", comment.id);
    return comment;
}

void removeComment(const User &actor, const Comment &comment)
{
    if (actor.role != "admin" && comment.authorId != actor.id) throw forbidden();
    const int commentId = comment.id;
    comments.erase(commentId);
    record(actor.id, "delete", "comment", commentId);
}

User &createUser(const User &actor, const std::string &username, const std::string &password,
                 const Json *role, const Json *quota)
{
    Details errors;
    checkString(username, "username", MAX_NAME_LENGTH, errors);
    checkString(password, "password", MAX_NAME_LENGTH, errors);
    checkRole(role, errors);
    checkQuota(quota, errors);
    if (!errors.empty()) throw invalid(std::move(errors));
    if (findByUsername(username) != nullptr) throw conflict();
    User &user = insertUser(username, password, role == nullptr ? "user" : role->text,
                            quota == nullptr ? DEFAULT_QUOTA : static_cast<int>(quota->number));
    record(actor.id, "create", "user", user.id);
    return user;
}

void updateUser(const User &actor, User &user, const Json &body)
{
    Details errors;
    const Json *role = body.find("role");
    const Json *quota = body.find("quota");
    checkRole(role, errors);
    checkQuota(quota, errors);
    if (!errors.empty()) throw invalid(std::move(errors));
    if (role != nullptr) user.role = role->text;
    if (quota != nullptr) user.quota = static_cast<int>(quota->number);
    user.version += 1;
    record(actor.id, "update", "user", user.id);
}

void deleteUser(const User &actor, User &user)
{
    if (user.id == actor.id) throw conflict();
    user.deleted = true;
    user.version += 1;
    record(actor.id, "delete", "user", user.id);
}

std::vector<const Project *> visibleProjects(const User &user, bool includeDeleted)
{
    std::vector<const Project *> rows;
    for (const auto &[id, project] : projects) {
        if ((includeDeleted || !project.deleted)
            && (user.role == "admin" || project.ownerId == user.id)) {
            rows.push_back(&project);
        }
    }
    return rows;
}

std::vector<const Task *> visibleTasks(const User &user, bool includeDeleted)
{
    std::set<int> allowed;
    for (const Project *project : visibleProjects(user, true)) allowed.insert(project->id);
    std::vector<const Task *> rows;
    for (const auto &[id, task] : tasks) {
        if (allowed.contains(task.projectId) && (includeDeleted || !task.deleted)) {
            rows.push_back(&task);
        }
    }
    return rows;
}

std::string search(const User &user, const std::string &query)
{
    const std::string needle = lower(query);
    std::string results;
    int total = 0;
    const auto emit = [&](std::string_view type, int id, const std::string &label) {
        if (!results.empty()) results += ',';
        results += std::format(R"({{"type":"{}","id":{},"label":{}}})", type, id,
                               writeJsonString(label));
        total += 1;
    };
    for (const Project *project : visibleProjects(user, false)) {
        if (lower(project->name).find(needle) != std::string::npos) {
            emit("project", project->id, project->name);
        }
    }
    for (const Task *task : visibleTasks(user, false)) {
        if (lower(task->title).find(needle) != std::string::npos) {
            emit("task", task->id, task->title);
        }
    }
    return std::format(R"({{"results":[{}],"total":{}}})", results, total);
}

std::string workload(const User &user, const std::string &groupBy)
{
    const std::vector<const Task *> rows = visibleTasks(user, false);
    std::string groups;
    const auto emit = [&groups](std::string_view key, const std::vector<const Task *> &picked) {
        int totalScore = 0;
        for (const Task *task : picked) totalScore += computeScore(task->priority, task->status);
        if (!groups.empty()) groups += ',';
        groups += std::format(R"({{"key":{},"tasks":{},"totalScore":{}}})", writeJsonString(key),
                              picked.size(), totalScore);
    };
    const auto pick = [&rows](auto keep) {
        std::vector<const Task *> picked;
        for (const Task *task : rows) {
            if (keep(task)) picked.push_back(task);
        }
        return picked;
    };
    if (groupBy == "status") {
        for (const auto &entry : STATUS_BONUS) {
            emit(entry.name, pick([&entry](const Task *task) { return task->status == entry.name; }));
        }
    } else if (groupBy == "assignee") {
        std::set<int> named;
        for (const Task *task : rows) {
            if (task->assigneeId) named.insert(*task->assigneeId);
        }
        for (const int assignee : named) {
            emit(std::to_string(assignee),
                 pick([assignee](const Task *task) { return task->assigneeId == assignee; }));
        }
        const std::vector<const Task *> loose =
            pick([](const Task *task) { return !task->assigneeId; });
        if (!loose.empty()) emit("unassigned", loose);
    } else {
        for (const Project *project : visibleProjects(user, false)) {
            emit(project->name,
                 pick([project](const Task *task) { return task->projectId == project->id; }));
        }
    }
    return std::format(R"({{"groupBy":{},"groups":[{}]}})", writeJsonString(groupBy), groups);
}

int flushOutbox()
{
    int flushed = 0;
    for (OutboxEvent &event : outbox) {
        if (!event.delivered) {
            event.delivered = true;
            flushed += 1;
        }
    }
    return flushed;
}

std::string metrics()
{
    std::string statusPart;
    for (const auto &[code, count] : byStatus) {
        if (!statusPart.empty()) statusPart += ',';
        statusPart += std::format(R"("{}":{})", code, count);
    }
    std::string routePart;
    for (const auto &[route, count] : byRoute) {
        if (!routePart.empty()) routePart += ',';
        routePart += std::format(R"({{"route":{},"count":{}}})", writeJsonString(route), count);
    }
    return std::format(R"({{"requests":{},"byStatus":{{{}}},"byRoute":[{}],"auditEntries":{},)"
                       R"("outboxPending":{}}})",
                       requests, statusPart, routePart, audit.size(), outboxPending());
}

std::string stats()
{
    std::string statusPart;
    for (const auto &entry : STATUS_BONUS) {
        int count = 0;
        for (const auto &[id, task] : tasks) {
            if (!task.deleted && task.status == entry.name) count += 1;
        }
        if (!statusPart.empty()) statusPart += ',';
        statusPart += std::format(R"("{}":{})", entry.name, count);
    }
    int total = 0;
    int sumScore = 0;
    for (const auto &[id, task] : tasks) {
        if (task.deleted) continue;
        total += 1;
        sumScore += computeScore(task.priority, task.status);
    }
    const double avgScore =
        total == 0 ? 0.0
                   : std::round(static_cast<double>(sumScore) / static_cast<double>(total) * 100.0)
                         / 100.0;
    const Project *best = nullptr;
    for (const auto &[id, project] : projects) {
        if (project.deleted) continue;
        if (best == nullptr || taskCount(project.id) > taskCount(best->id)) best = &project;
    }
    return std::format(R"({{"projects":{},"tasks":{},"users":{},"sessions":{},"comments":{},)"
                       R"("byStatus":{{{}}},"avgScore":{},"topProjectName":{},)"
                       R"("auditEntries":{},"outboxPending":{}}})",
                       liveProjects(), total, liveUsers(), sessions.size(), comments.size(),
                       statusPart, avgScore,
                       best == nullptr ? std::string{"null"} : writeJsonString(best->name),
                       audit.size(), outboxPending());
}

void checkBulkSize(const Json *operations)
{
    if (operations == nullptr || operations->kind != Json::Kind::Array
        || operations->items.empty() || operations->items.size() > MAX_BULK_ITEMS) {
        throw invalid({fail("operations", "operations is out of range")});
    }
}
