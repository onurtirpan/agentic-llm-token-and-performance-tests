// Task Service — ASP.NET Core Minimal API implementation.

using System.Text.Json;

const int MaxTitleLength = 80;
const int MinPriority = 1;
const int MaxPriority = 5;
const int Port = 8080;

var tasks = new Dictionary<int, TaskItem>();
var nextId = 1;
var jsonOptions = new JsonSerializerOptions(JsonSerializerDefaults.Web);

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls($"http://127.0.0.1:{Port}");
var app = builder.Build();

int ComputeScore(int priority, bool done)
{
    var baseScore = priority * 10;
    return done ? baseScore : baseScore + 5;
}

string? Validate(string title, int priority)
{
    if (title.Length == 0) return "title is required";
    if (title.Length > MaxTitleLength) return "title is too long";
    if (priority < MinPriority || priority > MaxPriority) return "priority is out of range";
    return null;
}

IResult Fail(int status, string message) =>
    Results.Json(new { error = message }, statusCode: status);

int? ParseId(string raw) => int.TryParse(raw, out var value) ? value : null;

TaskItem[] SortedTasks() => [.. tasks.Values.OrderBy(t => t.Id)];

async Task<TaskInput?> ReadInput(HttpRequest request)
{
    try
    {
        var raw = await JsonSerializer.DeserializeAsync<RawInput>(request.Body, jsonOptions);
        if (raw is null) return null;
        return new TaskInput(raw.Title ?? "", raw.Priority ?? 0, raw.Done ?? false);
    }
    catch (JsonException)
    {
        return null;
    }
}

app.MapGet("/health", () => Results.Json(new { status = "ok", count = tasks.Count }));

app.MapGet("/tasks", (string? done) =>
{
    if (done is not null && done != "true" && done != "false")
        return Fail(400, "done must be true or false");
    var selected = SortedTasks()
        .Where(t => done is null || t.Done == (done == "true"))
        .OrderByDescending(t => t.Score)
        .ThenBy(t => t.Id)
        .ToArray();
    return Results.Json(new { tasks = selected, total = selected.Length });
});

app.MapGet("/tasks/{id}", (string id) =>
{
    if (ParseId(id) is not int taskId) return Fail(400, "invalid id");
    if (!tasks.TryGetValue(taskId, out var task)) return Fail(404, "task not found");
    return Results.Json(task);
});

app.MapPost("/tasks", async (HttpRequest request) =>
{
    if (await ReadInput(request) is not TaskInput input) return Fail(400, "invalid json");
    if (Validate(input.Title, input.Priority) is string error) return Fail(400, error);
    var task = new TaskItem(nextId, input.Title, input.Priority, false,
        ComputeScore(input.Priority, false));
    tasks[nextId] = task;
    nextId += 1;
    return Results.Json(task, statusCode: 201);
});

app.MapPut("/tasks/{id}", async (string id, HttpRequest request) =>
{
    if (ParseId(id) is not int taskId) return Fail(400, "invalid id");
    if (!tasks.ContainsKey(taskId)) return Fail(404, "task not found");
    if (await ReadInput(request) is not TaskInput input) return Fail(400, "invalid json");
    if (Validate(input.Title, input.Priority) is string error) return Fail(400, error);
    var task = new TaskItem(taskId, input.Title, input.Priority, input.Done,
        ComputeScore(input.Priority, input.Done));
    tasks[taskId] = task;
    return Results.Json(task);
});

app.MapDelete("/tasks/{id}", (string id) =>
{
    if (ParseId(id) is not int taskId) return Fail(400, "invalid id");
    if (!tasks.Remove(taskId)) return Fail(404, "task not found");
    return Results.StatusCode(204);
});

app.MapGet("/stats", () =>
{
    var all = SortedTasks();
    var total = all.Length;
    var doneCount = all.Count(t => t.Done);
    var sumScore = all.Sum(t => t.Score);
    var avgScore = total == 0 ? 0d : Math.Round((double)sumScore / total, 2);
    TaskItem? best = null;
    foreach (var task in all)
    {
        if (!task.Done && (best is null || task.Priority > best.Priority)) best = task;
    }
    return Results.Json(new
    {
        total,
        doneCount,
        openCount = total - doneCount,
        avgScore,
        topOpenTitle = best?.Title,
    });
});

app.MapFallback(() => Fail(404, "not found"));

app.Run();

record TaskItem(int Id, string Title, int Priority, bool Done, int Score);

record TaskInput(string Title, int Priority, bool Done);

record RawInput(string? Title, int? Priority, bool? Done);
