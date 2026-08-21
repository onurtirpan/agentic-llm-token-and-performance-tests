// Task Service, mid tier — ASP.NET Core Minimal API implementation.

using System.Diagnostics;
using System.Text.Json;

const int MaxTitleLength = 80;
const int MaxNameLength = 60;
const int MinPriority = 1;
const int MaxPriority = 5;
const int DefaultLimit = 20;
const int MaxLimit = 100;
const int Port = 8080;

var StatusBonus = new Dictionary<string, int>
{
    ["todo"] = 0, ["in_progress"] = 3, ["done"] = 5, ["archived"] = 0,
};
var Transitions = new HashSet<(string, string)>
{
    ("todo", "in_progress"), ("todo", "archived"), ("in_progress", "todo"),
    ("in_progress", "done"), ("done", "archived"),
};
string[] ProjectSorts = ["id", "name", "taskCount"];
string[] TaskSorts = ["id", "title", "priority", "score", "status"];

var users = new Dictionary<int, User>
{
    [1] = new(1, "admin", "admin-secret", "admin"),
    [2] = new(2, "alice", "alice-secret", "user"),
    [3] = new(3, "bob", "bob-secret", "user"),
};
var sessions = new Dictionary<string, int>();
var projects = new Dictionary<int, Project>();
var tasks = new Dictionary<int, TaskItem>();
var nextProjectId = 1;
var nextTaskId = 1;
var byField = Comparer<object>.Create((left, right) => left is string text
    ? string.CompareOrdinal(text, (string)right)
    : Comparer<object>.Default.Compare(left, right));

var builder = WebApplication.CreateBuilder(args);
builder.Logging.ClearProviders();
builder.WebHost.UseUrls($"http://127.0.0.1:{Port}");
var app = builder.Build();

int ComputeScore(int priority, string status)
{
    var baseScore = priority * 10;
    return baseScore + StatusBonus[status];
}

int TaskCount(int projectId) => tasks.Values.Count(task => task.ProjectId == projectId);

Dictionary<string, object?> SerializeProject(Project project) => new()
{
    ["id"] = project.Id, ["name"] = project.Name, ["ownerId"] = project.OwnerId,
    ["taskCount"] = TaskCount(project.Id),
};

Dictionary<string, object?> SerializeTask(TaskItem task) => new()
{
    ["id"] = task.Id, ["projectId"] = task.ProjectId, ["title"] = task.Title,
    ["priority"] = task.Priority, ["status"] = task.Status,
    ["assigneeId"] = task.AssigneeId, ["score"] = task.Score,
};

AppError BadRequest() => new(400, "bad_request", "the request is malformed");

AppError NotFound() => new(404, "not_found", "the resource does not exist");

AppError Forbidden() => new(403, "forbidden", "you may not access this resource");

AppError Conflict() => new(409, "conflict", "the resource already exists");

AppError Invalid(List<Detail> details) =>
    new(422, "validation_failed", "the request body is not valid",
        [.. details.OrderBy(detail => detail.Field, StringComparer.Ordinal)
            .ThenBy(detail => detail.Message, StringComparer.Ordinal)]);

Detail Fail(string field, string message) => new(field, message);

async Task<JsonElement> ReadBody(HttpRequest request)
{
    using var reader = new StreamReader(request.Body);
    var raw = await reader.ReadToEndAsync();
    if (raw.Trim().Length == 0) raw = "{}";
    try
    {
        var parsed = JsonSerializer.Deserialize<JsonElement>(raw);
        if (parsed.ValueKind == JsonValueKind.Object) return parsed;
    }
    catch (JsonException) { }
    throw BadRequest();
}

int? ReadInt(JsonElement body, string field, int? fallback)
{
    if (!body.TryGetProperty(field, out var node)) return fallback;
    if (node.ValueKind == JsonValueKind.Null) return null;
    if (node.ValueKind == JsonValueKind.Number && node.TryGetInt32(out var value)) return value;
    throw BadRequest();
}

string ReadString(JsonElement body, string field, List<Detail> errors, int maxLength, bool required)
{
    var value = "";
    if (body.TryGetProperty(field, out var node))
    {
        if (node.ValueKind != JsonValueKind.String) throw BadRequest();
        value = node.GetString() ?? "";
    }
    if (value.Length == 0)
    {
        if (required) errors.Add(Fail(field, $"{field} is required"));
    }
    else if (value.Length > maxLength) errors.Add(Fail(field, $"{field} is too long"));
    return value;
}

int ReadPriority(JsonElement body, List<Detail> errors)
{
    var value = ReadInt(body, "priority", 0);
    if (value is null || value < MinPriority || value > MaxPriority)
        errors.Add(Fail("priority", "priority is out of range"));
    return value ?? 0;
}

int? ReadUserRef(JsonElement body, string field, List<Detail> errors, int? fallback)
{
    var value = ReadInt(body, field, fallback);
    if (value is not null && !users.ContainsKey(value.Value))
        errors.Add(Fail(field, $"{field} is not a known user"));
    return value;
}

int ParseId(string raw) => int.TryParse(raw, out var value) ? value : throw BadRequest();

(int, int, string, string) ReadPage(HttpRequest request, string[] allowed)
{
    var query = request.Query;
    var errors = new List<Detail>();
    int limit = DefaultLimit, offset = 0;
    var sort = query.ContainsKey("sort") ? query["sort"].ToString() : "id";
    var order = query.ContainsKey("order") ? query["order"].ToString() : "asc";
    if (query.ContainsKey("limit"))
    {
        limit = int.TryParse(query["limit"].ToString(), out var value) ? value : -1;
        if (limit < 1 || limit > MaxLimit) errors.Add(Fail("limit", "limit is out of range"));
    }
    if (query.ContainsKey("offset"))
    {
        offset = int.TryParse(query["offset"].ToString(), out var value) ? value : -1;
        if (offset < 0) errors.Add(Fail("offset", "offset is out of range"));
    }
    if (!allowed.Contains(sort)) errors.Add(Fail("sort", "sort is not a valid field"));
    if (order != "asc" && order != "desc") errors.Add(Fail("order", "order must be asc or desc"));
    if (errors.Count > 0) throw Invalid(errors);
    return (limit, offset, sort, order);
}

object Paginate(List<Dictionary<string, object?>> rows, int limit, int offset, string sort, string order)
{
    var sorted = order == "desc"
        ? rows.OrderByDescending(row => row[sort]!, byField).ThenBy(row => row["id"]!, byField)
        : rows.OrderBy(row => row[sort]!, byField).ThenBy(row => row["id"]!, byField);
    return new { items = sorted.Skip(offset).Take(limit), total = rows.Count, limit, offset };
}

User Authenticate(HttpRequest request)
{
    var header = request.Headers.Authorization.ToString();
    if (!header.StartsWith("Bearer ") || !sessions.TryGetValue(header[7..], out var userId))
        throw new AppError(401, "unauthorized", "authentication is required");
    request.HttpContext.Items["userId"] = userId;
    return users[userId];
}

void RequireAdmin(User user)
{
    if (user.Role != "admin") throw Forbidden();
}

Project ReachableProject(int projectId, User user)
{
    if (!projects.TryGetValue(projectId, out var project)) throw NotFound();
    if (user.Role != "admin" && project.OwnerId != user.Id) throw Forbidden();
    return project;
}

TaskItem ReachableTask(int taskId, User user)
{
    if (!tasks.TryGetValue(taskId, out var task)) throw NotFound();
    ReachableProject(task.ProjectId, user);
    return task;
}

async Task Observe(HttpContext context, RequestDelegate next)
{
    var requestId = context.Request.Headers["X-Request-Id"].ToString();
    if (requestId.Length == 0) requestId = Guid.NewGuid().ToString("N")[..12];
    context.Response.Headers["X-Request-Id"] = requestId;
    var started = Stopwatch.GetTimestamp();
    try
    {
        await next(context);
    }
    catch (AppError error)
    {
        context.Response.StatusCode = error.Status;
        await context.Response.WriteAsJsonAsync(new
        {
            error = new { error.Code, error.Message, requestId, error.Details },
        });
    }
    var status = context.Response.StatusCode;
    Console.Out.Write(JsonSerializer.Serialize(new
    {
        level = status >= 500 ? "error" : status >= 400 ? "warn" : "info",
        requestId,
        method = context.Request.Method,
        path = context.Request.Path.ToString(),
        status,
        durationMs = (int)Stopwatch.GetElapsedTime(started).TotalMilliseconds,
        userId = context.Items["userId"],
    }) + "\n");
}

IResult GetHealth() =>
    Results.Json(new { status = "ok", projects = projects.Count, tasks = tasks.Count });

async Task<IResult> Login(HttpRequest request)
{
    var body = await ReadBody(request);
    var errors = new List<Detail>();
    var username = ReadString(body, "username", errors, MaxNameLength, true);
    var password = ReadString(body, "password", errors, MaxNameLength, true);
    if (errors.Count > 0) throw Invalid(errors);
    var user = users.Values.FirstOrDefault(candidate =>
        candidate.Username == username && candidate.Password == password);
    if (user is null) throw new AppError(401, "invalid_credentials", "the username or password is wrong");
    var token = Guid.NewGuid().ToString("N");
    sessions[token] = user.Id;
    return Results.Json(new { token, userId = user.Id, role = user.Role });
}

IResult Logout(HttpRequest request)
{
    Authenticate(request);
    sessions.Remove(request.Headers.Authorization.ToString()[7..]);
    return Results.StatusCode(204);
}

IResult GetMe(HttpRequest request)
{
    var user = Authenticate(request);
    return Results.Json(new { userId = user.Id, username = user.Username, role = user.Role });
}

IResult ListProjects(HttpRequest request)
{
    var user = Authenticate(request);
    var (limit, offset, sort, order) = ReadPage(request, ProjectSorts);
    var rows = projects.Values
        .Where(project => user.Role == "admin" || project.OwnerId == user.Id)
        .Select(SerializeProject).ToList();
    return Results.Json(Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateProject(HttpRequest request)
{
    var user = Authenticate(request);
    RequireAdmin(user);
    var body = await ReadBody(request);
    var errors = new List<Detail>();
    var name = ReadString(body, "name", errors, MaxNameLength, true);
    var ownerId = ReadUserRef(body, "ownerId", errors, user.Id) ?? user.Id;
    if (errors.Count > 0) throw Invalid(errors);
    if (projects.Values.Any(project => project.OwnerId == ownerId && project.Name == name))
        throw Conflict();
    var project = new Project(nextProjectId, name, ownerId);
    projects[nextProjectId] = project;
    nextProjectId += 1;
    return Results.Json(SerializeProject(project), statusCode: 201);
}

IResult GetProject(HttpRequest request, string id)
{
    var user = Authenticate(request);
    return Results.Json(SerializeProject(ReachableProject(ParseId(id), user)));
}

async Task<IResult> UpdateProject(HttpRequest request, string id)
{
    var user = Authenticate(request);
    RequireAdmin(user);
    var project = ReachableProject(ParseId(id), user);
    var body = await ReadBody(request);
    if (!body.TryGetProperty("name", out _)) return Results.Json(SerializeProject(project));
    var errors = new List<Detail>();
    var name = ReadString(body, "name", errors, MaxNameLength, true);
    if (errors.Count > 0) throw Invalid(errors);
    if (projects.Values.Any(other =>
        other.OwnerId == project.OwnerId && other.Name == name && other.Id != project.Id))
        throw Conflict();
    var renamed = project with { Name = name };
    projects[project.Id] = renamed;
    return Results.Json(SerializeProject(renamed));
}

IResult DeleteProject(HttpRequest request, string id)
{
    var user = Authenticate(request);
    RequireAdmin(user);
    var project = ReachableProject(ParseId(id), user);
    foreach (var taskId in tasks.Values.Where(task => task.ProjectId == project.Id)
        .Select(task => task.Id).ToArray()) tasks.Remove(taskId);
    projects.Remove(project.Id);
    return Results.StatusCode(204);
}

IResult ListTasks(HttpRequest request, string id)
{
    var user = Authenticate(request);
    var project = ReachableProject(ParseId(id), user);
    var (limit, offset, sort, order) = ReadPage(request, TaskSorts);
    var rows = tasks.Values.Where(task => task.ProjectId == project.Id).Select(SerializeTask).ToList();
    return Results.Json(Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateTask(HttpRequest request, string id)
{
    var user = Authenticate(request);
    var project = ReachableProject(ParseId(id), user);
    var body = await ReadBody(request);
    var errors = new List<Detail>();
    var title = ReadString(body, "title", errors, MaxTitleLength, true);
    var priority = ReadPriority(body, errors);
    var assigneeId = ReadUserRef(body, "assigneeId", errors, null);
    if (errors.Count > 0) throw Invalid(errors);
    var task = new TaskItem(nextTaskId, project.Id, title, priority, "todo", assigneeId,
        ComputeScore(priority, "todo"));
    tasks[nextTaskId] = task;
    nextTaskId += 1;
    return Results.Json(SerializeTask(task), statusCode: 201);
}

IResult GetTask(HttpRequest request, string id)
{
    var user = Authenticate(request);
    return Results.Json(SerializeTask(ReachableTask(ParseId(id), user)));
}

async Task<IResult> ReplaceTask(HttpRequest request, string id)
{
    var user = Authenticate(request);
    var task = ReachableTask(ParseId(id), user);
    var body = await ReadBody(request);
    var errors = new List<Detail>();
    var title = ReadString(body, "title", errors, MaxTitleLength, true);
    var priority = ReadPriority(body, errors);
    var assigneeId = ReadUserRef(body, "assigneeId", errors, null);
    if (errors.Count > 0) throw Invalid(errors);
    var replaced = task with
    {
        Title = title, Priority = priority, AssigneeId = assigneeId,
        Score = ComputeScore(priority, task.Status),
    };
    tasks[task.Id] = replaced;
    return Results.Json(SerializeTask(replaced));
}

async Task<IResult> UpdateStatus(HttpRequest request, string id)
{
    var user = Authenticate(request);
    var task = ReachableTask(ParseId(id), user);
    var body = await ReadBody(request);
    var status = body.TryGetProperty("status", out var node) && node.ValueKind == JsonValueKind.String
        ? node.GetString()! : "";
    if (!StatusBonus.ContainsKey(status)) throw Invalid([Fail("status", "status is not valid")]);
    if (!Transitions.Contains((task.Status, status)))
        throw new AppError(409, "invalid_transition", "the status change is not allowed");
    var moved = task with { Status = status, Score = ComputeScore(task.Priority, status) };
    tasks[task.Id] = moved;
    return Results.Json(SerializeTask(moved));
}

IResult DeleteTask(HttpRequest request, string id)
{
    var user = Authenticate(request);
    var task = ReachableTask(ParseId(id), user);
    tasks.Remove(task.Id);
    return Results.StatusCode(204);
}

IResult GetStats(HttpRequest request)
{
    var user = Authenticate(request);
    RequireAdmin(user);
    var byStatus = StatusBonus.ToDictionary(entry => entry.Key,
        entry => tasks.Values.Count(task => task.Status == entry.Key));
    var total = tasks.Count;
    var avgScore = total == 0 ? 0d : Math.Round((double)tasks.Values.Sum(task => task.Score) / total, 2);
    var best = projects.Values.OrderByDescending(project => TaskCount(project.Id))
        .ThenBy(project => project.Id).FirstOrDefault();
    return Results.Json(new
    {
        projects = projects.Count, tasks = total, users = users.Count, sessions = sessions.Count,
        byStatus, avgScore, topProjectName = best?.Name,
    });
}

IResult Fallback() => throw NotFound();

app.Use(Observe);

app.MapGet("/health", GetHealth);
app.MapPost("/auth/login", Login);
app.MapPost("/auth/logout", Logout);
app.MapGet("/me", GetMe);
app.MapGet("/projects", ListProjects);
app.MapPost("/projects", CreateProject);
app.MapGet("/projects/{id}", GetProject);
app.MapPatch("/projects/{id}", UpdateProject);
app.MapDelete("/projects/{id}", DeleteProject);
app.MapGet("/projects/{id}/tasks", ListTasks);
app.MapPost("/projects/{id}/tasks", CreateTask);
app.MapGet("/tasks/{id}", GetTask);
app.MapPut("/tasks/{id}", ReplaceTask);
app.MapPatch("/tasks/{id}/status", UpdateStatus);
app.MapDelete("/tasks/{id}", DeleteTask);
app.MapGet("/stats", GetStats);
app.MapFallback(Fallback);

app.Run();

record User(int Id, string Username, string Password, string Role);

record Project(int Id, string Name, int OwnerId);

record TaskItem(int Id, int ProjectId, string Title, int Priority, string Status,
    int? AssigneeId, int Score);

record Detail(string Field, string Message);

class AppError(int status, string code, string message, List<Detail>? details = null)
    : Exception(message)
{
    public int Status { get; } = status;
    public string Code { get; } = code;
    public List<Detail> Details { get; } = details ?? [];
}
