// Task Service, large tier — domain types, constants, errors and pure rules.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr std::size_t MAX_TITLE_LENGTH = 80;
constexpr std::size_t MAX_NAME_LENGTH = 60;
constexpr std::size_t MAX_COMMENT_LENGTH = 200;
constexpr std::size_t MAX_BULK_ITEMS = 20;
constexpr int MIN_PRIORITY = 1;
constexpr int MAX_PRIORITY = 5;
constexpr int DEFAULT_LIMIT = 20;
constexpr int MAX_LIMIT = 100;
constexpr int DEFAULT_QUOTA = 10000;
constexpr int PROBE_QUOTA = 5;
constexpr unsigned short PORT = 8080;

struct StatusBonus {
    std::string_view name;
    int bonus;
};

constexpr std::array<StatusBonus, 4> STATUS_BONUS{
    {{"todo", 0}, {"in_progress", 3}, {"done", 5}, {"archived", 0}}};

using Move = std::pair<std::string_view, std::string_view>;

constexpr std::array<Move, 5> TRANSITIONS{{{"todo", "in_progress"},
                                           {"todo", "archived"},
                                           {"in_progress", "todo"},
                                           {"in_progress", "done"},
                                           {"done", "archived"}}};

constexpr std::array<std::string_view, 2> ROLES{"admin", "user"};
constexpr std::array<std::string_view, 3> PROJECT_SORTS{"id", "name", "taskCount"};
constexpr std::array<std::string_view, 5> TASK_SORTS{"id", "title", "priority", "score", "status"};
constexpr std::array<std::string_view, 3> USER_SORTS{"id", "username", "role"};
constexpr std::array<std::string_view, 2> COMMENT_SORTS{"id", "authorId"};
constexpr std::array<std::string_view, 1> SEQ_SORTS{"seq"};
constexpr std::array<std::string_view, 3> GROUP_BYS{"assignee", "status", "project"};

struct User {
    int id{};
    std::string username;
    std::string password;
    std::string role;
    int quota{};
    int version = 1;
    bool deleted = false;
};

struct Session {
    std::string token;
    int userId{};
    int used = 0;
};

struct Project {
    int id{};
    std::string name;
    int ownerId{};
    int version = 1;
    bool deleted = false;
};

struct Task {
    int id{};
    int projectId{};
    std::string title;
    int priority{};
    std::string status;
    std::optional<int> assigneeId;
    std::string internalNote;
    int version = 1;
    bool deleted = false;
};

struct Comment {
    int id{};
    int taskId{};
    int authorId{};
    std::string body;
};

struct AuditEntry {
    int seq{};
    int actorId{};
    std::string action;
    std::string resource;
    int resourceId{};
};

struct OutboxEvent {
    int seq{};
    std::string name;
    int resourceId{};
    bool delivered = false;
};

// The pagination tiebreak. Audit and outbox rows key on seq, everything else on id.
int rowKey(const User &row);
int rowKey(const Project &row);
int rowKey(const Task &row);
int rowKey(const Comment &row);
int rowKey(const AuditEntry &row);
int rowKey(const OutboxEvent &row);

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

// A parsed JSON value. An object keeps its member names in keys, parallel to items.
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    bool integral = false;
    double number = 0.0;
    std::string text;
    std::vector<std::string> keys;
    std::vector<Json> items;

    [[nodiscard]] const Json *find(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;
};

using Query = std::map<std::string, std::string>;

struct Page {
    int limit = DEFAULT_LIMIT;
    int offset = 0;
    std::string sort;
    std::string order = "asc";
};

std::optional<int> statusBonus(std::string_view status);
int computeScore(int priority, std::string_view status);
bool allowedMove(std::string_view from, std::string_view to);
std::string lower(std::string_view text);
std::string_view boolText(bool value);
std::size_t skipSpace(std::string_view text, std::size_t at);
std::string writeJsonString(std::string_view text);
std::optional<Json> parseJson(std::string_view text);

AppError badRequest();
AppError unauthorized();
AppError invalidCredentials();
AppError forbidden();
AppError notFound();
AppError conflict();
AppError invalidTransition();
AppError preconditionFailed();
AppError preconditionRequired();
AppError quotaExceeded();
AppError invalid(Details details);
Detail fail(std::string field, std::string message);

void checkString(const std::string &value, const std::string &field, std::size_t maxLength,
                 Details &errors);
void checkPriority(std::optional<int> value, Details &errors);
void checkStatus(const Json *value, Details &errors);
void checkRole(const Json *value, Details &errors);
void checkQuota(const Json *value, Details &errors);
