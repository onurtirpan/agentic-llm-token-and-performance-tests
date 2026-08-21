// Task Service, large tier — the domain rules, the JSON reader and the JSON writer.

#include "domain.hpp"

#include <algorithm>
#include <cstdlib>

int rowKey(const User &row)
{
    return row.id;
}

int rowKey(const Project &row)
{
    return row.id;
}

int rowKey(const Task &row)
{
    return row.id;
}

int rowKey(const Comment &row)
{
    return row.id;
}

int rowKey(const AuditEntry &row)
{
    return row.seq;
}

int rowKey(const OutboxEvent &row)
{
    return row.seq;
}

const Json *Json::find(std::string_view key) const
{
    for (std::size_t index = 0; index < keys.size() && index < items.size(); index += 1) {
        if (keys[index] == key) return &items[index];
    }
    return nullptr;
}

bool Json::contains(std::string_view key) const
{
    return find(key) != nullptr;
}

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

bool allowedMove(std::string_view from, std::string_view to)
{
    return std::ranges::any_of(
        TRANSITIONS, [&](const Move &move) { return move.first == from && move.second == to; });
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

std::string_view boolText(bool value)
{
    return value ? "true" : "false";
}

std::size_t skipSpace(std::string_view text, std::size_t at)
{
    while (at < text.size()
           && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        at += 1;
    }
    return at;
}

namespace {

std::optional<std::size_t> parseText(std::string_view text, std::size_t at, std::string &out)
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

std::optional<std::size_t> parseNumber(std::string_view text, std::size_t at, Json &out)
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

std::optional<std::size_t> parseValue(std::string_view text, std::size_t at, Json &out);

std::optional<std::size_t> parseObject(std::string_view text, std::size_t at, Json &out)
{
    out.kind = Json::Kind::Object;
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == '}') return at + 1;
    for (;;) {
        std::string key;
        auto next = parseText(text, at, key);
        if (!next) return std::nullopt;
        at = skipSpace(text, *next);
        if (at >= text.size() || text[at] != ':') return std::nullopt;
        Json value;
        next = parseValue(text, skipSpace(text, at + 1), value);
        if (!next) return std::nullopt;
        out.keys.push_back(std::move(key));
        out.items.push_back(std::move(value));
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

std::optional<std::size_t> parseArray(std::string_view text, std::size_t at, Json &out)
{
    out.kind = Json::Kind::Array;
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == ']') return at + 1;
    for (;;) {
        Json value;
        const auto next = parseValue(text, at, value);
        if (!next) return std::nullopt;
        out.items.push_back(std::move(value));
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

std::optional<std::size_t> parseValue(std::string_view text, std::size_t at, Json &out)
{
    if (at >= text.size()) return std::nullopt;
    switch (text[at]) {
    case '"':
        out.kind = Json::Kind::String;
        return parseText(text, at, out.text);
    case '{': return parseObject(text, at, out);
    case '[': return parseArray(text, at, out);
    case 't':
        if (!text.substr(at).starts_with("true")) return std::nullopt;
        out.kind = Json::Kind::Bool;
        out.boolean = true;
        return at + 4;
    case 'f':
        if (!text.substr(at).starts_with("false")) return std::nullopt;
        out.kind = Json::Kind::Bool;
        return at + 5;
    case 'n':
        if (!text.substr(at).starts_with("null")) return std::nullopt;
        out.kind = Json::Kind::Null;
        return at + 4;
    default: return parseNumber(text, at, out);
    }
}

}  // namespace

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

std::optional<Json> parseJson(std::string_view text)
{
    Json value;
    const auto next = parseValue(text, skipSpace(text, 0), value);
    if (!next || skipSpace(text, *next) != text.size()) return std::nullopt;
    return value;
}

AppError badRequest()
{
    return {400, "bad_request", "the request is malformed", {}};
}

AppError unauthorized()
{
    return {401, "unauthorized", "authentication is required", {}};
}

AppError invalidCredentials()
{
    return {401, "invalid_credentials", "the username or password is wrong", {}};
}

AppError forbidden()
{
    return {403, "forbidden", "you may not access this resource", {}};
}

AppError notFound()
{
    return {404, "not_found", "the resource does not exist", {}};
}

AppError conflict()
{
    return {409, "conflict", "the resource already exists", {}};
}

AppError invalidTransition()
{
    return {409, "invalid_transition", "the status change is not allowed", {}};
}

AppError preconditionFailed()
{
    return {412, "precondition_failed", "the resource has changed", {}};
}

AppError preconditionRequired()
{
    return {428, "precondition_required", "the If-Match header is required", {}};
}

AppError quotaExceeded()
{
    return {429, "quota_exceeded", "the request quota is exhausted", {}};
}

AppError invalid(Details details)
{
    std::ranges::sort(details, [](const Detail &a, const Detail &b) {
        return a.field != b.field ? a.field < b.field : a.message < b.message;
    });
    return {422, "validation_failed", "the request body is not valid", std::move(details)};
}

Detail fail(std::string field, std::string message)
{
    return {std::move(field), std::move(message)};
}

void checkString(const std::string &value, const std::string &field, std::size_t maxLength,
                 Details &errors)
{
    if (value.empty()) {
        errors.push_back(fail(field, field + " is required"));
    } else if (value.size() > maxLength) {
        errors.push_back(fail(field, field + " is too long"));
    }
}

void checkPriority(std::optional<int> value, Details &errors)
{
    if (!value || *value < MIN_PRIORITY || *value > MAX_PRIORITY) {
        errors.push_back(fail("priority", "priority is out of range"));
    }
}

void checkStatus(const Json *value, Details &errors)
{
    if (value == nullptr || value->kind != Json::Kind::String || !statusBonus(value->text)) {
        errors.push_back(fail("status", "status is not valid"));
    }
}

void checkRole(const Json *value, Details &errors)
{
    if (value == nullptr) return;
    if (value->kind != Json::Kind::String || std::ranges::find(ROLES, value->text) == ROLES.end()) {
        errors.push_back(fail("role", "role is not valid"));
    }
}

void checkQuota(const Json *value, Details &errors)
{
    if (value == nullptr) return;
    if (value->kind != Json::Kind::Number || !value->integral || value->number < 0.0) {
        errors.push_back(fail("quota", "quota is out of range"));
    }
}
