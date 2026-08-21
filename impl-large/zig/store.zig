// Task Service, large tier — the in-memory state and its repositories.
const std = @import("std");
const domain = @import("domain.zig");

pub const gpa = std.heap.smp_allocator;

pub const Recorded = struct { status: std.http.Status, body: []const u8, version: ?i64 };

pub var users: std.array_hash_map.Auto(i64, domain.User) = .empty;
pub var sessions: std.array_hash_map.String(domain.Session) = .empty;
pub var projects: std.array_hash_map.Auto(i64, domain.Project) = .empty;
pub var tasks: std.array_hash_map.Auto(i64, domain.Task) = .empty;
pub var comments: std.array_hash_map.Auto(i64, domain.Comment) = .empty;
pub var audit: std.ArrayList(domain.AuditEntry) = .empty;
pub var outbox: std.ArrayList(domain.OutboxEvent) = .empty;
pub var idempotency: std.array_hash_map.String(Recorded) = .empty;
pub var by_status: std.array_hash_map.Auto(u16, i64) = .empty;
pub var by_route: std.array_hash_map.String(i64) = .empty;

pub var requests: i64 = 0;
pub var next_project_id: i64 = 1;
pub var next_task_id: i64 = 1;
pub var next_comment_id: i64 = 1;
pub var next_user_id: i64 = 5;
pub var next_seq: i64 = 1;

pub fn seed() !void {
    try users.put(gpa, 1, .{
        .id = 1,
        .username = "admin",
        .password = "admin-secret",
        .role = "admin",
        .quota = domain.default_quota,
    });
    try users.put(gpa, 2, .{
        .id = 2,
        .username = "alice",
        .password = "alice-secret",
        .role = "user",
        .quota = domain.default_quota,
    });
    try users.put(gpa, 3, .{
        .id = 3,
        .username = "bob",
        .password = "bob-secret",
        .role = "user",
        .quota = domain.default_quota,
    });
    try users.put(gpa, 4, .{
        .id = 4,
        .username = "probe",
        .password = "probe-secret",
        .role = "user",
        .quota = domain.probe_quota,
    });
}

pub fn takeSeq() i64 {
    const value = next_seq;
    next_seq += 1;
    return value;
}

/// One audit entry and one outbox event per successful write, sharing one counter.
pub fn record(actor_id: i64, action: []const u8, resource: []const u8, resource_id: i64) !void {
    try audit.append(gpa, .{
        .seq = takeSeq(),
        .actorId = actor_id,
        .action = action,
        .resource = resource,
        .resourceId = resource_id,
    });
    try outbox.append(gpa, .{
        .seq = takeSeq(),
        .name = try std.fmt.allocPrint(gpa, "{s}.{s}", .{ resource, action }),
        .resourceId = resource_id,
    });
}

pub fn countRequest(route: []const u8, status: u16) !void {
    requests += 1;
    const by_name = try by_route.getOrPut(gpa, route);
    if (!by_name.found_existing) {
        by_name.key_ptr.* = try gpa.dupe(u8, route);
        by_name.value_ptr.* = 0;
    }
    by_name.value_ptr.* += 1;
    const by_code = try by_status.getOrPut(gpa, status);
    if (!by_code.found_existing) by_code.value_ptr.* = 0;
    by_code.value_ptr.* += 1;
}

pub fn findUser(user_id: i64, include_deleted: bool) ?*domain.User {
    const user = users.getPtr(user_id) orelse return null;
    if (user.deleted and !include_deleted) return null;
    return user;
}

pub fn findByUsername(username: []const u8) ?*domain.User {
    for (users.values()) |*user| {
        if (std.mem.eql(u8, user.username, username) and !user.deleted) return user;
    }
    return null;
}

pub fn insertUser(username: []const u8, password: []const u8, role: []const u8, quota: i64) !*domain.User {
    const id = next_user_id;
    try users.put(gpa, id, .{
        .id = id,
        .username = try gpa.dupe(u8, username),
        .password = try gpa.dupe(u8, password),
        .role = try gpa.dupe(u8, role),
        .quota = quota,
    });
    next_user_id += 1;
    return users.getPtr(id).?;
}

pub fn findProject(project_id: i64, include_deleted: bool) ?*domain.Project {
    const project = projects.getPtr(project_id) orelse return null;
    if (project.deleted and !include_deleted) return null;
    return project;
}

pub fn insertProject(name: []const u8, owner_id: i64) !*domain.Project {
    const id = next_project_id;
    try projects.put(gpa, id, .{ .id = id, .name = try gpa.dupe(u8, name), .ownerId = owner_id });
    next_project_id += 1;
    return projects.getPtr(id).?;
}

pub fn findTask(task_id: i64, include_deleted: bool) ?*domain.Task {
    const task = tasks.getPtr(task_id) orelse return null;
    if (task.deleted and !include_deleted) return null;
    return task;
}

pub fn insertTask(
    project_id: i64,
    title: []const u8,
    priority: i64,
    assignee_id: ?i64,
    internal_note: []const u8,
) !*domain.Task {
    const id = next_task_id;
    try tasks.put(gpa, id, .{
        .id = id,
        .projectId = project_id,
        .title = try gpa.dupe(u8, title),
        .priority = priority,
        .status = "todo",
        .assigneeId = assignee_id,
        .internalNote = try gpa.dupe(u8, internal_note),
    });
    next_task_id += 1;
    return tasks.getPtr(id).?;
}

pub fn findComment(comment_id: i64) ?*domain.Comment {
    return comments.getPtr(comment_id);
}

pub fn insertComment(task_id: i64, author_id: i64, body: []const u8) !*domain.Comment {
    const id = next_comment_id;
    try comments.put(gpa, id, .{
        .id = id,
        .taskId = task_id,
        .authorId = author_id,
        .body = try gpa.dupe(u8, body),
    });
    next_comment_id += 1;
    return comments.getPtr(id).?;
}

pub fn taskCount(project_id: i64) i64 {
    var count: i64 = 0;
    for (tasks.values()) |task| {
        if (task.projectId == project_id and !task.deleted) count += 1;
    }
    return count;
}

pub fn outboxPending() i64 {
    var count: i64 = 0;
    for (outbox.items) |event| {
        if (!event.delivered) count += 1;
    }
    return count;
}
