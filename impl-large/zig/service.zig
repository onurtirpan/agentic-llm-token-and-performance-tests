// Task Service, large tier — business rules, authorization and audit emission.
const std = @import("std");
const domain = @import("domain.zig");
const store = @import("store.zig");
const Allocator = std.mem.Allocator;
const Value = std.json.Value;
const Detail = domain.Detail;
const Errors = std.ArrayList(Detail);

const null_value: Value = .{ .null = {} };

// ------------------------------------------------------------------ serializers

fn text(value: []const u8) Value {
    return .{ .string = value };
}

fn whole(value: i64) Value {
    return .{ .integer = value };
}

fn maybe(value: ?i64) Value {
    return if (value) |number| .{ .integer = number } else null_value;
}

pub fn serializeUser(arena: Allocator, user: *const domain.User) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "id", whole(user.id));
    try map.put(arena, "username", text(user.username));
    try map.put(arena, "role", text(user.role));
    try map.put(arena, "quota", whole(user.quota));
    try map.put(arena, "version", whole(user.version));
    try map.put(arena, "deleted", .{ .bool = user.deleted });
    return .{ .object = map };
}

pub fn serializeProject(arena: Allocator, project: *const domain.Project) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "id", whole(project.id));
    try map.put(arena, "name", text(project.name));
    try map.put(arena, "ownerId", whole(project.ownerId));
    try map.put(arena, "taskCount", whole(store.taskCount(project.id)));
    try map.put(arena, "version", whole(project.version));
    try map.put(arena, "deleted", .{ .bool = project.deleted });
    return .{ .object = map };
}

pub fn serializeTask(arena: Allocator, task: *const domain.Task, role: []const u8) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "id", whole(task.id));
    try map.put(arena, "projectId", whole(task.projectId));
    try map.put(arena, "title", text(task.title));
    try map.put(arena, "priority", whole(task.priority));
    try map.put(arena, "status", text(task.status));
    try map.put(arena, "assigneeId", maybe(task.assigneeId));
    if (std.mem.eql(u8, role, "admin")) try map.put(arena, "internalNote", text(task.internalNote));
    try map.put(arena, "version", whole(task.version));
    try map.put(arena, "deleted", .{ .bool = task.deleted });
    try map.put(arena, "score", whole(domain.computeScore(task.priority, task.status)));
    return .{ .object = map };
}

pub fn serializeComment(arena: Allocator, comment: *const domain.Comment) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "id", whole(comment.id));
    try map.put(arena, "taskId", whole(comment.taskId));
    try map.put(arena, "authorId", whole(comment.authorId));
    try map.put(arena, "body", text(comment.body));
    return .{ .object = map };
}

pub fn serializeAudit(arena: Allocator, entry: *const domain.AuditEntry) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "seq", whole(entry.seq));
    try map.put(arena, "actorId", whole(entry.actorId));
    try map.put(arena, "action", text(entry.action));
    try map.put(arena, "resource", text(entry.resource));
    try map.put(arena, "resourceId", whole(entry.resourceId));
    return .{ .object = map };
}

pub fn serializeOutbox(arena: Allocator, event: *const domain.OutboxEvent) !Value {
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "seq", whole(event.seq));
    try map.put(arena, "name", text(event.name));
    try map.put(arena, "resourceId", whole(event.resourceId));
    try map.put(arena, "delivered", .{ .bool = event.delivered });
    return .{ .object = map };
}

// ----------------------------------------------------------------- access rules

pub const Actor = struct { user: domain.User, token: []const u8 };

pub fn authenticate(header: []const u8) domain.Error!Actor {
    const raw = if (std.mem.startsWith(u8, header, "Bearer ")) header["Bearer ".len..] else "";
    const session = store.sessions.getPtr(raw) orelse return domain.unauthorized();
    const user = store.findUser(session.userId, false) orelse return domain.unauthorized();
    return .{ .user = user.*, .token = session.token };
}

pub fn chargeQuota(actor: Actor) domain.Error!i64 {
    const session = store.sessions.getPtr(actor.token) orelse return domain.unauthorized();
    if (session.used >= actor.user.quota) return domain.quotaExceeded();
    session.used += 1;
    return @max(actor.user.quota - session.used, 0);
}

pub fn isAdmin(user: domain.User) bool {
    return std.mem.eql(u8, user.role, "admin");
}

pub fn requireAdmin(user: domain.User) domain.Error!void {
    if (!isAdmin(user)) return domain.forbidden();
}

pub fn reachableProject(
    project_id: i64,
    user: domain.User,
    include_deleted: bool,
) domain.Error!*domain.Project {
    const project = store.findProject(project_id, include_deleted) orelse return domain.notFound();
    if (!isAdmin(user) and project.ownerId != user.id) return domain.forbidden();
    return project;
}

pub fn reachableTask(task_id: i64, user: domain.User, include_deleted: bool) domain.Error!*domain.Task {
    const task = store.findTask(task_id, include_deleted) orelse return domain.notFound();
    _ = try reachableProject(task.projectId, user, true);
    return task;
}

pub fn checkIfMatch(header: ?[]const u8, version: i64) domain.Error!void {
    const value = header orelse return domain.preconditionRequired();
    if (value.len == 0) return domain.preconditionRequired();
    var buffer: [24]u8 = undefined;
    const want = std.fmt.bufPrint(&buffer, "{d}", .{version}) catch return domain.preconditionFailed();
    if (!std.mem.eql(u8, value, want)) return domain.preconditionFailed();
}

pub fn checkIncludeDeleted(raw: ?[]const u8, user: domain.User) domain.Error!bool {
    const value = raw orelse return false;
    if (!isAdmin(user)) return domain.forbidden();
    return std.mem.eql(u8, value, "true");
}

// ------------------------------------------------------------------- pagination

pub const Input = struct {
    limit: i64 = domain.default_limit,
    offset: i64 = 0,
    sort: []const u8 = "id",
    order: []const u8 = "asc",
};
pub const Page = struct { items: []Value, total: usize, limit: i64, offset: i64 };

const SortBy = struct { field: []const u8, desc: bool };

fn lessValue(first: Value, second: Value) bool {
    return switch (first) {
        .integer => |left| switch (second) {
            .integer => |right| left < right,
            else => false,
        },
        .string => |left| switch (second) {
            .string => |right| std.mem.order(u8, left, right) == .lt,
            else => false,
        },
        else => false,
    };
}

fn byField(sort_by: SortBy, a: Value, b: Value) bool {
    const first = a.object.get(sort_by.field) orelse null_value;
    const second = b.object.get(sort_by.field) orelse null_value;
    if (sort_by.desc) return lessValue(second, first);
    return lessValue(first, second);
}

/// Sort by the tiebreak first, then stably by the requested field.
pub fn paginate(rows: []Value, input: Input) Page {
    const tiebreak: []const u8 =
        if (rows.len > 0 and rows[0].object.contains("seq")) "seq" else "id";
    std.mem.sort(Value, rows, SortBy{ .field = tiebreak, .desc = false }, byField);
    std.mem.sort(Value, rows, SortBy{
        .field = input.sort,
        .desc = std.mem.eql(u8, input.order, "desc"),
    }, byField);
    const start = @min(@as(usize, @intCast(input.offset)), rows.len);
    const end = @min(start + @as(usize, @intCast(input.limit)), rows.len);
    return .{ .items = rows[start..end], .total = rows.len, .limit = input.limit, .offset = input.offset };
}

// ------------------------------------------------------------------------- auth

pub fn login(username: []const u8, password: []const u8, token: []const u8) !domain.User {
    const found = store.findByUsername(username) orelse return domain.invalidCredentials();
    if (!std.mem.eql(u8, found.password, password)) return domain.invalidCredentials();
    const user = found.*;
    try store.sessions.put(store.gpa, token, .{ .token = token, .userId = user.id });
    return user;
}

// --------------------------------------------------------------------- projects

pub fn createProject(
    arena: Allocator,
    actor: domain.User,
    name: []const u8,
    owner_id: ?i64,
) !*domain.Project {
    var errors: Errors = .empty;
    try domain.checkString(name, "name", domain.max_name_length, &errors, arena);
    const owner = owner_id orelse -1;
    if (store.findUser(owner, false) == null) {
        try errors.append(arena, domain.fail("ownerId", "ownerId is not a known user"));
    }
    if (errors.items.len > 0) return domain.invalid(errors.items);
    for (store.projects.values()) |other| {
        if (other.ownerId == owner and std.mem.eql(u8, other.name, name) and !other.deleted) {
            return domain.conflict();
        }
    }
    const project = try store.insertProject(name, owner);
    try store.record(actor.id, "create", "project", project.id);
    return project;
}

pub fn renameProject(
    arena: Allocator,
    actor: domain.User,
    project: *domain.Project,
    name: []const u8,
) !void {
    var errors: Errors = .empty;
    try domain.checkString(name, "name", domain.max_name_length, &errors, arena);
    if (errors.items.len > 0) return domain.invalid(errors.items);
    for (store.projects.values()) |other| {
        if (other.ownerId == project.ownerId and std.mem.eql(u8, other.name, name) and
            other.id != project.id and !other.deleted) return domain.conflict();
    }
    project.name = try store.gpa.dupe(u8, name);
    project.version += 1;
    try store.record(actor.id, "update", "project", project.id);
}

pub fn deleteProject(actor: domain.User, project: *domain.Project) !void {
    project.deleted = true;
    project.version += 1;
    try store.record(actor.id, "delete", "project", project.id);
    for (store.tasks.values()) |*task| {
        if (task.projectId == project.id and !task.deleted) {
            task.deleted = true;
            task.version += 1;
            try store.record(actor.id, "delete", "task", task.id);
        }
    }
}

pub fn restoreProject(actor: domain.User, project: *domain.Project) !void {
    if (!project.deleted) return domain.conflict();
    project.deleted = false;
    project.version += 1;
    try store.record(actor.id, "restore", "project", project.id);
}

// ------------------------------------------------------------------------ tasks

pub fn readNote(
    arena: Allocator,
    actor: domain.User,
    body: Value,
    errors: *Errors,
    current: []const u8,
) ![]const u8 {
    const raw = body.object.get("internalNote") orelse return current;
    if (!isAdmin(actor)) return domain.forbidden();
    const note = switch (raw) {
        .string => |value| value,
        else => return domain.badRequest(),
    };
    if (domain.textLength(note) > domain.max_title_length) {
        try errors.append(arena, domain.fail("internalNote", "internalNote is too long"));
    }
    return note;
}

fn checkTaskFields(
    arena: Allocator,
    title: []const u8,
    priority: ?i64,
    assignee_id: ?i64,
    errors: *Errors,
) !void {
    try domain.checkString(title, "title", domain.max_title_length, errors, arena);
    try domain.checkPriority(priority, errors, arena);
    if (assignee_id) |id| {
        if (store.findUser(id, false) == null) {
            try errors.append(arena, domain.fail("assigneeId", "assigneeId is not a known user"));
        }
    }
    if (errors.items.len > 0) return domain.invalid(errors.items);
}

pub fn createTask(
    arena: Allocator,
    actor: domain.User,
    project: *const domain.Project,
    title: []const u8,
    priority: ?i64,
    assignee_id: ?i64,
    note: []const u8,
    errors: *Errors,
) !*domain.Task {
    try checkTaskFields(arena, title, priority, assignee_id, errors);
    const task = try store.insertTask(project.id, title, priority.?, assignee_id, note);
    try store.record(actor.id, "create", "task", task.id);
    return task;
}

pub fn replaceTask(
    arena: Allocator,
    actor: domain.User,
    task: *domain.Task,
    title: []const u8,
    priority: ?i64,
    assignee_id: ?i64,
    note: []const u8,
    errors: *Errors,
) !void {
    try checkTaskFields(arena, title, priority, assignee_id, errors);
    task.title = try store.gpa.dupe(u8, title);
    task.priority = priority.?;
    task.assigneeId = assignee_id;
    task.internalNote = try store.gpa.dupe(u8, note);
    task.version += 1;
    try store.record(actor.id, "update", "task", task.id);
}

pub fn moveStatus(arena: Allocator, actor: domain.User, task: *domain.Task, wanted: ?Value) !void {
    var errors: Errors = .empty;
    try domain.checkStatus(wanted orelse null_value, &errors, arena);
    if (errors.items.len > 0) return domain.invalid(errors.items);
    const next = wanted.?.string;
    if (!domain.allowedMove(task.status, next)) return domain.invalidTransition();
    for (domain.statuses) |name| {
        if (std.mem.eql(u8, name, next)) task.status = name;
    }
    task.version += 1;
    try store.record(actor.id, "update", "task", task.id);
}

pub fn deleteTask(actor: domain.User, task: *domain.Task) !void {
    task.deleted = true;
    task.version += 1;
    try store.record(actor.id, "delete", "task", task.id);
}

pub fn restoreTask(actor: domain.User, task: *domain.Task) !void {
    if (!task.deleted) return domain.conflict();
    task.deleted = false;
    task.version += 1;
    try store.record(actor.id, "restore", "task", task.id);
}

// --------------------------------------------------------------------- comments

pub fn createComment(
    arena: Allocator,
    actor: domain.User,
    task: *const domain.Task,
    body: []const u8,
) !*domain.Comment {
    var errors: Errors = .empty;
    try domain.checkString(body, "body", domain.max_comment_length, &errors, arena);
    if (errors.items.len > 0) return domain.invalid(errors.items);
    const comment = try store.insertComment(task.id, actor.id, body);
    try store.record(actor.id, "create", "comment", comment.id);
    return comment;
}

pub fn removeComment(actor: domain.User, comment: *const domain.Comment) !void {
    if (!isAdmin(actor) and comment.authorId != actor.id) return domain.forbidden();
    const comment_id = comment.id;
    _ = store.comments.orderedRemove(comment_id);
    try store.record(actor.id, "delete", "comment", comment_id);
}

// ------------------------------------------------------------------------ users

pub fn createUser(
    arena: Allocator,
    actor: domain.User,
    username: []const u8,
    password: []const u8,
    role: ?Value,
    quota: ?Value,
) !*domain.User {
    var errors: Errors = .empty;
    try domain.checkString(username, "username", domain.max_name_length, &errors, arena);
    try domain.checkString(password, "password", domain.max_name_length, &errors, arena);
    const role_value = role orelse Value{ .string = "user" };
    const quota_value = quota orelse Value{ .integer = domain.default_quota };
    try domain.checkRole(role_value, &errors, arena);
    try domain.checkQuota(quota_value, &errors, arena);
    if (errors.items.len > 0) return domain.invalid(errors.items);
    if (store.findByUsername(username) != null) return domain.conflict();
    const user = try store.insertUser(username, password, role_value.string, quota_value.integer);
    try store.record(actor.id, "create", "user", user.id);
    return user;
}

pub fn updateUser(arena: Allocator, actor: domain.User, user: *domain.User, body: Value) !void {
    var errors: Errors = .empty;
    if (body.object.get("role")) |value| try domain.checkRole(value, &errors, arena);
    if (body.object.get("quota")) |value| try domain.checkQuota(value, &errors, arena);
    if (errors.items.len > 0) return domain.invalid(errors.items);
    if (body.object.get("role")) |value| user.role = try store.gpa.dupe(u8, value.string);
    if (body.object.get("quota")) |value| user.quota = value.integer;
    user.version += 1;
    try store.record(actor.id, "update", "user", user.id);
}

pub fn deleteUser(actor: domain.User, user: *domain.User) !void {
    if (user.id == actor.id) return domain.conflict();
    user.deleted = true;
    user.version += 1;
    try store.record(actor.id, "delete", "user", user.id);
}

// ---------------------------------------------------------- queries and reports

pub fn visibleProjects(
    arena: Allocator,
    user: domain.User,
    include_deleted: bool,
) ![]*domain.Project {
    var rows: std.ArrayList(*domain.Project) = .empty;
    for (store.projects.values()) |*project| {
        if ((include_deleted or !project.deleted) and
            (isAdmin(user) or project.ownerId == user.id)) try rows.append(arena, project);
    }
    return rows.items;
}

pub fn visibleTasks(arena: Allocator, user: domain.User, include_deleted: bool) ![]*domain.Task {
    var rows: std.ArrayList(*domain.Task) = .empty;
    for (store.tasks.values()) |*task| {
        const project = store.findProject(task.projectId, true) orelse continue;
        if (!isAdmin(user) and project.ownerId != user.id) continue;
        if (task.deleted and !include_deleted) continue;
        try rows.append(arena, task);
    }
    return rows.items;
}

pub const Hit = struct { @"type": []const u8, id: i64, label: []const u8 };
pub const SearchResult = struct { results: []const Hit, total: usize };

pub fn search(arena: Allocator, user: domain.User, query: []const u8) !SearchResult {
    var hits: std.ArrayList(Hit) = .empty;
    for (try visibleProjects(arena, user, false)) |project| {
        if (domain.containsIgnoreCase(project.name, query)) {
            try hits.append(arena, .{ .@"type" = "project", .id = project.id, .label = project.name });
        }
    }
    for (try visibleTasks(arena, user, false)) |task| {
        if (domain.containsIgnoreCase(task.title, query)) {
            try hits.append(arena, .{ .@"type" = "task", .id = task.id, .label = task.title });
        }
    }
    return .{ .results = hits.items, .total = hits.items.len };
}

pub const Group = struct { key: []const u8, tasks: usize, totalScore: i64 };
pub const Workload = struct { groupBy: []const u8, groups: []const Group };

fn tally(rows: []const *domain.Task) i64 {
    var total: i64 = 0;
    for (rows) |task| total += domain.computeScore(task.priority, task.status);
    return total;
}

pub fn workload(arena: Allocator, user: domain.User, group_by: []const u8) !Workload {
    const rows = try visibleTasks(arena, user, false);
    var groups: std.ArrayList(Group) = .empty;
    var picked: std.ArrayList(*domain.Task) = .empty;
    if (std.mem.eql(u8, group_by, "status")) {
        for (domain.statuses) |status| {
            picked.clearRetainingCapacity();
            for (rows) |task| {
                if (std.mem.eql(u8, task.status, status)) try picked.append(arena, task);
            }
            try groups.append(arena, .{
                .key = status,
                .tasks = picked.items.len,
                .totalScore = tally(picked.items),
            });
        }
    } else if (std.mem.eql(u8, group_by, "assignee")) {
        var named: std.ArrayList(i64) = .empty;
        for (rows) |task| {
            if (task.assigneeId) |id| {
                if (std.mem.indexOfScalar(i64, named.items, id) == null) try named.append(arena, id);
            }
        }
        std.mem.sort(i64, named.items, {}, struct {
            fn lessThan(_: void, a: i64, b: i64) bool {
                return a < b;
            }
        }.lessThan);
        for (named.items) |id| {
            picked.clearRetainingCapacity();
            for (rows) |task| {
                if (task.assigneeId != null and task.assigneeId.? == id) try picked.append(arena, task);
            }
            try groups.append(arena, .{
                .key = try std.fmt.allocPrint(arena, "{d}", .{id}),
                .tasks = picked.items.len,
                .totalScore = tally(picked.items),
            });
        }
        picked.clearRetainingCapacity();
        for (rows) |task| {
            if (task.assigneeId == null) try picked.append(arena, task);
        }
        if (picked.items.len > 0) try groups.append(arena, .{
            .key = "unassigned",
            .tasks = picked.items.len,
            .totalScore = tally(picked.items),
        });
    } else {
        for (try visibleProjects(arena, user, false)) |project| {
            picked.clearRetainingCapacity();
            for (rows) |task| {
                if (task.projectId == project.id) try picked.append(arena, task);
            }
            try groups.append(arena, .{
                .key = project.name,
                .tasks = picked.items.len,
                .totalScore = tally(picked.items),
            });
        }
    }
    return .{ .groupBy = group_by, .groups = groups.items };
}

pub fn flushOutbox() i64 {
    var flushed: i64 = 0;
    for (store.outbox.items) |*event| {
        if (!event.delivered) {
            event.delivered = true;
            flushed += 1;
        }
    }
    return flushed;
}

pub fn metrics(arena: Allocator) !Value {
    var codes: std.ArrayList(u16) = .empty;
    for (store.by_status.keys()) |code| try codes.append(arena, code);
    std.mem.sort(u16, codes.items, {}, struct {
        fn lessThan(_: void, a: u16, b: u16) bool {
            return a < b;
        }
    }.lessThan);
    var by_status: std.json.ObjectMap = .empty;
    for (codes.items) |code| {
        const key = try std.fmt.allocPrint(arena, "{d}", .{code});
        try by_status.put(arena, key, whole(store.by_status.get(code).?));
    }
    var routes: std.ArrayList([]const u8) = .empty;
    for (store.by_route.keys()) |route| try routes.append(arena, route);
    std.mem.sort([]const u8, routes.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lessThan);
    var by_route: std.json.Array = .init(arena);
    for (routes.items) |route| {
        var row: std.json.ObjectMap = .empty;
        try row.put(arena, "route", text(route));
        try row.put(arena, "count", whole(store.by_route.get(route).?));
        try by_route.append(.{ .object = row });
    }
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "requests", whole(store.requests));
    try map.put(arena, "byStatus", .{ .object = by_status });
    try map.put(arena, "byRoute", .{ .array = by_route });
    try map.put(arena, "auditEntries", whole(@intCast(store.audit.items.len)));
    try map.put(arena, "outboxPending", whole(store.outboxPending()));
    return .{ .object = map };
}

pub fn stats(arena: Allocator) !Value {
    var counts = [_]i64{0} ** domain.statuses.len;
    var sum_score: i64 = 0;
    var total: i64 = 0;
    for (store.tasks.values()) |task| {
        if (task.deleted) continue;
        total += 1;
        sum_score += domain.computeScore(task.priority, task.status);
        for (domain.statuses, 0..) |name, index| {
            if (std.mem.eql(u8, name, task.status)) counts[index] += 1;
        }
    }
    var live_projects: i64 = 0;
    var best: ?*const domain.Project = null;
    for (store.projects.values()) |*project| {
        if (project.deleted) continue;
        live_projects += 1;
        if (best == null or store.taskCount(project.id) > store.taskCount(best.?.id)) best = project;
    }
    var live_users: i64 = 0;
    for (store.users.values()) |user| {
        if (!user.deleted) live_users += 1;
    }
    var avg_score: f64 = 0;
    if (total > 0) {
        const mean = @as(f64, @floatFromInt(sum_score)) / @as(f64, @floatFromInt(total));
        avg_score = @round(mean * 100) / 100;
    }
    var by_status: std.json.ObjectMap = .empty;
    for (domain.statuses, counts) |name, count| try by_status.put(arena, name, whole(count));
    var map: std.json.ObjectMap = .empty;
    try map.put(arena, "projects", whole(live_projects));
    try map.put(arena, "tasks", whole(total));
    try map.put(arena, "users", whole(live_users));
    try map.put(arena, "sessions", whole(@intCast(store.sessions.count())));
    try map.put(arena, "comments", whole(@intCast(store.comments.count())));
    try map.put(arena, "byStatus", .{ .object = by_status });
    try map.put(arena, "avgScore", .{ .float = avg_score });
    try map.put(arena, "topProjectName", if (best) |project| text(project.name) else null_value);
    try map.put(arena, "auditEntries", whole(@intCast(store.audit.items.len)));
    try map.put(arena, "outboxPending", whole(store.outboxPending()));
    return .{ .object = map };
}

pub fn checkBulkSize(arena: Allocator, operations: ?Value) !void {
    const count = switch (operations orelse null_value) {
        .array => |items| items.items.len,
        else => 0,
    };
    if (count >= 1 and count <= domain.max_bulk_items) return;
    var errors: Errors = .empty;
    try errors.append(arena, domain.fail("operations", "operations is out of range"));
    return domain.invalid(errors.items);
}
