// Task Service — std.http.Server implementation.
const std = @import("std");
const http = std.http;
const Allocator = std.mem.Allocator;
const Request = http.Server.Request;

const max_title_length = 80;
const min_priority = 1;
const max_priority = 5;
const port = 8080;

const Task = struct {
    id: i64,
    title: []const u8,
    priority: i64,
    done: bool,
    score: i64,
};

const Input = struct {
    title: []const u8 = "",
    priority: i64 = 0,
    done: bool = false,
};

const gpa = std.heap.smp_allocator;

var tasks: std.array_hash_map.Auto(i64, Task) = .empty;
var next_id: i64 = 1;

fn computeScore(priority: i64, done: bool) i64 {
    const base_score = priority * 10;
    if (done) return base_score;
    return base_score + 5;
}

fn validate(title: []const u8, priority: i64) ?[]const u8 {
    if (title.len == 0) return "title is required";
    if ((std.unicode.utf8CountCodepoints(title) catch title.len) > max_title_length) return "title is too long";
    if (priority < min_priority or priority > max_priority) return "priority is out of range";
    return null;
}

fn parseId(raw: []const u8) !i64 {
    return std.fmt.parseInt(i64, raw, 10);
}

fn readInput(request: *Request, arena: Allocator) !Input {
    var buffer: [1024]u8 = undefined;
    const body = try request.readerExpectNone(&buffer).allocRemaining(arena, .limited(64 * 1024));
    return std.json.parseFromSliceLeaky(Input, arena, body, .{});
}

fn sortedTasks() []Task {
    return tasks.values();
}

fn writeJson(request: *Request, status: http.Status, body: anytype) !void {
    var buffer: [16 * 1024]u8 = undefined;
    var writer: std.Io.Writer = .fixed(&buffer);
    try std.json.Stringify.value(body, .{}, &writer);
    try request.respond(writer.buffered(), .{
        .status = status,
        .extra_headers = &.{.{ .name = "content-type", .value = "application/json" }},
    });
}

fn fail(request: *Request, status: http.Status, message: []const u8) !void {
    return writeJson(request, status, .{ .@"error" = message });
}

fn getHealth(request: *Request) !void {
    return writeJson(request, .ok, .{ .status = "ok", .count = tasks.count() });
}

fn listTasks(request: *Request, arena: Allocator, query: []const u8) !void {
    var done: ?bool = null;
    var pairs = std.mem.splitScalar(u8, query, '&');
    while (pairs.next()) |pair| {
        if (!std.mem.startsWith(u8, pair, "done=")) continue;
        const value = pair["done=".len..];
        if (std.mem.eql(u8, value, "true")) {
            done = true;
        } else if (std.mem.eql(u8, value, "false")) {
            done = false;
        } else {
            return fail(request, .bad_request, "done must be true or false");
        }
    }
    var selected: std.ArrayList(Task) = .empty;
    for (sortedTasks()) |task| {
        if (done == null or task.done == done.?) try selected.append(arena, task);
    }
    std.mem.sort(Task, selected.items, {}, struct {
        fn lessThan(_: void, a: Task, b: Task) bool {
            return if (a.score != b.score) a.score > b.score else a.id < b.id;
        }
    }.lessThan);
    return writeJson(request, .ok, .{ .tasks = selected.items, .total = selected.items.len });
}

fn getTask(request: *Request, id: i64) !void {
    const task = tasks.getPtr(id) orelse return fail(request, .not_found, "task not found");
    return writeJson(request, .ok, task.*);
}

fn createTask(request: *Request, arena: Allocator) !void {
    const input = readInput(request, arena) catch return fail(request, .bad_request, "invalid json");
    if (validate(input.title, input.priority)) |message| return fail(request, .bad_request, message);
    const task: Task = .{
        .id = next_id,
        .title = try gpa.dupe(u8, input.title),
        .priority = input.priority,
        .done = false,
        .score = computeScore(input.priority, false),
    };
    try tasks.put(gpa, next_id, task);
    next_id += 1;
    return writeJson(request, .created, task);
}

fn updateTask(request: *Request, arena: Allocator, id: i64) !void {
    const task = tasks.getPtr(id) orelse return fail(request, .not_found, "task not found");
    const input = readInput(request, arena) catch return fail(request, .bad_request, "invalid json");
    if (validate(input.title, input.priority)) |message| return fail(request, .bad_request, message);
    task.title = try gpa.dupe(u8, input.title);
    task.priority = input.priority;
    task.done = input.done;
    task.score = computeScore(input.priority, input.done);
    return writeJson(request, .ok, task.*);
}

fn deleteTask(request: *Request, id: i64) !void {
    if (!tasks.orderedRemove(id)) return fail(request, .not_found, "task not found");
    return request.respond("", .{ .status = .no_content });
}

fn getStats(request: *Request) !void {
    const all = sortedTasks();
    const total = all.len;
    var done_count: usize = 0;
    var sum_score: i64 = 0;
    var best: ?*const Task = null;
    for (all) |*task| {
        if (task.done) done_count += 1;
        sum_score += task.score;
        if (!task.done and (best == null or task.priority > best.?.priority)) best = task;
    }
    var avg_score: f64 = 0;
    if (total > 0) {
        const mean = @as(f64, @floatFromInt(sum_score)) / @as(f64, @floatFromInt(total));
        avg_score = @round(mean * 100) / 100;
    }
    return writeJson(request, .ok, .{
        .total = total,
        .doneCount = done_count,
        .openCount = total - done_count,
        .avgScore = avg_score,
        .topOpenTitle = if (best) |task| task.title else null,
    });
}

fn handleRequest(request: *Request, arena: Allocator) !void {
    const target = request.head.target;
    const cut = std.mem.findScalar(u8, target, '?');
    const path = if (cut) |i| target[0..i] else target;
    const query = if (cut) |i| target[i + 1 ..] else "";
    if (std.mem.eql(u8, path, "/health")) return getHealth(request);
    if (std.mem.eql(u8, path, "/stats")) return getStats(request);
    if (std.mem.eql(u8, path, "/tasks")) return switch (request.head.method) {
        .POST => createTask(request, arena),
        else => listTasks(request, arena, query),
    };
    if (std.mem.startsWith(u8, path, "/tasks/")) {
        const id = parseId(path["/tasks/".len..]) catch return fail(request, .bad_request, "invalid id");
        return switch (request.head.method) {
            .PUT => updateTask(request, arena, id),
            .DELETE => deleteTask(request, id),
            else => getTask(request, id),
        };
    }
    return fail(request, .not_found, "not found");
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init(gpa, .{});
    defer threaded.deinit();
    const io = threaded.io();

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
            handleRequest(&request, arena_state.allocator()) catch break;
        }
    }
}
