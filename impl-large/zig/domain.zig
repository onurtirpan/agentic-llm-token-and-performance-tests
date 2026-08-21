// Task Service, large tier — types, constants and pure rules.
const std = @import("std");
const http = std.http;
const Allocator = std.mem.Allocator;
const Value = std.json.Value;

pub const max_title_length = 80;
pub const max_name_length = 60;
pub const max_comment_length = 200;
pub const max_bulk_items = 20;
pub const min_priority = 1;
pub const max_priority = 5;
pub const default_limit = 20;
pub const max_limit = 100;
pub const default_quota = 10000;
pub const probe_quota = 5;
pub const port = 8080;

pub const roles = [_][]const u8{ "admin", "user" };
pub const statuses = [_][]const u8{ "todo", "in_progress", "done", "archived" };
pub const status_bonus = [_]i64{ 0, 3, 5, 0 };
pub const transitions = [_][2][]const u8{
    .{ "todo", "in_progress" }, .{ "todo", "archived" },  .{ "in_progress", "todo" },
    .{ "in_progress", "done" }, .{ "done", "archived" },
};
pub const project_sorts = [_][]const u8{ "id", "name", "taskCount" };
pub const task_sorts = [_][]const u8{ "id", "title", "priority", "score", "status" };
pub const user_sorts = [_][]const u8{ "id", "username", "role" };
pub const comment_sorts = [_][]const u8{ "id", "authorId" };
pub const seq_sorts = [_][]const u8{"seq"};
pub const group_bys = [_][]const u8{ "assignee", "status", "project" };

pub const User = struct {
    id: i64,
    username: []const u8,
    password: []const u8,
    role: []const u8,
    quota: i64,
    version: i64 = 1,
    deleted: bool = false,
};
pub const Session = struct { token: []const u8, userId: i64, used: i64 = 0 };
pub const Project = struct {
    id: i64,
    name: []const u8,
    ownerId: i64,
    version: i64 = 1,
    deleted: bool = false,
};
pub const Task = struct {
    id: i64,
    projectId: i64,
    title: []const u8,
    priority: i64,
    status: []const u8,
    assigneeId: ?i64,
    internalNote: []const u8 = "",
    version: i64 = 1,
    deleted: bool = false,
};
pub const Comment = struct { id: i64, taskId: i64, authorId: i64, body: []const u8 };
pub const AuditEntry = struct {
    seq: i64,
    actorId: i64,
    action: []const u8,
    resource: []const u8,
    resourceId: i64,
};
pub const OutboxEvent = struct {
    seq: i64,
    name: []const u8,
    resourceId: i64,
    delivered: bool = false,
};

pub const Detail = struct { field: []const u8, message: []const u8 };
pub const AppError = struct {
    status: http.Status,
    code: []const u8,
    message: []const u8,
    details: []const Detail,
};
pub const Error = error{App};

/// An error carries no payload in Zig, so the envelope waits here.
pub var app_error: AppError = undefined;

fn appError(
    status: http.Status,
    code: []const u8,
    message: []const u8,
    details: []const Detail,
) Error {
    app_error = .{ .status = status, .code = code, .message = message, .details = details };
    return error.App;
}

pub fn badRequest() Error {
    return appError(.bad_request, "bad_request", "the request is malformed", &.{});
}

pub fn unauthorized() Error {
    return appError(.unauthorized, "unauthorized", "authentication is required", &.{});
}

pub fn invalidCredentials() Error {
    return appError(.unauthorized, "invalid_credentials", "the username or password is wrong", &.{});
}

pub fn forbidden() Error {
    return appError(.forbidden, "forbidden", "you may not access this resource", &.{});
}

pub fn notFound() Error {
    return appError(.not_found, "not_found", "the resource does not exist", &.{});
}

pub fn conflict() Error {
    return appError(.conflict, "conflict", "the resource already exists", &.{});
}

pub fn invalidTransition() Error {
    return appError(.conflict, "invalid_transition", "the status change is not allowed", &.{});
}

pub fn preconditionFailed() Error {
    return appError(.precondition_failed, "precondition_failed", "the resource has changed", &.{});
}

pub fn preconditionRequired() Error {
    return appError(.precondition_required, "precondition_required", "the If-Match header is required", &.{});
}

pub fn quotaExceeded() Error {
    return appError(.too_many_requests, "quota_exceeded", "the request quota is exhausted", &.{});
}

pub fn invalid(details: []Detail) Error {
    std.mem.sort(Detail, details, {}, struct {
        fn lessThan(_: void, a: Detail, b: Detail) bool {
            const by_field = std.mem.order(u8, a.field, b.field);
            if (by_field != .eq) return by_field == .lt;
            return std.mem.order(u8, a.message, b.message) == .lt;
        }
    }.lessThan);
    return appError(.unprocessable_entity, "validation_failed", "the request body is not valid", details);
}

pub fn fail(field: []const u8, message: []const u8) Detail {
    return .{ .field = field, .message = message };
}

pub fn computeScore(priority: i64, status: []const u8) i64 {
    const base_score = priority * 10;
    for (statuses, status_bonus) |name, bonus| {
        if (std.mem.eql(u8, name, status)) return base_score + bonus;
    }
    return base_score;
}

pub fn isStatus(value: []const u8) bool {
    for (statuses) |name| {
        if (std.mem.eql(u8, name, value)) return true;
    }
    return false;
}

pub fn isRole(value: []const u8) bool {
    for (roles) |name| {
        if (std.mem.eql(u8, name, value)) return true;
    }
    return false;
}

pub fn isGroupBy(value: []const u8) bool {
    for (group_bys) |name| {
        if (std.mem.eql(u8, name, value)) return true;
    }
    return false;
}

pub fn allowedMove(from: []const u8, to: []const u8) bool {
    for (transitions) |move| {
        if (std.mem.eql(u8, move[0], from) and std.mem.eql(u8, move[1], to)) return true;
    }
    return false;
}

pub fn textLength(value: []const u8) usize {
    return std.unicode.utf8CountCodepoints(value) catch value.len;
}

pub fn containsIgnoreCase(haystack: []const u8, needle: []const u8) bool {
    if (needle.len > haystack.len) return false;
    var start: usize = 0;
    while (start + needle.len <= haystack.len) : (start += 1) {
        if (std.ascii.eqlIgnoreCase(haystack[start .. start + needle.len], needle)) return true;
    }
    return false;
}

pub fn checkString(
    value: []const u8,
    field: []const u8,
    max_length: usize,
    errors: *std.ArrayList(Detail),
    arena: Allocator,
) !void {
    if (value.len == 0) {
        const message = try std.fmt.allocPrint(arena, "{s} is required", .{field});
        try errors.append(arena, fail(field, message));
    } else if (textLength(value) > max_length) {
        const message = try std.fmt.allocPrint(arena, "{s} is too long", .{field});
        try errors.append(arena, fail(field, message));
    }
}

pub fn checkPriority(value: ?i64, errors: *std.ArrayList(Detail), arena: Allocator) !void {
    if (value == null or value.? < min_priority or value.? > max_priority) {
        try errors.append(arena, fail("priority", "priority is out of range"));
    }
}

pub fn checkStatus(value: Value, errors: *std.ArrayList(Detail), arena: Allocator) !void {
    const ok = switch (value) {
        .string => |text| isStatus(text),
        else => false,
    };
    if (!ok) try errors.append(arena, fail("status", "status is not valid"));
}

pub fn checkRole(value: Value, errors: *std.ArrayList(Detail), arena: Allocator) !void {
    const ok = switch (value) {
        .string => |text| isRole(text),
        else => false,
    };
    if (!ok) try errors.append(arena, fail("role", "role is not valid"));
}

pub fn checkQuota(value: Value, errors: *std.ArrayList(Detail), arena: Allocator) !void {
    const ok = switch (value) {
        .integer => |number| number >= 0,
        else => false,
    };
    if (!ok) try errors.append(arena, fail("quota", "quota is out of range"));
}
