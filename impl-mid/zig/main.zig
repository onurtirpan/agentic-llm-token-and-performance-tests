// Task Service, mid tier — std.http.Server implementation.
const std = @import("std");
const http = std.http;
const Allocator = std.mem.Allocator;
const Request = http.Server.Request;

const max_title_length = 80;
const max_name_length = 60;
const min_priority = 1;
const max_priority = 5;
const default_limit = 20;
const max_limit = 100;
const port = 8080;

const status_bonus = [_]struct { []const u8, i64 }{
    .{ "todo", 0 }, .{ "in_progress", 3 }, .{ "done", 5 }, .{ "archived", 0 },
};
const transitions = [_][2][]const u8{
    .{ "todo", "in_progress" }, .{ "todo", "archived" }, .{ "in_progress", "todo" },
    .{ "in_progress", "done" }, .{ "done", "archived" },
};
const project_sorts = [_][]const u8{ "id", "name", "taskCount" };
const task_sorts = [_][]const u8{ "id", "title", "priority", "score", "status" };

const User = struct { id: i64, username: []const u8, password: []const u8, role: []const u8 };
const Project = struct { id: i64, name: []const u8, ownerId: i64 };
const Task = struct {
    id: i64,
    projectId: i64,
    title: []const u8,
    priority: i64,
    status: []const u8,
    assigneeId: ?i64,
    score: i64,
};
const ProjectRow = struct { id: i64, name: []const u8, ownerId: i64, taskCount: i64 };
const TaskRow = Task;
const Detail = struct { field: []const u8, message: []const u8 };
const AppError = struct {
    status: http.Status,
    code: []const u8,
    message: []const u8,
    details: []const Detail,
};
const Input = struct {
    limit: i64 = default_limit,
    offset: i64 = 0,
    sort: []const u8 = "id",
    order: []const u8 = "asc",
};

const gpa = std.heap.smp_allocator;

var users: std.array_hash_map.Auto(i64, User) = .empty;
var sessions: std.array_hash_map.String(i64) = .empty;
var projects: std.array_hash_map.Auto(i64, Project) = .empty;
var tasks: std.array_hash_map.Auto(i64, Task) = .empty;
var next_project_id: i64 = 1;
var next_task_id: i64 = 1;

var io: std.Io = undefined;
var prng: std.Random.DefaultPrng = .init(0x2545f4914f6cdd1d);
var id_counter: u32 = 0;
var app_error: AppError = undefined;
var request_id: []const u8 = "";
var authorization: []const u8 = "";
var user_id: ?i64 = null;
var log_status: u16 = 200;
var body_buffer: [128 * 1024]u8 = undefined;

fn computeScore(priority: i64, status: []const u8) i64 {
    const base_score = priority * 10;
    for (status_bonus) |entry| {
        if (std.mem.eql(u8, entry[0], status)) return base_score + entry[1];
    }
    return base_score;
}

fn taskCount(project_id: i64) i64 {
    var count: i64 = 0;
    for (tasks.values()) |task| {
        if (task.projectId == project_id) count += 1;
    }
    return count;
}

fn serializeProject(project: *const Project) ProjectRow {
    return .{
        .id = project.id,
        .name = project.name,
        .ownerId = project.ownerId,
        .taskCount = taskCount(project.id),
    };
}

fn serializeTask(task: *const Task) TaskRow {
    return task.*;
}

fn appError(
    status: http.Status,
    code: []const u8,
    message: []const u8,
    details: []const Detail,
) error{App} {
    app_error = .{ .status = status, .code = code, .message = message, .details = details };
    return error.App;
}

fn badRequest() error{App} {
    return appError(.bad_request, "bad_request", "the request is malformed", &.{});
}

fn notFound() error{App} {
    return appError(.not_found, "not_found", "the resource does not exist", &.{});
}

fn forbidden() error{App} {
    return appError(.forbidden, "forbidden", "you may not access this resource", &.{});
}

fn conflict() error{App} {
    return appError(.conflict, "conflict", "the resource already exists", &.{});
}

fn invalid(details: []Detail) error{App} {
    std.mem.sort(Detail, details, {}, struct {
        fn lessThan(_: void, a: Detail, b: Detail) bool {
            const by_field = std.mem.order(u8, a.field, b.field);
            if (by_field != .eq) return by_field == .lt;
            return std.mem.order(u8, a.message, b.message) == .lt;
        }
    }.lessThan);
    return appError(.unprocessable_entity, "validation_failed", "the request body is not valid", details);
}

fn fail(field: []const u8, message: []const u8) Detail {
    return .{ .field = field, .message = message };
}

fn newId(allocator: Allocator) ![]const u8 {
    id_counter += 1;
    var bytes: [12]u8 = undefined;
    prng.random().bytes(&bytes);
    std.mem.writeInt(u32, bytes[0..4], id_counter, .little);
    const hex = std.fmt.bytesToHex(bytes, .lower);
    return allocator.dupe(u8, &hex);
}

fn readBody(request: *Request, arena: Allocator) !std.json.Value {
    var buffer: [4096]u8 = undefined;
    const raw = try request.readerExpectNone(&buffer).allocRemaining(arena, .limited(64 * 1024));
    if (std.mem.trim(u8, raw, " \t\r\n").len == 0) return .{ .object = .empty };
    const parsed = std.json.parseFromSliceLeaky(std.json.Value, arena, raw, .{}) catch
        return badRequest();
    if (parsed != .object) return badRequest();
    return parsed;
}

fn readInt(body: std.json.Value, field: []const u8, default: ?i64) error{App}!?i64 {
    const value = body.object.get(field) orelse return default;
    return switch (value) {
        .null => null,
        .integer => |number| number,
        else => badRequest(),
    };
}

fn readString(
    body: std.json.Value,
    field: []const u8,
    errors: *std.ArrayList(Detail),
    max_length: usize,
    required: bool,
    arena: Allocator,
) ![]const u8 {
    const value = switch (body.object.get(field) orelse std.json.Value{ .string = "" }) {
        .string => |text| text,
        else => return badRequest(),
    };
    if (value.len == 0) {
        if (required) try errors.append(arena, fail(field, try std.fmt.allocPrint(
            arena,
            "{s} is required",
            .{field},
        )));
    } else if ((std.unicode.utf8CountCodepoints(value) catch value.len) > max_length) {
        try errors.append(arena, fail(field, try std.fmt.allocPrint(
            arena,
            "{s} is too long",
            .{field},
        )));
    }
    return value;
}

fn readPriority(body: std.json.Value, errors: *std.ArrayList(Detail), arena: Allocator) !i64 {
    const value = try readInt(body, "priority", 0);
    if (value == null or value.? < min_priority or value.? > max_priority) {
        try errors.append(arena, fail("priority", "priority is out of range"));
    }
    return value orelse 0;
}

fn readUserRef(
    body: std.json.Value,
    field: []const u8,
    errors: *std.ArrayList(Detail),
    default: ?i64,
    arena: Allocator,
) !?i64 {
    const value = try readInt(body, field, default);
    if (value) |id| {
        if (!users.contains(id)) try errors.append(arena, fail(field, try std.fmt.allocPrint(
            arena,
            "{s} is not a known user",
            .{field},
        )));
    }
    return value;
}

fn parseId(raw: []const u8) error{App}!i64 {
    return std.fmt.parseInt(i64, raw, 10) catch return badRequest();
}

fn readPage(query: []const u8, allowed: []const []const u8, arena: Allocator) !Input {
    var input: Input = .{};
    var errors: std.ArrayList(Detail) = .empty;
    var pairs = std.mem.splitScalar(u8, query, '&');
    while (pairs.next()) |pair| {
        const cut = std.mem.findScalar(u8, pair, '=') orelse continue;
        const key = pair[0..cut];
        const value = pair[cut + 1 ..];
        if (std.mem.eql(u8, key, "limit")) {
            input.limit = std.fmt.parseInt(i64, value, 10) catch -1;
            if (input.limit < 1 or input.limit > max_limit) {
                try errors.append(arena, fail("limit", "limit is out of range"));
            }
        } else if (std.mem.eql(u8, key, "offset")) {
            input.offset = std.fmt.parseInt(i64, value, 10) catch -1;
            if (input.offset < 0) {
                try errors.append(arena, fail("offset", "offset is out of range"));
            }
        } else if (std.mem.eql(u8, key, "sort")) {
            input.sort = value;
        } else if (std.mem.eql(u8, key, "order")) {
            input.order = value;
        }
    }
    var known = false;
    for (allowed) |name| {
        if (std.mem.eql(u8, name, input.sort)) known = true;
    }
    if (!known) try errors.append(arena, fail("sort", "sort is not a valid field"));
    if (!std.mem.eql(u8, input.order, "asc") and !std.mem.eql(u8, input.order, "desc")) {
        try errors.append(arena, fail("order", "order must be asc or desc"));
    }
    if (errors.items.len > 0) return invalid(errors.items);
    return input;
}

fn paginate(request: *Request, rows: anytype, input: Input) !void {
    const Row = @typeInfo(@TypeOf(rows)).pointer.child;
    std.mem.sort(Row, rows, {}, struct {
        fn lessThan(_: void, a: Row, b: Row) bool {
            return a.id < b.id;
        }
    }.lessThan);
    const descending = std.mem.eql(u8, input.order, "desc");
    inline for (std.meta.fields(Row)) |field| {
        if (std.mem.eql(u8, field.name, input.sort)) {
            std.mem.sort(Row, rows, descending, struct {
                fn lessThan(desc: bool, a: Row, b: Row) bool {
                    const first = @field(if (desc) b else a, field.name);
                    const second = @field(if (desc) a else b, field.name);
                    return switch (@TypeOf(first)) {
                        []const u8 => std.mem.order(u8, first, second) == .lt,
                        i64 => first < second,
                        else => false,
                    };
                }
            }.lessThan);
            break;
        }
    }
    const total = rows.len;
    const start = @min(@as(usize, @intCast(input.offset)), total);
    const end = @min(start + @as(usize, @intCast(input.limit)), total);
    return writeJson(request, .ok, .{
        .items = rows[start..end],
        .total = total,
        .limit = input.limit,
        .offset = input.offset,
    });
}

fn authenticate() error{App}!*User {
    if (std.mem.startsWith(u8, authorization, "Bearer ")) {
        if (sessions.get(authorization["Bearer ".len..])) |id| {
            user_id = id;
            return users.getPtr(id).?;
        }
    }
    return appError(.unauthorized, "unauthorized", "authentication is required", &.{});
}

fn requireAdmin(user: *const User) error{App}!void {
    if (!std.mem.eql(u8, user.role, "admin")) return forbidden();
}

fn reachableProject(project_id: i64, user: *const User) error{App}!*Project {
    const project = projects.getPtr(project_id) orelse return notFound();
    if (!std.mem.eql(u8, user.role, "admin") and project.ownerId != user.id) return forbidden();
    return project;
}

fn reachableTask(task_id: i64, user: *const User) error{App}!*Task {
    const task = tasks.getPtr(task_id) orelse return notFound();
    _ = try reachableProject(task.projectId, user);
    return task;
}

fn respond(request: *Request, status: http.Status, body: []const u8) !void {
    log_status = @intFromEnum(status);
    const headers = [_]http.Header{
        .{ .name = "content-type", .value = "application/json" },
        .{ .name = "X-Request-Id", .value = request_id },
    };
    return request.respond(body, .{ .status = status, .extra_headers = &headers });
}

fn writeJson(request: *Request, status: http.Status, body: anytype) !void {
    var writer: std.Io.Writer = .fixed(&body_buffer);
    try std.json.Stringify.value(body, .{}, &writer);
    return respond(request, status, writer.buffered());
}

fn getHealth(request: *Request) !void {
    return writeJson(request, .ok, .{
        .status = "ok",
        .projects = projects.count(),
        .tasks = tasks.count(),
    });
}

fn login(request: *Request, arena: Allocator) !void {
    const body = try readBody(request, arena);
    var errors: std.ArrayList(Detail) = .empty;
    const username = try readString(body, "username", &errors, max_name_length, true, arena);
    const password = try readString(body, "password", &errors, max_name_length, true, arena);
    if (errors.items.len > 0) return invalid(errors.items);
    for (users.values()) |user| {
        if (std.mem.eql(u8, user.username, username) and std.mem.eql(u8, user.password, password)) {
            const token = try newId(gpa);
            try sessions.put(gpa, token, user.id);
            return writeJson(request, .ok, .{
                .token = token,
                .userId = user.id,
                .role = user.role,
            });
        }
    }
    return appError(.unauthorized, "invalid_credentials", "the username or password is wrong", &.{});
}

fn logout(request: *Request) !void {
    _ = try authenticate();
    _ = sessions.orderedRemove(authorization["Bearer ".len..]);
    return respond(request, .no_content, "");
}

fn getMe(request: *Request) !void {
    const user = try authenticate();
    return writeJson(request, .ok, .{
        .userId = user.id,
        .username = user.username,
        .role = user.role,
    });
}

fn listProjects(request: *Request, arena: Allocator, query: []const u8) !void {
    const user = try authenticate();
    const input = try readPage(query, &project_sorts, arena);
    var rows: std.ArrayList(ProjectRow) = .empty;
    for (projects.values()) |*project| {
        if (std.mem.eql(u8, user.role, "admin") or project.ownerId == user.id) {
            try rows.append(arena, serializeProject(project));
        }
    }
    return paginate(request, rows.items, input);
}

fn createProject(request: *Request, arena: Allocator) !void {
    const user = try authenticate();
    try requireAdmin(user);
    const body = try readBody(request, arena);
    var errors: std.ArrayList(Detail) = .empty;
    const name = try readString(body, "name", &errors, max_name_length, true, arena);
    const owner_id = (try readUserRef(body, "ownerId", &errors, user.id, arena)) orelse user.id;
    if (errors.items.len > 0) return invalid(errors.items);
    for (projects.values()) |other| {
        if (other.ownerId == owner_id and std.mem.eql(u8, other.name, name)) return conflict();
    }
    const project: Project = .{
        .id = next_project_id,
        .name = try gpa.dupe(u8, name),
        .ownerId = owner_id,
    };
    try projects.put(gpa, next_project_id, project);
    next_project_id += 1;
    return writeJson(request, .created, serializeProject(&project));
}

fn getProject(request: *Request, raw_id: []const u8) !void {
    const user = try authenticate();
    return writeJson(request, .ok, serializeProject(try reachableProject(try parseId(raw_id), user)));
}

fn updateProject(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const user = try authenticate();
    try requireAdmin(user);
    const project = try reachableProject(try parseId(raw_id), user);
    const body = try readBody(request, arena);
    if (body.object.get("name") == null) return writeJson(request, .ok, serializeProject(project));
    var errors: std.ArrayList(Detail) = .empty;
    const name = try readString(body, "name", &errors, max_name_length, true, arena);
    if (errors.items.len > 0) return invalid(errors.items);
    for (projects.values()) |other| {
        if (other.ownerId == project.ownerId and std.mem.eql(u8, other.name, name) and
            other.id != project.id) return conflict();
    }
    project.name = try gpa.dupe(u8, name);
    return writeJson(request, .ok, serializeProject(project));
}

fn deleteProject(request: *Request, raw_id: []const u8) !void {
    const user = try authenticate();
    try requireAdmin(user);
    const project_id = (try reachableProject(try parseId(raw_id), user)).id;
    var index: usize = 0;
    while (index < tasks.count()) {
        if (tasks.values()[index].projectId == project_id) {
            _ = tasks.orderedRemove(tasks.keys()[index]);
        } else index += 1;
    }
    _ = projects.orderedRemove(project_id);
    return respond(request, .no_content, "");
}

fn listTasks(request: *Request, arena: Allocator, raw_id: []const u8, query: []const u8) !void {
    const user = try authenticate();
    const project = try reachableProject(try parseId(raw_id), user);
    const input = try readPage(query, &task_sorts, arena);
    var rows: std.ArrayList(TaskRow) = .empty;
    for (tasks.values()) |*task| {
        if (task.projectId == project.id) try rows.append(arena, serializeTask(task));
    }
    return paginate(request, rows.items, input);
}

fn createTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const user = try authenticate();
    const project = try reachableProject(try parseId(raw_id), user);
    const body = try readBody(request, arena);
    var errors: std.ArrayList(Detail) = .empty;
    const title = try readString(body, "title", &errors, max_title_length, true, arena);
    const priority = try readPriority(body, &errors, arena);
    const assignee_id = try readUserRef(body, "assigneeId", &errors, null, arena);
    if (errors.items.len > 0) return invalid(errors.items);
    const task: Task = .{
        .id = next_task_id,
        .projectId = project.id,
        .title = try gpa.dupe(u8, title),
        .priority = priority,
        .status = "todo",
        .assigneeId = assignee_id,
        .score = computeScore(priority, "todo"),
    };
    try tasks.put(gpa, next_task_id, task);
    next_task_id += 1;
    return writeJson(request, .created, serializeTask(&task));
}

fn getTask(request: *Request, raw_id: []const u8) !void {
    const user = try authenticate();
    return writeJson(request, .ok, serializeTask(try reachableTask(try parseId(raw_id), user)));
}

fn replaceTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const user = try authenticate();
    const task = try reachableTask(try parseId(raw_id), user);
    const body = try readBody(request, arena);
    var errors: std.ArrayList(Detail) = .empty;
    const title = try readString(body, "title", &errors, max_title_length, true, arena);
    const priority = try readPriority(body, &errors, arena);
    const assignee_id = try readUserRef(body, "assigneeId", &errors, null, arena);
    if (errors.items.len > 0) return invalid(errors.items);
    task.title = try gpa.dupe(u8, title);
    task.priority = priority;
    task.assigneeId = assignee_id;
    task.score = computeScore(priority, task.status);
    return writeJson(request, .ok, serializeTask(task));
}

fn updateStatus(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const user = try authenticate();
    const task = try reachableTask(try parseId(raw_id), user);
    const body = try readBody(request, arena);
    const wanted = switch (body.object.get("status") orelse std.json.Value{ .null = {} }) {
        .string => |text| text,
        else => "",
    };
    var next_status: ?[]const u8 = null;
    for (status_bonus) |entry| {
        if (std.mem.eql(u8, entry[0], wanted)) next_status = entry[0];
    }
    if (next_status == null) {
        var errors: std.ArrayList(Detail) = .empty;
        try errors.append(arena, fail("status", "status is not valid"));
        return invalid(errors.items);
    }
    for (transitions) |move| {
        if (std.mem.eql(u8, move[0], task.status) and std.mem.eql(u8, move[1], next_status.?)) {
            task.status = next_status.?;
            task.score = computeScore(task.priority, task.status);
            return writeJson(request, .ok, serializeTask(task));
        }
    }
    return appError(.conflict, "invalid_transition", "the status change is not allowed", &.{});
}

fn deleteTask(request: *Request, raw_id: []const u8) !void {
    const user = try authenticate();
    const task = try reachableTask(try parseId(raw_id), user);
    _ = tasks.orderedRemove(task.id);
    return respond(request, .no_content, "");
}

fn getStats(request: *Request) !void {
    const user = try authenticate();
    try requireAdmin(user);
    var by_status = [_]i64{0} ** status_bonus.len;
    var sum_score: i64 = 0;
    for (tasks.values()) |task| {
        sum_score += task.score;
        for (status_bonus, 0..) |entry, index| {
            if (std.mem.eql(u8, entry[0], task.status)) by_status[index] += 1;
        }
    }
    const total = tasks.count();
    var avg_score: f64 = 0;
    if (total > 0) {
        const mean = @as(f64, @floatFromInt(sum_score)) / @as(f64, @floatFromInt(total));
        avg_score = @round(mean * 100) / 100;
    }
    var best: ?*const Project = null;
    for (projects.values()) |*project| {
        if (best == null or taskCount(project.id) > taskCount(best.?.id)) best = project;
    }
    return writeJson(request, .ok, .{
        .projects = projects.count(),
        .tasks = total,
        .users = users.count(),
        .sessions = sessions.count(),
        .byStatus = .{
            .todo = by_status[0],
            .in_progress = by_status[1],
            .done = by_status[2],
            .archived = by_status[3],
        },
        .avgScore = avg_score,
        .topProjectName = if (best) |project| project.name else null,
    });
}

fn handleRequest(request: *Request, arena: Allocator, path: []const u8, query: []const u8) !void {
    const method = request.head.method;
    if (std.mem.eql(u8, path, "/health")) return getHealth(request);
    if (std.mem.eql(u8, path, "/auth/login")) return login(request, arena);
    if (std.mem.eql(u8, path, "/auth/logout")) return logout(request);
    if (std.mem.eql(u8, path, "/me")) return getMe(request);
    if (std.mem.eql(u8, path, "/stats")) return getStats(request);
    if (std.mem.eql(u8, path, "/projects")) return switch (method) {
        .POST => createProject(request, arena),
        else => listProjects(request, arena, query),
    };
    if (std.mem.startsWith(u8, path, "/projects/")) {
        const rest = path["/projects/".len..];
        const cut = std.mem.findScalar(u8, rest, '/');
        const raw_id = if (cut) |i| rest[0..i] else rest;
        const tail = if (cut) |i| rest[i..] else "";
        if (tail.len == 0) return switch (method) {
            .PATCH => updateProject(request, arena, raw_id),
            .DELETE => deleteProject(request, raw_id),
            else => getProject(request, raw_id),
        };
        if (std.mem.eql(u8, tail, "/tasks")) return switch (method) {
            .POST => createTask(request, arena, raw_id),
            else => listTasks(request, arena, raw_id, query),
        };
    }
    if (std.mem.startsWith(u8, path, "/tasks/")) {
        const rest = path["/tasks/".len..];
        const cut = std.mem.findScalar(u8, rest, '/');
        const raw_id = if (cut) |i| rest[0..i] else rest;
        const tail = if (cut) |i| rest[i..] else "";
        if (tail.len == 0) return switch (method) {
            .PUT => replaceTask(request, arena, raw_id),
            .DELETE => deleteTask(request, raw_id),
            else => getTask(request, raw_id),
        };
        if (std.mem.eql(u8, tail, "/status")) return updateStatus(request, arena, raw_id);
    }
    return notFound();
}

fn observe(request: *Request, arena: Allocator) !void {
    const started: std.Io.Timestamp = .now(io, .awake);
    const method = @tagName(request.head.method);
    const target = try arena.dupe(u8, request.head.target);
    request_id = "";
    authorization = "";
    user_id = null;
    log_status = 200;
    var headers = request.iterateHeaders();
    while (headers.next()) |header| {
        if (std.ascii.eqlIgnoreCase(header.name, "authorization")) {
            authorization = try arena.dupe(u8, header.value);
        } else if (std.ascii.eqlIgnoreCase(header.name, "x-request-id") and header.value.len > 0) {
            request_id = try arena.dupe(u8, header.value);
        }
    }
    if (request_id.len == 0) request_id = try newId(arena);
    const cut = std.mem.findScalar(u8, target, '?');
    const path = if (cut) |i| target[0..i] else target;
    const query = if (cut) |i| target[i + 1 ..] else "";
    handleRequest(request, arena, path, query) catch |err| switch (err) {
        error.App => try writeJson(request, app_error.status, .{ .@"error" = .{
            .code = app_error.code,
            .message = app_error.message,
            .requestId = request_id,
            .details = app_error.details,
        } }),
        else => return err,
    };
    var buffer: [1024]u8 = undefined;
    var writer: std.Io.Writer = .fixed(&buffer);
    try std.json.Stringify.value(.{
        .level = if (log_status >= 500) "error" else if (log_status >= 400) "warn" else "info",
        .requestId = request_id,
        .method = method,
        .path = path,
        .status = log_status,
        .durationMs = started.durationTo(.now(io, .awake)).toMilliseconds(),
        .userId = user_id,
    }, .{}, &writer);
    try writer.writeByte('\n');
    try std.Io.File.stdout().writeStreamingAll(io, writer.buffered());
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init(gpa, .{});
    defer threaded.deinit();
    io = threaded.io();

    try users.put(gpa, 1, .{ .id = 1, .username = "admin", .password = "admin-secret", .role = "admin" });
    try users.put(gpa, 2, .{ .id = 2, .username = "alice", .password = "alice-secret", .role = "user" });
    try users.put(gpa, 3, .{ .id = 3, .username = "bob", .password = "bob-secret", .role = "user" });

    const address: std.Io.net.IpAddress = .{ .ip4 = .loopback(port) };
    var server = try address.listen(io, .{ .reuse_address = true });
    defer server.deinit(io);

    var arena_state: std.heap.ArenaAllocator = .init(gpa);
    defer arena_state.deinit();

    while (true) {
        const stream = server.accept(io) catch continue;
        defer stream.close(io);
        var in_buffer: [16 * 1024]u8 = undefined;
        var out_buffer: [16 * 1024]u8 = undefined;
        var reader = stream.reader(io, &in_buffer);
        var writer = stream.writer(io, &out_buffer);
        var connection: http.Server = .init(&reader.interface, &writer.interface);
        while (true) {
            var request = connection.receiveHead() catch break;
            _ = arena_state.reset(.retain_capacity);
            observe(&request, arena_state.allocator()) catch break;
        }
    }
}
