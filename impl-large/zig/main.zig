// Task Service, large tier — HTTP routing, middleware and the entry point.
const std = @import("std");
const domain = @import("domain.zig");
const store = @import("store.zig");
const service = @import("service.zig");
const http = std.http;
const Allocator = std.mem.Allocator;
const Request = http.Server.Request;
const Value = std.json.Value;
const Detail = domain.Detail;
const Errors = std.ArrayList(Detail);
const gpa = store.gpa;

var io: std.Io = undefined;
var prng: std.Random.DefaultPrng = .init(0x2545f4914f6cdd1d);
var id_counter: u32 = 0;
var body_buffer: [256 * 1024]u8 = undefined;

var request_id: []const u8 = "";
var authorization: []const u8 = "";
var if_match: ?[]const u8 = null;
var idempotency_key: ?[]const u8 = null;
var idem_slot: ?[]const u8 = null;
var user_id: ?i64 = null;
var quota_remaining: ?i64 = null;
var quota_text: ?[]const u8 = null;
var etag: ?[]const u8 = null;
var etag_version: ?i64 = null;
var replayed: bool = false;
var log_status: u16 = 200;
var route_pattern: []const u8 = "/{path}";
var etag_buffer: [24]u8 = undefined;
var quota_buffer: [24]u8 = undefined;

// ---------------------------------------------------------------------- helpers

fn newId(allocator: Allocator) ![]const u8 {
    id_counter += 1;
    var bytes: [12]u8 = undefined;
    prng.random().bytes(&bytes);
    std.mem.writeInt(u32, bytes[0..4], id_counter, .little);
    const hex = std.fmt.bytesToHex(bytes, .lower);
    return allocator.dupe(u8, &hex);
}

fn setEtag(version: i64) void {
    etag_version = version;
    etag = std.fmt.bufPrint(&etag_buffer, "{d}", .{version}) catch null;
}

fn respond(request: *Request, status: http.Status, body: []const u8) !void {
    log_status = @intFromEnum(status);
    if (!replayed) {
        if (idem_slot) |slot| {
            try store.idempotency.put(gpa, slot, .{
                .status = status,
                .body = try gpa.dupe(u8, body),
                .version = etag_version,
            });
            idem_slot = null;
        }
    }
    var list: [5]http.Header = undefined;
    var count: usize = 0;
    list[count] = .{ .name = "content-type", .value = "application/json" };
    count += 1;
    list[count] = .{ .name = "X-Request-Id", .value = request_id };
    count += 1;
    if (etag) |value| {
        list[count] = .{ .name = "ETag", .value = value };
        count += 1;
    }
    if (quota_text) |value| {
        list[count] = .{ .name = "X-Quota-Remaining", .value = value };
        count += 1;
    }
    if (replayed) {
        list[count] = .{ .name = "Idempotency-Replayed", .value = "true" };
        count += 1;
    }
    return request.respond(body, .{ .status = status, .extra_headers = list[0..count] });
}

fn writeJson(request: *Request, status: http.Status, body: anytype) !void {
    var writer: std.Io.Writer = .fixed(&body_buffer);
    try std.json.Stringify.value(body, .{}, &writer);
    return respond(request, status, writer.buffered());
}

/// A single-resource body carries its version, so the ETag comes for free.
fn writeValue(request: *Request, status: http.Status, value: Value) !void {
    if (value == .object) {
        if (value.object.get("version")) |version| {
            if (version == .integer) setEtag(version.integer);
        }
    }
    return writeJson(request, status, value);
}

fn writeError(request: *Request) !void {
    return writeJson(request, domain.app_error.status, .{ .@"error" = .{
        .code = domain.app_error.code,
        .message = domain.app_error.message,
        .requestId = request_id,
        .details = domain.app_error.details,
    } });
}

fn readBody(request: *Request, arena: Allocator) !Value {
    var buffer: [4096]u8 = undefined;
    const raw = try request.readerExpectNone(&buffer).allocRemaining(arena, .limited(128 * 1024));
    if (std.mem.trim(u8, raw, " \t\r\n").len == 0) return .{ .object = .empty };
    const parsed = std.json.parseFromSliceLeaky(Value, arena, raw, .{}) catch
        return domain.badRequest();
    if (parsed != .object) return domain.badRequest();
    return parsed;
}

fn text(body: Value, field: []const u8) domain.Error![]const u8 {
    const value = body.object.get(field) orelse return "";
    return switch (value) {
        .string => |found| found,
        else => domain.badRequest(),
    };
}

fn whole(body: Value, field: []const u8, default: ?i64) domain.Error!?i64 {
    const value = body.object.get(field) orelse return default;
    return switch (value) {
        .null => null,
        .integer => |number| number,
        else => domain.badRequest(),
    };
}

fn parseId(raw: []const u8) domain.Error!i64 {
    return std.fmt.parseInt(i64, raw, 10) catch return domain.badRequest();
}

fn queryGet(query: []const u8, key: []const u8) ?[]const u8 {
    var pairs = std.mem.splitScalar(u8, query, '&');
    while (pairs.next()) |pair| {
        const cut = std.mem.findScalar(u8, pair, '=') orelse continue;
        if (std.mem.eql(u8, pair[0..cut], key)) return pair[cut + 1 ..];
    }
    return null;
}

fn oneError(
    arena: Allocator,
    field: []const u8,
    message: []const u8,
) error{ App, OutOfMemory } {
    var errors: Errors = .empty;
    errors.append(arena, domain.fail(field, message)) catch return error.OutOfMemory;
    return domain.invalid(errors.items);
}

/// Authenticate, charge the quota, then check the role. This order is fixed.
fn begin(admin: bool) !service.Actor {
    const actor = try service.authenticate(authorization);
    user_id = actor.user.id;
    const remaining = try service.chargeQuota(actor);
    quota_remaining = remaining;
    quota_text = std.fmt.bufPrint(&quota_buffer, "{d}", .{remaining}) catch null;
    if (admin) try service.requireAdmin(actor.user);
    return actor;
}

/// Run the handler once per Idempotency-Key, then replay the recorded outcome.
fn tryReplay(request: *Request, arena: Allocator, token: []const u8) !bool {
    const key = idempotency_key orelse return false;
    const slot = try std.fmt.allocPrint(arena, "{s}\x00{s}", .{ token, key });
    if (store.idempotency.get(slot)) |saved| {
        replayed = true;
        if (saved.version) |version| setEtag(version);
        try respond(request, saved.status, saved.body);
        return true;
    }
    idem_slot = try gpa.dupe(u8, slot);
    return false;
}

fn readPage(arena: Allocator, query: []const u8, allowed: []const []const u8) !service.Input {
    var input: service.Input = .{ .sort = allowed[0] };
    var errors: Errors = .empty;
    if (queryGet(query, "limit")) |raw| {
        input.limit = std.fmt.parseInt(i64, raw, 10) catch -1;
        if (input.limit < 1 or input.limit > domain.max_limit) {
            try errors.append(arena, domain.fail("limit", "limit is out of range"));
        }
    }
    if (queryGet(query, "offset")) |raw| {
        input.offset = std.fmt.parseInt(i64, raw, 10) catch -1;
        if (input.offset < 0) {
            try errors.append(arena, domain.fail("offset", "offset is out of range"));
        }
    }
    if (queryGet(query, "sort")) |raw| input.sort = raw;
    if (queryGet(query, "order")) |raw| input.order = raw;
    var known = false;
    for (allowed) |name| {
        if (std.mem.eql(u8, name, input.sort)) known = true;
    }
    if (!known) try errors.append(arena, domain.fail("sort", "sort is not a valid field"));
    if (!std.mem.eql(u8, input.order, "asc") and !std.mem.eql(u8, input.order, "desc")) {
        try errors.append(arena, domain.fail("order", "order must be asc or desc"));
    }
    if (errors.items.len > 0) return domain.invalid(errors.items);
    return input;
}

fn writePage(request: *Request, rows: []Value, input: service.Input) !void {
    return writeJson(request, .ok, service.paginate(rows, input));
}

// ----------------------------------------------------------------- health, auth

fn getHealth(request: *Request) !void {
    var live_projects: i64 = 0;
    for (store.projects.values()) |project| {
        if (!project.deleted) live_projects += 1;
    }
    var live_tasks: i64 = 0;
    for (store.tasks.values()) |task| {
        if (!task.deleted) live_tasks += 1;
    }
    return writeJson(request, .ok, .{
        .status = "ok",
        .projects = live_projects,
        .tasks = live_tasks,
        .comments = store.comments.count(),
    });
}

fn login(request: *Request, arena: Allocator) !void {
    const body = try readBody(request, arena);
    var errors: Errors = .empty;
    const username = try text(body, "username");
    const password = try text(body, "password");
    if (username.len == 0) try errors.append(arena, domain.fail("username", "username is required"));
    if (password.len == 0) try errors.append(arena, domain.fail("password", "password is required"));
    if (errors.items.len > 0) return domain.invalid(errors.items);
    const token = try newId(gpa);
    const user = try service.login(username, password, token);
    return writeJson(request, .ok, .{ .token = token, .userId = user.id, .role = user.role });
}

fn logout(request: *Request) !void {
    const actor = try begin(false);
    _ = store.sessions.orderedRemove(actor.token);
    return respond(request, .no_content, "");
}

fn getMe(request: *Request) !void {
    const actor = try begin(false);
    return writeJson(request, .ok, .{
        .userId = actor.user.id,
        .username = actor.user.username,
        .role = actor.user.role,
    });
}

// ----------------------------------------------------------------------- users

fn listUsers(request: *Request, arena: Allocator, query: []const u8) !void {
    _ = try begin(true);
    const input = try readPage(arena, query, &domain.user_sorts);
    var rows: std.ArrayList(Value) = .empty;
    for (store.users.values()) |*user| {
        if (!user.deleted) try rows.append(arena, try service.serializeUser(arena, user));
    }
    return writePage(request, rows.items, input);
}

fn createUser(request: *Request, arena: Allocator) !void {
    const actor = try begin(true);
    const body = try readBody(request, arena);
    if (try tryReplay(request, arena, actor.token)) return;
    const user = try service.createUser(
        arena,
        actor.user,
        try text(body, "username"),
        try text(body, "password"),
        body.object.get("role"),
        body.object.get("quota"),
    );
    return writeValue(request, .created, try service.serializeUser(arena, user));
}

fn getUser(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    _ = try begin(true);
    const user = store.findUser(try parseId(raw_id), false) orelse return domain.notFound();
    return writeValue(request, .ok, try service.serializeUser(arena, user));
}

fn updateUser(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(true);
    const user = store.findUser(try parseId(raw_id), false) orelse return domain.notFound();
    try service.checkIfMatch(if_match, user.version);
    const body = try readBody(request, arena);
    try service.updateUser(arena, actor.user, user, body);
    return writeValue(request, .ok, try service.serializeUser(arena, user));
}

fn deleteUser(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(true);
    const user = store.findUser(try parseId(raw_id), false) orelse return domain.notFound();
    try service.checkIfMatch(if_match, user.version);
    try service.deleteUser(actor.user, user);
    return writeValue(request, .ok, try service.serializeUser(arena, user));
}

// --------------------------------------------------------------------- projects

fn listProjects(request: *Request, arena: Allocator, query: []const u8) !void {
    const actor = try begin(false);
    const include = try service.checkIncludeDeleted(queryGet(query, "includeDeleted"), actor.user);
    const input = try readPage(arena, query, &domain.project_sorts);
    var rows: std.ArrayList(Value) = .empty;
    for (try service.visibleProjects(arena, actor.user, include)) |project| {
        try rows.append(arena, try service.serializeProject(arena, project));
    }
    return writePage(request, rows.items, input);
}

fn createProject(request: *Request, arena: Allocator) !void {
    const actor = try begin(true);
    const body = try readBody(request, arena);
    if (try tryReplay(request, arena, actor.token)) return;
    const project = try service.createProject(
        arena,
        actor.user,
        try text(body, "name"),
        try whole(body, "ownerId", actor.user.id),
    );
    return writeValue(request, .created, try service.serializeProject(arena, project));
}

fn getProject(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, false);
    return writeValue(request, .ok, try service.serializeProject(arena, project));
}

fn updateProject(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(true);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, false);
    try service.checkIfMatch(if_match, project.version);
    const body = try readBody(request, arena);
    if (body.object.get("name") != null) {
        try service.renameProject(arena, actor.user, project, try text(body, "name"));
    }
    return writeValue(request, .ok, try service.serializeProject(arena, project));
}

fn deleteProject(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(true);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, false);
    try service.checkIfMatch(if_match, project.version);
    try service.deleteProject(actor.user, project);
    return writeValue(request, .ok, try service.serializeProject(arena, project));
}

fn restoreProject(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(true);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, true);
    try service.checkIfMatch(if_match, project.version);
    try service.restoreProject(actor.user, project);
    return writeValue(request, .ok, try service.serializeProject(arena, project));
}

// ----------------------------------------------------------------------- tasks

fn taskFilters(arena: Allocator, query: []const u8, rows: []*domain.Task) ![]*domain.Task {
    var errors: Errors = .empty;
    const status = queryGet(query, "status");
    var assignee: ?i64 = null;
    if (status) |value| {
        if (!domain.isStatus(value)) {
            try errors.append(arena, domain.fail("status", "status is not valid"));
        }
    }
    if (queryGet(query, "assigneeId")) |raw| {
        assignee = std.fmt.parseInt(i64, raw, 10) catch null;
        if (assignee == null) {
            try errors.append(arena, domain.fail("assigneeId", "assigneeId is not a known user"));
        }
    }
    if (errors.items.len > 0) return domain.invalid(errors.items);
    var kept: std.ArrayList(*domain.Task) = .empty;
    for (rows) |task| {
        if (status) |value| {
            if (!std.mem.eql(u8, task.status, value)) continue;
        }
        if (assignee) |value| {
            if (task.assigneeId == null or task.assigneeId.? != value) continue;
        }
        try kept.append(arena, task);
    }
    return kept.items;
}

fn listAllTasks(request: *Request, arena: Allocator, query: []const u8) !void {
    const actor = try begin(false);
    const include = try service.checkIncludeDeleted(queryGet(query, "includeDeleted"), actor.user);
    const input = try readPage(arena, query, &domain.task_sorts);
    const visible = try service.visibleTasks(arena, actor.user, include);
    var rows: std.ArrayList(Value) = .empty;
    for (try taskFilters(arena, query, visible)) |task| {
        try rows.append(arena, try service.serializeTask(arena, task, actor.user.role));
    }
    return writePage(request, rows.items, input);
}

fn listTasks(request: *Request, arena: Allocator, raw_id: []const u8, query: []const u8) !void {
    const actor = try begin(false);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, false);
    const input = try readPage(arena, query, &domain.task_sorts);
    var rows: std.ArrayList(Value) = .empty;
    for (store.tasks.values()) |*task| {
        if (task.projectId == project.id and !task.deleted) {
            try rows.append(arena, try service.serializeTask(arena, task, actor.user.role));
        }
    }
    return writePage(request, rows.items, input);
}

fn createTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const project = try service.reachableProject(try parseId(raw_id), actor.user, false);
    const body = try readBody(request, arena);
    if (try tryReplay(request, arena, actor.token)) return;
    var errors: Errors = .empty;
    const note = try service.readNote(arena, actor.user, body, &errors, "");
    const task = try service.createTask(
        arena,
        actor.user,
        project,
        try text(body, "title"),
        try whole(body, "priority", 0),
        try whole(body, "assigneeId", null),
        note,
        &errors,
    );
    return writeValue(request, .created, try service.serializeTask(arena, task, actor.user.role));
}

fn getTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    return writeValue(request, .ok, try service.serializeTask(arena, task, actor.user.role));
}

fn replaceTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    try service.checkIfMatch(if_match, task.version);
    const body = try readBody(request, arena);
    var errors: Errors = .empty;
    const note = try service.readNote(arena, actor.user, body, &errors, task.internalNote);
    try service.replaceTask(
        arena,
        actor.user,
        task,
        try text(body, "title"),
        try whole(body, "priority", 0),
        try whole(body, "assigneeId", null),
        note,
        &errors,
    );
    return writeValue(request, .ok, try service.serializeTask(arena, task, actor.user.role));
}

fn updateStatus(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    try service.checkIfMatch(if_match, task.version);
    const body = try readBody(request, arena);
    try service.moveStatus(arena, actor.user, task, body.object.get("status"));
    return writeValue(request, .ok, try service.serializeTask(arena, task, actor.user.role));
}

fn deleteTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    try service.checkIfMatch(if_match, task.version);
    try service.deleteTask(actor.user, task);
    return writeValue(request, .ok, try service.serializeTask(arena, task, actor.user.role));
}

fn restoreTask(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, true);
    try service.checkIfMatch(if_match, task.version);
    try service.restoreTask(actor.user, task);
    return writeValue(request, .ok, try service.serializeTask(arena, task, actor.user.role));
}

const BulkResult = struct { index: i64, status: u16, id: ?i64, @"error": ?[]const u8 };

fn itemVersion(arena: Allocator, item: Value, version: i64) !void {
    const raw = switch (item.object.get("version") orelse Value{ .null = {} }) {
        .integer => |number| try std.fmt.allocPrint(arena, "{d}", .{number}),
        else => "absent",
    };
    return service.checkIfMatch(raw, version);
}

fn applyBulk(arena: Allocator, actor: domain.User, index: i64, item: Value) !BulkResult {
    if (item != .object) return domain.badRequest();
    const operation = switch (item.object.get("op") orelse Value{ .null = {} }) {
        .string => |found| found,
        else => "",
    };
    if (std.mem.eql(u8, operation, "create")) {
        const project = try service.reachableProject(
            (try whole(item, "projectId", 0)) orelse 0,
            actor,
            false,
        );
        var errors: Errors = .empty;
        const task = try service.createTask(
            arena,
            actor,
            project,
            try text(item, "title"),
            try whole(item, "priority", 0),
            null,
            "",
            &errors,
        );
        return .{ .index = index, .status = 201, .id = task.id, .@"error" = null };
    }
    if (std.mem.eql(u8, operation, "status")) {
        const task = try service.reachableTask((try whole(item, "id", 0)) orelse 0, actor, false);
        try itemVersion(arena, item, task.version);
        try service.moveStatus(arena, actor, task, item.object.get("status"));
        return .{ .index = index, .status = 200, .id = task.id, .@"error" = null };
    }
    if (std.mem.eql(u8, operation, "delete")) {
        const task = try service.reachableTask((try whole(item, "id", 0)) orelse 0, actor, false);
        try itemVersion(arena, item, task.version);
        try service.deleteTask(actor, task);
        return .{ .index = index, .status = 200, .id = task.id, .@"error" = null };
    }
    return oneError(arena, "op", "op is not valid");
}

fn bulkTasks(request: *Request, arena: Allocator) !void {
    const actor = try begin(false);
    const body = try readBody(request, arena);
    const operations = body.object.get("operations");
    try service.checkBulkSize(arena, operations);
    var results: std.ArrayList(BulkResult) = .empty;
    for (operations.?.array.items, 0..) |item, index| {
        const outcome = applyBulk(arena, actor.user, @intCast(index), item) catch |err| switch (err) {
            error.App => BulkResult{
                .index = @intCast(index),
                .status = @intFromEnum(domain.app_error.status),
                .id = null,
                .@"error" = domain.app_error.code,
            },
            else => return err,
        };
        try results.append(arena, outcome);
    }
    return writeJson(request, .ok, .{ .results = results.items });
}

// --------------------------------------------------------------------- comments

fn listComments(request: *Request, arena: Allocator, raw_id: []const u8, query: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    const input = try readPage(arena, query, &domain.comment_sorts);
    var rows: std.ArrayList(Value) = .empty;
    for (store.comments.values()) |*comment| {
        if (comment.taskId == task.id) {
            try rows.append(arena, try service.serializeComment(arena, comment));
        }
    }
    return writePage(request, rows.items, input);
}

fn createComment(request: *Request, arena: Allocator, raw_id: []const u8) !void {
    const actor = try begin(false);
    const task = try service.reachableTask(try parseId(raw_id), actor.user, false);
    const body = try readBody(request, arena);
    if (try tryReplay(request, arena, actor.token)) return;
    const comment = try service.createComment(arena, actor.user, task, try text(body, "body"));
    return writeValue(request, .created, try service.serializeComment(arena, comment));
}

fn deleteComment(request: *Request, raw_id: []const u8) !void {
    const actor = try begin(false);
    const comment = store.findComment(try parseId(raw_id)) orelse return domain.notFound();
    _ = try service.reachableTask(comment.taskId, actor.user, true);
    try service.removeComment(actor.user, comment);
    return respond(request, .no_content, "");
}

// ---------------------------------------------------- search, reports, telemetry

fn search(request: *Request, arena: Allocator, query: []const u8) !void {
    const actor = try begin(false);
    const wanted = queryGet(query, "q") orelse "";
    if (wanted.len == 0) return oneError(arena, "q", "q is required");
    return writeJson(request, .ok, try service.search(arena, actor.user, wanted));
}

fn workload(request: *Request, arena: Allocator, query: []const u8) !void {
    const actor = try begin(false);
    const group_by = queryGet(query, "groupBy") orelse "status";
    if (!domain.isGroupBy(group_by)) return oneError(arena, "groupBy", "groupBy is not valid");
    return writeJson(request, .ok, try service.workload(arena, actor.user, group_by));
}

fn listAudit(request: *Request, arena: Allocator, query: []const u8) !void {
    _ = try begin(true);
    const input = try readPage(arena, query, &domain.seq_sorts);
    const actor_id = queryGet(query, "actorId");
    const resource = queryGet(query, "resource");
    const action = queryGet(query, "action");
    var rows: std.ArrayList(Value) = .empty;
    for (store.audit.items) |*entry| {
        if (actor_id) |wanted| {
            var buffer: [24]u8 = undefined;
            const own = try std.fmt.bufPrint(&buffer, "{d}", .{entry.actorId});
            if (!std.mem.eql(u8, own, wanted)) continue;
        }
        if (resource) |wanted| {
            if (!std.mem.eql(u8, entry.resource, wanted)) continue;
        }
        if (action) |wanted| {
            if (!std.mem.eql(u8, entry.action, wanted)) continue;
        }
        try rows.append(arena, try service.serializeAudit(arena, entry));
    }
    return writePage(request, rows.items, input);
}

fn listOutbox(request: *Request, arena: Allocator, query: []const u8) !void {
    _ = try begin(true);
    const input = try readPage(arena, query, &domain.seq_sorts);
    const wanted = queryGet(query, "delivered");
    var rows: std.ArrayList(Value) = .empty;
    for (store.outbox.items) |*event| {
        if (wanted) |raw| {
            if (event.delivered != std.mem.eql(u8, raw, "true")) continue;
        }
        try rows.append(arena, try service.serializeOutbox(arena, event));
    }
    return writePage(request, rows.items, input);
}

fn flushOutbox(request: *Request) !void {
    _ = try begin(true);
    return writeJson(request, .ok, .{ .flushed = service.flushOutbox() });
}

fn getMetrics(request: *Request, arena: Allocator) !void {
    _ = try begin(true);
    return writeJson(request, .ok, try service.metrics(arena));
}

fn getStats(request: *Request, arena: Allocator) !void {
    _ = try begin(true);
    return writeJson(request, .ok, try service.stats(arena));
}

// -------------------------------------------------------------------- the router

fn dispatch(
    request: *Request,
    arena: Allocator,
    method: http.Method,
    path: []const u8,
    query: []const u8,
) !void {
    if (std.mem.eql(u8, path, "/health")) {
        route_pattern = "/health";
        return getHealth(request);
    }
    if (std.mem.eql(u8, path, "/auth/login")) {
        route_pattern = "/auth/login";
        return login(request, arena);
    }
    if (std.mem.eql(u8, path, "/auth/logout")) {
        route_pattern = "/auth/logout";
        return logout(request);
    }
    if (std.mem.eql(u8, path, "/me")) {
        route_pattern = "/me";
        return getMe(request);
    }
    if (std.mem.eql(u8, path, "/search")) {
        route_pattern = "/search";
        return search(request, arena, query);
    }
    if (std.mem.eql(u8, path, "/reports/workload")) {
        route_pattern = "/reports/workload";
        return workload(request, arena, query);
    }
    if (std.mem.eql(u8, path, "/audit")) {
        route_pattern = "/audit";
        return listAudit(request, arena, query);
    }
    if (std.mem.eql(u8, path, "/outbox")) {
        route_pattern = "/outbox";
        return listOutbox(request, arena, query);
    }
    if (std.mem.eql(u8, path, "/outbox/flush")) {
        route_pattern = "/outbox/flush";
        return flushOutbox(request);
    }
    if (std.mem.eql(u8, path, "/metrics")) {
        route_pattern = "/metrics";
        return getMetrics(request, arena);
    }
    if (std.mem.eql(u8, path, "/stats")) {
        route_pattern = "/stats";
        return getStats(request, arena);
    }
    if (std.mem.eql(u8, path, "/users")) {
        route_pattern = "/users";
        return switch (method) {
            .POST => createUser(request, arena),
            else => listUsers(request, arena, query),
        };
    }
    if (std.mem.startsWith(u8, path, "/users/")) {
        const raw_id = path["/users/".len..];
        if (std.mem.findScalar(u8, raw_id, '/') == null) {
            route_pattern = "/users/{id}";
            return switch (method) {
                .PATCH => updateUser(request, arena, raw_id),
                .DELETE => deleteUser(request, arena, raw_id),
                else => getUser(request, arena, raw_id),
            };
        }
    }
    if (std.mem.eql(u8, path, "/projects")) {
        route_pattern = "/projects";
        return switch (method) {
            .POST => createProject(request, arena),
            else => listProjects(request, arena, query),
        };
    }
    if (std.mem.startsWith(u8, path, "/projects/")) {
        const rest = path["/projects/".len..];
        const cut = std.mem.findScalar(u8, rest, '/');
        const raw_id = if (cut) |index| rest[0..index] else rest;
        const tail = if (cut) |index| rest[index..] else "";
        if (tail.len == 0) {
            route_pattern = "/projects/{id}";
            return switch (method) {
                .PATCH => updateProject(request, arena, raw_id),
                .DELETE => deleteProject(request, arena, raw_id),
                else => getProject(request, arena, raw_id),
            };
        }
        if (std.mem.eql(u8, tail, "/tasks")) {
            route_pattern = "/projects/{id}/tasks";
            return switch (method) {
                .POST => createTask(request, arena, raw_id),
                else => listTasks(request, arena, raw_id, query),
            };
        }
        if (std.mem.eql(u8, tail, "/restore")) {
            route_pattern = "/projects/{id}/restore";
            return restoreProject(request, arena, raw_id);
        }
    }
    if (std.mem.eql(u8, path, "/tasks")) {
        route_pattern = "/tasks";
        return listAllTasks(request, arena, query);
    }
    if (std.mem.eql(u8, path, "/tasks/bulk")) {
        route_pattern = "/tasks/bulk";
        return bulkTasks(request, arena);
    }
    if (std.mem.startsWith(u8, path, "/tasks/")) {
        const rest = path["/tasks/".len..];
        const cut = std.mem.findScalar(u8, rest, '/');
        const raw_id = if (cut) |index| rest[0..index] else rest;
        const tail = if (cut) |index| rest[index..] else "";
        if (tail.len == 0) {
            route_pattern = "/tasks/{id}";
            return switch (method) {
                .PUT => replaceTask(request, arena, raw_id),
                .DELETE => deleteTask(request, arena, raw_id),
                else => getTask(request, arena, raw_id),
            };
        }
        if (std.mem.eql(u8, tail, "/status")) {
            route_pattern = "/tasks/{id}/status";
            return updateStatus(request, arena, raw_id);
        }
        if (std.mem.eql(u8, tail, "/restore")) {
            route_pattern = "/tasks/{id}/restore";
            return restoreTask(request, arena, raw_id);
        }
        if (std.mem.eql(u8, tail, "/comments")) {
            route_pattern = "/tasks/{id}/comments";
            return switch (method) {
                .POST => createComment(request, arena, raw_id),
                else => listComments(request, arena, raw_id, query),
            };
        }
    }
    if (std.mem.startsWith(u8, path, "/comments/")) {
        const raw_id = path["/comments/".len..];
        if (std.mem.findScalar(u8, raw_id, '/') == null) {
            route_pattern = "/comments/{id}";
            return deleteComment(request, raw_id);
        }
    }
    return domain.notFound();
}

fn observe(request: *Request, arena: Allocator) !void {
    const started: std.Io.Timestamp = .now(io, .awake);
    const method = request.head.method;
    const method_name = @tagName(method);
    const target = try arena.dupe(u8, request.head.target);
    request_id = "";
    authorization = "";
    if_match = null;
    idempotency_key = null;
    idem_slot = null;
    user_id = null;
    quota_remaining = null;
    quota_text = null;
    etag = null;
    etag_version = null;
    replayed = false;
    log_status = 200;
    route_pattern = "/{path}";
    const audit_before = store.audit.items.len;
    var headers = request.iterateHeaders();
    while (headers.next()) |header| {
        if (std.ascii.eqlIgnoreCase(header.name, "authorization")) {
            authorization = try arena.dupe(u8, header.value);
        } else if (std.ascii.eqlIgnoreCase(header.name, "x-request-id") and header.value.len > 0) {
            request_id = try arena.dupe(u8, header.value);
        } else if (std.ascii.eqlIgnoreCase(header.name, "if-match")) {
            if_match = try arena.dupe(u8, header.value);
        } else if (std.ascii.eqlIgnoreCase(header.name, "idempotency-key")) {
            idempotency_key = try arena.dupe(u8, header.value);
        }
    }
    if (request_id.len == 0) request_id = try newId(arena);
    const cut = std.mem.findScalar(u8, target, '?');
    const path = if (cut) |index| target[0..index] else target;
    const query = if (cut) |index| target[index + 1 ..] else "";
    dispatch(request, arena, method, path, query) catch |err| switch (err) {
        error.App => {
            etag = null;
            etag_version = null;
            try writeError(request);
        },
        else => return err,
    };
    const label = try std.fmt.allocPrint(arena, "{s} {s}", .{ method_name, route_pattern });
    try store.countRequest(label, log_status);
    var buffer: [1024]u8 = undefined;
    var writer: std.Io.Writer = .fixed(&buffer);
    try std.json.Stringify.value(.{
        .level = if (log_status >= 500) "error" else if (log_status >= 400) "warn" else "info",
        .requestId = request_id,
        .method = method_name,
        .path = path,
        .status = log_status,
        .durationMs = started.durationTo(.now(io, .awake)).toMilliseconds(),
        .userId = user_id,
        .quotaRemaining = quota_remaining,
        .auditSeq = store.audit.items.len - audit_before,
    }, .{}, &writer);
    try writer.writeByte('\n');
    try std.Io.File.stdout().writeStreamingAll(io, writer.buffered());
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init(gpa, .{});
    defer threaded.deinit();
    io = threaded.io();

    try store.seed();

    const address: std.Io.net.IpAddress = .{ .ip4 = .loopback(domain.port) };
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
