// Task Service, large tier — HTTP routing, middleware and the entry point.

using System.Diagnostics;
using System.Text.Json;

Store.Seed();

var builder = WebApplication.CreateBuilder(args);
builder.Logging.ClearProviders();
builder.WebHost.UseUrls($"http://127.0.0.1:{Domain.Port}");
var app = builder.Build();

// ----------------------------------------------------------------- middleware

Dictionary<string, object?> Envelope(string requestId, AppError error) => new()
{
    ["error"] = new Dictionary<string, object?>
    {
        ["code"] = error.Code, ["message"] = error.Message,
        ["requestId"] = requestId, ["details"] = error.Details,
    },
};

async Task Observe(HttpContext context, RequestDelegate next)
{
    var requestId = context.Request.Headers["X-Request-Id"].ToString();
    if (requestId.Length == 0) requestId = Guid.NewGuid().ToString("N")[..12];
    context.Items["requestId"] = requestId;
    context.Response.Headers["X-Request-Id"] = requestId;
    context.Response.OnStarting(() =>
    {
        if (context.Items["quotaRemaining"] is int remaining)
            context.Response.Headers["X-Quota-Remaining"] = remaining.ToString();
        if (context.Items["replayed"] is true)
            context.Response.Headers["Idempotency-Replayed"] = "true";
        return Task.CompletedTask;
    });
    var before = Store.Audit.Count;
    var started = Stopwatch.GetTimestamp();
    try
    {
        await next(context);
    }
    catch (AppError error)
    {
        context.Response.StatusCode = error.Status;
        await context.Response.WriteAsJsonAsync(Envelope(requestId, error));
    }
    var status = context.Response.StatusCode;
    var pattern = (context.GetEndpoint() as RouteEndpoint)?.RoutePattern.RawText ?? "unmatched";
    Store.CountRequest($"{context.Request.Method} {pattern}", status);
    Console.Out.Write(JsonSerializer.Serialize(new
    {
        level = status >= 500 ? "error" : status >= 400 ? "warn" : "info",
        requestId,
        method = context.Request.Method,
        path = context.Request.Path.ToString(),
        status,
        durationMs = (int)Stopwatch.GetElapsedTime(started).TotalMilliseconds,
        userId = context.Items["userId"],
        quotaRemaining = context.Items["quotaRemaining"],
        auditSeq = Store.Audit.Count - before,
    }) + "\n");
}

// -------------------------------------------------------------------- helpers

/// <summary>Authenticate, charge the quota, then check the role. This order is fixed.</summary>
(User, Session) Begin(HttpContext context, bool admin = false)
{
    var (user, session) = Service.Authenticate(context.Request.Headers.Authorization.ToString());
    context.Items["userId"] = user.Id;
    context.Items["quotaRemaining"] = Service.ChargeQuota(user, session);
    if (admin) Service.RequireAdmin(user);
    return (user, session);
}

async Task<JsonElement> BodyOf(HttpRequest request)
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
    throw Domain.BadRequest();
}

int ParseId(string raw) => int.TryParse(raw, out var value) ? value : throw Domain.BadRequest();

string? Query(HttpContext context, string name) =>
    context.Request.Query.TryGetValue(name, out var value) ? value.ToString() : null;

void IfMatch(HttpContext context, int version) =>
    Service.CheckIfMatch(context.Request.Headers.IfMatch.ToString(), version);

(int, int, string, string) ReadPage(HttpRequest request, string[] allowed)
{
    var query = request.Query;
    var errors = new List<Detail>();
    int limit = Domain.DefaultLimit, offset = 0;
    var sort = query.ContainsKey("sort") ? query["sort"].ToString() : allowed[0];
    var order = query.ContainsKey("order") ? query["order"].ToString() : "asc";
    if (query.ContainsKey("limit"))
    {
        limit = int.TryParse(query["limit"].ToString(), out var value) ? value : -1;
        if (limit < 1 || limit > Domain.MaxLimit)
            errors.Add(Domain.Fail("limit", "limit is out of range"));
    }
    if (query.ContainsKey("offset"))
    {
        offset = int.TryParse(query["offset"].ToString(), out var value) ? value : -1;
        if (offset < 0) errors.Add(Domain.Fail("offset", "offset is out of range"));
    }
    if (!allowed.Contains(sort)) errors.Add(Domain.Fail("sort", "sort is not a valid field"));
    if (order != "asc" && order != "desc")
        errors.Add(Domain.Fail("order", "order must be asc or desc"));
    if (errors.Count > 0) throw Domain.Invalid(errors);
    return (limit, offset, sort, order);
}

IResult Tagged(HttpContext context, Dictionary<string, object?> body, int version)
{
    context.Response.Headers.ETag = version.ToString();
    return Results.Json(body);
}

/// <summary>A single-resource body carries its version, so the ETag comes for free.</summary>
IResult Responded(HttpContext context, int status, Dictionary<string, object?> body)
{
    if (body.TryGetValue("version", out var version) && version is not null)
        context.Response.Headers.ETag = version.ToString();
    return Results.Json(body, statusCode: status);
}

/// <summary>Run produce once per Idempotency-Key, then replay the recorded outcome.</summary>
IResult Idempotent(HttpContext context, Session session,
    Func<(int Status, Dictionary<string, object?> Body)> produce)
{
    var key = context.Request.Headers["Idempotency-Key"].ToString();
    if (key.Length == 0)
    {
        var fresh = produce();
        return Responded(context, fresh.Status, fresh.Body);
    }
    var slot = (session.Token, key);
    if (Store.Idempotency.TryGetValue(slot, out var recorded))
    {
        context.Items["replayed"] = true;
        return Responded(context, recorded.Status, recorded.Body);
    }
    try
    {
        var made = produce();
        Store.Idempotency[slot] = made;
        return Responded(context, made.Status, made.Body);
    }
    catch (AppError error)
    {
        Store.Idempotency[slot] = (error.Status, Envelope((string)context.Items["requestId"]!, error));
        throw;
    }
}

// ------------------------------------------------------------- health and auth

IResult GetHealth() => Results.Json(new
{
    status = "ok",
    projects = Store.Projects.Values.Count(project => !project.Deleted),
    tasks = Store.Tasks.Values.Count(task => !task.Deleted),
    comments = Store.Comments.Count,
});

async Task<IResult> Login(HttpRequest request)
{
    var body = await BodyOf(request);
    var errors = new List<Detail>();
    var username = Domain.Text(body, "username");
    var password = Domain.Text(body, "password");
    if (username.Length == 0) errors.Add(Domain.Fail("username", "username is required"));
    if (password.Length == 0) errors.Add(Domain.Fail("password", "password is required"));
    if (errors.Count > 0) throw Domain.Invalid(errors);
    var token = Guid.NewGuid().ToString("N");
    var user = Service.Login(username, password, token);
    return Results.Json(new { token, userId = user.Id, role = user.Role });
}

IResult Logout(HttpContext context)
{
    var (_, session) = Begin(context);
    Store.Sessions.Remove(session.Token);
    return Results.StatusCode(204);
}

IResult GetMe(HttpContext context)
{
    var (user, _) = Begin(context);
    return Results.Json(new { userId = user.Id, username = user.Username, role = user.Role });
}

// ---------------------------------------------------------------------- users

IResult ListUsers(HttpContext context)
{
    Begin(context, true);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.UserSorts);
    var rows = Store.Users.Values.Where(user => !user.Deleted)
        .Select(Service.SerializeUser).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateUser(HttpContext context)
{
    var (actor, session) = Begin(context, true);
    var body = await BodyOf(context.Request);
    return Idempotent(context, session, () =>
    {
        var user = Service.CreateUser(actor, Domain.Text(body, "username"),
            Domain.Text(body, "password"), Domain.LooseText(body, "role", "user"),
            Domain.LooseWhole(body, "quota", Domain.DefaultQuota));
        return (201, Service.SerializeUser(user));
    });
}

IResult GetUser(HttpContext context, string id)
{
    Begin(context, true);
    var user = Store.FindUser(ParseId(id)) ?? throw Domain.NotFound();
    return Tagged(context, Service.SerializeUser(user), user.Version);
}

async Task<IResult> UpdateUser(HttpContext context, string id)
{
    var (actor, _) = Begin(context, true);
    var user = Store.FindUser(ParseId(id)) ?? throw Domain.NotFound();
    IfMatch(context, user.Version);
    Service.UpdateUser(actor, user, await BodyOf(context.Request));
    return Tagged(context, Service.SerializeUser(user), user.Version);
}

IResult DeleteUser(HttpContext context, string id)
{
    var (actor, _) = Begin(context, true);
    var user = Store.FindUser(ParseId(id)) ?? throw Domain.NotFound();
    IfMatch(context, user.Version);
    Service.DeleteUser(actor, user);
    return Tagged(context, Service.SerializeUser(user), user.Version);
}

// ------------------------------------------------------------------- projects

IResult ListProjects(HttpContext context)
{
    var (user, _) = Begin(context);
    var include = Service.CheckIncludeDeleted(Query(context, "includeDeleted"), user);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.ProjectSorts);
    var rows = Service.VisibleProjects(user, include).Select(Service.SerializeProject).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateProject(HttpContext context)
{
    var (actor, session) = Begin(context, true);
    var body = await BodyOf(context.Request);
    return Idempotent(context, session, () =>
    {
        var project = Service.CreateProject(actor, Domain.Text(body, "name"),
            Domain.Whole(body, "ownerId", actor.Id));
        return (201, Service.SerializeProject(project));
    });
}

IResult GetProject(HttpContext context, string id)
{
    var (user, _) = Begin(context);
    var project = Service.ReachableProject(ParseId(id), user);
    return Tagged(context, Service.SerializeProject(project), project.Version);
}

async Task<IResult> UpdateProject(HttpContext context, string id)
{
    var (actor, _) = Begin(context, true);
    var project = Service.ReachableProject(ParseId(id), actor);
    IfMatch(context, project.Version);
    var body = await BodyOf(context.Request);
    if (body.TryGetProperty("name", out _))
        Service.RenameProject(actor, project, Domain.Text(body, "name"));
    return Tagged(context, Service.SerializeProject(project), project.Version);
}

IResult DeleteProject(HttpContext context, string id)
{
    var (actor, _) = Begin(context, true);
    var project = Service.ReachableProject(ParseId(id), actor);
    IfMatch(context, project.Version);
    Service.DeleteProject(actor, project);
    return Tagged(context, Service.SerializeProject(project), project.Version);
}

IResult RestoreProject(HttpContext context, string id)
{
    var (actor, _) = Begin(context, true);
    var project = Service.ReachableProject(ParseId(id), actor, true);
    IfMatch(context, project.Version);
    Service.RestoreProject(actor, project);
    return Tagged(context, Service.SerializeProject(project), project.Version);
}

// ---------------------------------------------------------------------- tasks

List<TaskItem> TaskFilters(HttpContext context, List<TaskItem> rows)
{
    var errors = new List<Detail>();
    var status = Query(context, "status");
    var assignee = Query(context, "assigneeId");
    var wanted = 0;
    if (status is not null && !Domain.Statuses.Contains(status))
        errors.Add(Domain.Fail("status", "status is not valid"));
    if (assignee is not null && !int.TryParse(assignee, out wanted))
        errors.Add(Domain.Fail("assigneeId", "assigneeId is not a known user"));
    if (errors.Count > 0) throw Domain.Invalid(errors);
    if (status is not null) rows = [.. rows.Where(task => task.Status == status)];
    if (assignee is not null) rows = [.. rows.Where(task => task.AssigneeId == wanted)];
    return rows;
}

IResult ListAllTasks(HttpContext context)
{
    var (user, _) = Begin(context);
    var include = Service.CheckIncludeDeleted(Query(context, "includeDeleted"), user);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.TaskSorts);
    var rows = TaskFilters(context, Service.VisibleTasks(user, include))
        .Select(task => Service.SerializeTask(task, user.Role)).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

IResult ListTasks(HttpContext context, string id)
{
    var (user, _) = Begin(context);
    var project = Service.ReachableProject(ParseId(id), user);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.TaskSorts);
    var rows = Store.Tasks.Values
        .Where(task => task.ProjectId == project.Id && !task.Deleted)
        .Select(task => Service.SerializeTask(task, user.Role)).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateTask(HttpContext context, string id)
{
    var (actor, session) = Begin(context);
    var project = Service.ReachableProject(ParseId(id), actor);
    var body = await BodyOf(context.Request);
    return Idempotent(context, session, () =>
    {
        var errors = new List<Detail>();
        var note = Service.ReadNote(actor, body, errors, "");
        var task = Service.CreateTask(actor, project, Domain.Text(body, "title"),
            Domain.Whole(body, "priority", 0), Domain.Whole(body, "assigneeId", null), note, errors);
        return (201, Service.SerializeTask(task, actor.Role));
    });
}

IResult GetTask(HttpContext context, string id)
{
    var (user, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), user);
    return Tagged(context, Service.SerializeTask(task, user.Role), task.Version);
}

async Task<IResult> ReplaceTask(HttpContext context, string id)
{
    var (actor, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), actor);
    IfMatch(context, task.Version);
    var body = await BodyOf(context.Request);
    var errors = new List<Detail>();
    var note = Service.ReadNote(actor, body, errors, task.InternalNote);
    Service.ReplaceTask(actor, task, Domain.Text(body, "title"),
        Domain.Whole(body, "priority", 0), Domain.Whole(body, "assigneeId", null), note, errors);
    return Tagged(context, Service.SerializeTask(task, actor.Role), task.Version);
}

async Task<IResult> UpdateStatus(HttpContext context, string id)
{
    var (actor, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), actor);
    IfMatch(context, task.Version);
    var body = await BodyOf(context.Request);
    Service.MoveStatus(actor, task, Domain.LooseText(body, "status", null));
    return Tagged(context, Service.SerializeTask(task, actor.Role), task.Version);
}

IResult DeleteTask(HttpContext context, string id)
{
    var (actor, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), actor);
    IfMatch(context, task.Version);
    Service.DeleteTask(actor, task);
    return Tagged(context, Service.SerializeTask(task, actor.Role), task.Version);
}

IResult RestoreTask(HttpContext context, string id)
{
    var (actor, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), actor, true);
    IfMatch(context, task.Version);
    Service.RestoreTask(actor, task);
    return Tagged(context, Service.SerializeTask(task, actor.Role), task.Version);
}

Dictionary<string, object?> BulkResult(int index, int status, int? resourceId) => new()
{
    ["index"] = index, ["status"] = status, ["id"] = resourceId, ["error"] = null,
};

Dictionary<string, object?> ApplyBulk(User actor, JsonElement item, int index)
{
    var operation = Domain.LooseText(item, "op", null);
    if (operation == "create")
    {
        var project = Service.ReachableProject(Domain.Whole(item, "projectId", 0), actor);
        var task = Service.CreateTask(actor, project, Domain.Text(item, "title"),
            Domain.Whole(item, "priority", 0), null, "", []);
        return BulkResult(index, 201, task.Id);
    }
    if (operation == "status")
    {
        var task = Service.ReachableTask(Domain.Whole(item, "id", 0), actor);
        Service.CheckVersion(Domain.Whole(item, "version", null), task.Version);
        Service.MoveStatus(actor, task, Domain.LooseText(item, "status", null));
        return BulkResult(index, 200, task.Id);
    }
    if (operation == "delete")
    {
        var task = Service.ReachableTask(Domain.Whole(item, "id", 0), actor);
        Service.CheckVersion(Domain.Whole(item, "version", null), task.Version);
        Service.DeleteTask(actor, task);
        return BulkResult(index, 200, task.Id);
    }
    throw Domain.Invalid([Domain.Fail("op", "op is not valid")]);
}

async Task<IResult> BulkTasks(HttpContext context)
{
    var (actor, _) = Begin(context);
    var body = await BodyOf(context.Request);
    var operations = Domain.Node(body, "operations");
    Service.CheckBulkSize(operations);
    var results = new List<Dictionary<string, object?>>();
    var index = 0;
    foreach (var item in operations!.Value.EnumerateArray())
    {
        try
        {
            if (item.ValueKind != JsonValueKind.Object) throw Domain.BadRequest();
            results.Add(ApplyBulk(actor, item, index));
        }
        catch (AppError error)
        {
            results.Add(new Dictionary<string, object?>
            {
                ["index"] = index, ["status"] = error.Status,
                ["id"] = null, ["error"] = error.Code,
            });
        }
        index += 1;
    }
    return Results.Json(new { results });
}

// ------------------------------------------------------------------- comments

IResult ListComments(HttpContext context, string id)
{
    var (user, _) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), user);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.CommentSorts);
    var rows = Store.Comments.Values.Where(comment => comment.TaskId == task.Id)
        .Select(Service.SerializeComment).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

async Task<IResult> CreateComment(HttpContext context, string id)
{
    var (actor, session) = Begin(context);
    var task = Service.ReachableTask(ParseId(id), actor);
    var body = await BodyOf(context.Request);
    return Idempotent(context, session, () =>
        (201, Service.SerializeComment(
            Service.CreateComment(actor, task, Domain.Text(body, "body")))));
}

IResult DeleteComment(HttpContext context, string id)
{
    var (actor, _) = Begin(context);
    var comment = Store.FindComment(ParseId(id)) ?? throw Domain.NotFound();
    Service.ReachableTask(comment.TaskId, actor, true);
    Service.RemoveComment(actor, comment);
    return Results.StatusCode(204);
}

// -------------------------------------------------- search, reports, telemetry

IResult Search(HttpContext context)
{
    var (user, _) = Begin(context);
    var query = Query(context, "q") ?? "";
    if (query.Length == 0) throw Domain.Invalid([Domain.Fail("q", "q is required")]);
    return Results.Json(Service.Search(user, query));
}

IResult Workload(HttpContext context)
{
    var (user, _) = Begin(context);
    var groupBy = Query(context, "groupBy") ?? "status";
    if (!Domain.GroupBys.Contains(groupBy))
        throw Domain.Invalid([Domain.Fail("groupBy", "groupBy is not valid")]);
    return Results.Json(Service.Workload(user, groupBy));
}

IResult ListAudit(HttpContext context)
{
    Begin(context, true);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.SeqSorts);
    var actorId = Query(context, "actorId");
    var resource = Query(context, "resource");
    var action = Query(context, "action");
    var rows = Store.Audit
        .Where(entry => (actorId is null || entry.ActorId.ToString() == actorId)
            && (resource is null || entry.Resource == resource)
            && (action is null || entry.Action == action))
        .Select(Service.SerializeAudit).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

IResult ListOutbox(HttpContext context)
{
    Begin(context, true);
    var (limit, offset, sort, order) = ReadPage(context.Request, Domain.SeqSorts);
    var wanted = Query(context, "delivered");
    var rows = Store.Outbox.Where(evt => wanted is null || evt.Delivered == (wanted == "true"))
        .Select(Service.SerializeOutbox).ToList();
    return Results.Json(Service.Paginate(rows, limit, offset, sort, order));
}

IResult FlushOutbox(HttpContext context)
{
    Begin(context, true);
    return Results.Json(new { flushed = Service.FlushOutbox() });
}

IResult GetMetrics(HttpContext context)
{
    Begin(context, true);
    return Results.Json(Service.Metrics());
}

IResult GetStats(HttpContext context)
{
    Begin(context, true);
    return Results.Json(Service.Stats());
}

IResult Fallback() => throw Domain.NotFound();

// --------------------------------------------------------------------- routing

app.Use(Observe);

app.MapGet("/health", GetHealth);
app.MapPost("/auth/login", Login);
app.MapPost("/auth/logout", Logout);
app.MapGet("/me", GetMe);
app.MapGet("/users", ListUsers);
app.MapPost("/users", (Delegate)CreateUser);
app.MapGet("/users/{id}", GetUser);
app.MapPatch("/users/{id}", UpdateUser);
app.MapDelete("/users/{id}", DeleteUser);
app.MapGet("/projects", ListProjects);
app.MapPost("/projects", (Delegate)CreateProject);
app.MapGet("/projects/{id}", GetProject);
app.MapPatch("/projects/{id}", UpdateProject);
app.MapDelete("/projects/{id}", DeleteProject);
app.MapPost("/projects/{id}/restore", RestoreProject);
app.MapGet("/projects/{id}/tasks", ListTasks);
app.MapPost("/projects/{id}/tasks", CreateTask);
app.MapGet("/tasks", ListAllTasks);
app.MapPost("/tasks/bulk", (Delegate)BulkTasks);
app.MapGet("/tasks/{id}", GetTask);
app.MapPut("/tasks/{id}", ReplaceTask);
app.MapDelete("/tasks/{id}", DeleteTask);
app.MapPatch("/tasks/{id}/status", UpdateStatus);
app.MapPost("/tasks/{id}/restore", RestoreTask);
app.MapGet("/tasks/{id}/comments", ListComments);
app.MapPost("/tasks/{id}/comments", CreateComment);
app.MapDelete("/comments/{id}", DeleteComment);
app.MapGet("/search", Search);
app.MapGet("/reports/workload", Workload);
app.MapGet("/audit", ListAudit);
app.MapGet("/outbox", ListOutbox);
app.MapPost("/outbox/flush", FlushOutbox);
app.MapGet("/metrics", GetMetrics);
app.MapGet("/stats", GetStats);
app.MapFallback(Fallback);

app.Run();
