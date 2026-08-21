// Task Service, large tier — business rules, authorization, audit and serializers.

#pragma once

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "domain.hpp"
#include "store.hpp"

std::string serializeUser(const User &user);
std::string serializeProject(const Project &project);
std::string serializeTask(const Task &task, const std::string &role);
std::string serializeComment(const Comment &comment);
std::string serializeAudit(const AuditEntry &entry);
std::string serializeOutbox(const OutboxEvent &event);

// The authenticated caller and the session that pays for the request.
struct Caller {
    User *user{};
    Session *session{};
};

Caller authenticate(const std::string &header);
int chargeQuota(User &user, Session &session);
void requireAdmin(const User &user);
Project &reachableProject(int projectId, const User &user, bool includeDeleted = false);
Task &reachableTask(int taskId, const User &user, bool includeDeleted = false);
void checkIfMatch(const std::optional<std::string> &value, int version);
bool checkIncludeDeleted(const std::optional<std::string> &raw, const User &user);

bool lessUser(const User &a, const User &b, const std::string &sort);
bool lessProject(const Project &a, const Project &b, const std::string &sort);
bool lessTask(const Task &a, const Task &b, const std::string &sort);
bool lessComment(const Comment &a, const Comment &b, const std::string &sort);
bool lessAudit(const AuditEntry &a, const AuditEntry &b, const std::string &sort);
bool lessOutbox(const OutboxEvent &a, const OutboxEvent &b, const std::string &sort);

// Sort by the requested field, break every tie by the ascending row key, then window.
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
        return rowKey(*a) < rowKey(*b);
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

const User &login(const std::string &username, const std::string &password,
                  const std::string &token);

Project &createProject(const User &actor, const std::string &name, std::optional<int> ownerId);
void renameProject(const User &actor, Project &project, const std::string &name);
void deleteProject(const User &actor, Project &project);
void restoreProject(const User &actor, Project &project);

std::string readNote(const User &actor, const Json &body, Details &errors,
                     const std::string &current);
Task &createTask(const User &actor, const Project &project, const std::string &title,
                 std::optional<int> priority, std::optional<int> assigneeId,
                 const std::string &note, Details &errors);
void replaceTask(const User &actor, Task &task, const std::string &title,
                 std::optional<int> priority, std::optional<int> assigneeId,
                 const std::string &note, Details &errors);
void moveStatus(const User &actor, Task &task, const Json *status);
void deleteTask(const User &actor, Task &task);
void restoreTask(const User &actor, Task &task);

Comment &createComment(const User &actor, const Task &task, const std::string &body);
void removeComment(const User &actor, const Comment &comment);

User &createUser(const User &actor, const std::string &username, const std::string &password,
                 const Json *role, const Json *quota);
void updateUser(const User &actor, User &user, const Json &body);
void deleteUser(const User &actor, User &user);

std::vector<const Project *> visibleProjects(const User &user, bool includeDeleted);
std::vector<const Task *> visibleTasks(const User &user, bool includeDeleted);
std::string search(const User &user, const std::string &query);
std::string workload(const User &user, const std::string &groupBy);
int flushOutbox();
std::string metrics();
std::string stats();
void checkBulkSize(const Json *operations);
