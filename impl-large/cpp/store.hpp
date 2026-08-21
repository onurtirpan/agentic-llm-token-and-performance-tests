// Task Service, large tier — the in-memory state and its repository functions.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain.hpp"

// One recorded outcome of an idempotent write, replayed byte for byte.
struct Recorded {
    int status{};
    std::string body;
    std::string etag;
};

using Slot = std::pair<std::string, std::string>;

extern std::map<int, User> users;
extern std::map<std::string, Session> sessions;
extern std::map<int, Project> projects;
extern std::map<int, Task> tasks;
extern std::map<int, Comment> comments;
extern std::vector<AuditEntry> audit;
extern std::vector<OutboxEvent> outbox;
extern std::map<Slot, Recorded> idempotency;
extern std::map<int, int> byStatus;
extern std::map<std::string, int> byRoute;

extern int requests;
extern int nextProjectId;
extern int nextTaskId;
extern int nextCommentId;
extern int nextUserId;
extern int nextSeq;

void seed();
int takeSeq();
void record(int actorId, std::string_view action, std::string_view resource, int resourceId);
void countRequest(const std::string &route, int status);

User *findUser(int userId, bool includeDeleted = false);
User *findByUsername(const std::string &username);
User &insertUser(const std::string &username, const std::string &password, const std::string &role,
                 int quota);

Project *findProject(int projectId, bool includeDeleted = false);
Project &insertProject(const std::string &name, int ownerId);

Task *findTask(int taskId, bool includeDeleted = false);
Task &insertTask(int projectId, const std::string &title, int priority,
                 std::optional<int> assigneeId, const std::string &internalNote);

Comment *findComment(int commentId);
Comment &insertComment(int taskId, int authorId, const std::string &body);

std::vector<Task *> liveTasksOf(int projectId);
int taskCount(int projectId);
int liveProjects();
int liveTasks();
int liveUsers();
int outboxPending();
