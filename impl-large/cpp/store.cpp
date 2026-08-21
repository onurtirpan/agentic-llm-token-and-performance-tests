// Task Service, large tier — the single home of every mutable value.

#include "store.hpp"

#include <format>

std::map<int, User> users;
std::map<std::string, Session> sessions;
std::map<int, Project> projects;
std::map<int, Task> tasks;
std::map<int, Comment> comments;
std::vector<AuditEntry> audit;
std::vector<OutboxEvent> outbox;
std::map<Slot, Recorded> idempotency;
std::map<int, int> byStatus;
std::map<std::string, int> byRoute;

int requests = 0;
int nextProjectId = 1;
int nextTaskId = 1;
int nextCommentId = 1;
int nextUserId = 5;
int nextSeq = 1;

void seed()
{
    users.emplace(1, User{1, "admin", "admin-secret", "admin", DEFAULT_QUOTA, 1, false});
    users.emplace(2, User{2, "alice", "alice-secret", "user", DEFAULT_QUOTA, 1, false});
    users.emplace(3, User{3, "bob", "bob-secret", "user", DEFAULT_QUOTA, 1, false});
    users.emplace(4, User{4, "probe", "probe-secret", "user", PROBE_QUOTA, 1, false});
}

int takeSeq()
{
    const int value = nextSeq;
    nextSeq += 1;
    return value;
}

void record(int actorId, std::string_view action, std::string_view resource, int resourceId)
{
    audit.push_back({takeSeq(), actorId, std::string{action}, std::string{resource}, resourceId});
    outbox.push_back({takeSeq(), std::format("{}.{}", resource, action), resourceId, false});
}

void countRequest(const std::string &route, int status)
{
    requests += 1;
    byRoute[route] += 1;
    byStatus[status] += 1;
}

User *findUser(int userId, bool includeDeleted)
{
    const auto found = users.find(userId);
    if (found == users.end() || (found->second.deleted && !includeDeleted)) return nullptr;
    return &found->second;
}

User *findByUsername(const std::string &username)
{
    for (auto &[id, user] : users) {
        if (user.username == username && !user.deleted) return &user;
    }
    return nullptr;
}

User &insertUser(const std::string &username, const std::string &password, const std::string &role,
                 int quota)
{
    const int id = nextUserId;
    nextUserId += 1;
    return users.emplace(id, User{id, username, password, role, quota, 1, false}).first->second;
}

Project *findProject(int projectId, bool includeDeleted)
{
    const auto found = projects.find(projectId);
    if (found == projects.end() || (found->second.deleted && !includeDeleted)) return nullptr;
    return &found->second;
}

Project &insertProject(const std::string &name, int ownerId)
{
    const int id = nextProjectId;
    nextProjectId += 1;
    return projects.emplace(id, Project{id, name, ownerId, 1, false}).first->second;
}

Task *findTask(int taskId, bool includeDeleted)
{
    const auto found = tasks.find(taskId);
    if (found == tasks.end() || (found->second.deleted && !includeDeleted)) return nullptr;
    return &found->second;
}

Task &insertTask(int projectId, const std::string &title, int priority,
                 std::optional<int> assigneeId, const std::string &internalNote)
{
    const int id = nextTaskId;
    nextTaskId += 1;
    return tasks
        .emplace(id, Task{id, projectId, title, priority, "todo", assigneeId, internalNote, 1,
                          false})
        .first->second;
}

Comment *findComment(int commentId)
{
    const auto found = comments.find(commentId);
    return found == comments.end() ? nullptr : &found->second;
}

Comment &insertComment(int taskId, int authorId, const std::string &body)
{
    const int id = nextCommentId;
    nextCommentId += 1;
    return comments.emplace(id, Comment{id, taskId, authorId, body}).first->second;
}

std::vector<Task *> liveTasksOf(int projectId)
{
    std::vector<Task *> rows;
    for (auto &[id, task] : tasks) {
        if (task.projectId == projectId && !task.deleted) rows.push_back(&task);
    }
    return rows;
}

int taskCount(int projectId)
{
    return static_cast<int>(liveTasksOf(projectId).size());
}

int liveProjects()
{
    int total = 0;
    for (const auto &[id, project] : projects) {
        if (!project.deleted) total += 1;
    }
    return total;
}

int liveTasks()
{
    int total = 0;
    for (const auto &[id, task] : tasks) {
        if (!task.deleted) total += 1;
    }
    return total;
}

int liveUsers()
{
    int total = 0;
    for (const auto &[id, user] : users) {
        if (!user.deleted) total += 1;
    }
    return total;
}

int outboxPending()
{
    int total = 0;
    for (const OutboxEvent &event : outbox) {
        if (!event.delivered) total += 1;
    }
    return total;
}
