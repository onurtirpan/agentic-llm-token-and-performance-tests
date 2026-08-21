// Task Service, large tier — domain types, constants and pure rules.

using System.Text.Json;

record Detail(string Field, string Message);

record Session(string Token, int UserId)
{
    public int Used { get; set; }
}

record User(int Id, string Username, string Password)
{
    public required string Role { get; set; }
    public required int Quota { get; set; }
    public int Version { get; set; } = 1;
    public bool Deleted { get; set; }
}

record Project(int Id, int OwnerId)
{
    public required string Name { get; set; }
    public int Version { get; set; } = 1;
    public bool Deleted { get; set; }
}

record TaskItem(int Id, int ProjectId)
{
    public required string Title { get; set; }
    public required int Priority { get; set; }
    public required string Status { get; set; }
    public required int? AssigneeId { get; set; }
    public required string InternalNote { get; set; }
    public int Version { get; set; } = 1;
    public bool Deleted { get; set; }
}

record Comment(int Id, int TaskId, int AuthorId, string Body);

record AuditEntry(int Seq, int ActorId, string Action, string Resource, int ResourceId);

record OutboxEvent(int Seq, string Name, int ResourceId)
{
    public bool Delivered { get; set; }
}

/// <summary>Every failure path throws this. The api layer turns it into the envelope.</summary>
class AppError(int status, string code, string message, List<Detail>? details = null)
    : Exception(message)
{
    public int Status { get; } = status;
    public string Code { get; } = code;
    public List<Detail> Details { get; } = details ?? [];
}

static class Domain
{
    public const int MaxTitleLength = 80;
    public const int MaxNameLength = 60;
    public const int MaxCommentLength = 200;
    public const int MaxBulkItems = 20;
    public const int MinPriority = 1;
    public const int MaxPriority = 5;
    public const int DefaultLimit = 20;
    public const int MaxLimit = 100;
    public const int DefaultQuota = 10000;
    public const int ProbeQuota = 5;
    public const int Port = 8080;

    public static readonly string[] Roles = ["admin", "user"];
    public static readonly string[] Statuses = ["todo", "in_progress", "done", "archived"];
    public static readonly string[] ProjectSorts = ["id", "name", "taskCount"];
    public static readonly string[] TaskSorts = ["id", "title", "priority", "score", "status"];
    public static readonly string[] UserSorts = ["id", "username", "role"];
    public static readonly string[] CommentSorts = ["id", "authorId"];
    public static readonly string[] SeqSorts = ["seq"];
    public static readonly string[] GroupBys = ["assignee", "status", "project"];

    public static readonly Dictionary<string, int> StatusBonus = new()
    {
        ["todo"] = 0, ["in_progress"] = 3, ["done"] = 5, ["archived"] = 0,
    };

    public static readonly HashSet<(string, string)> Transitions =
    [
        ("todo", "in_progress"), ("todo", "archived"), ("in_progress", "todo"),
        ("in_progress", "done"), ("done", "archived"),
    ];

    public static AppError BadRequest() => new(400, "bad_request", "the request is malformed");

    public static AppError Unauthorized() => new(401, "unauthorized", "authentication is required");

    public static AppError InvalidCredentials() =>
        new(401, "invalid_credentials", "the username or password is wrong");

    public static AppError Forbidden() => new(403, "forbidden", "you may not access this resource");

    public static AppError NotFound() => new(404, "not_found", "the resource does not exist");

    public static AppError Conflict() => new(409, "conflict", "the resource already exists");

    public static AppError InvalidTransition() =>
        new(409, "invalid_transition", "the status change is not allowed");

    public static AppError PreconditionFailed() =>
        new(412, "precondition_failed", "the resource has changed");

    public static AppError PreconditionRequired() =>
        new(428, "precondition_required", "the If-Match header is required");

    public static AppError QuotaExceeded() =>
        new(429, "quota_exceeded", "the request quota is exhausted");

    public static AppError Invalid(List<Detail> details) =>
        new(422, "validation_failed", "the request body is not valid",
            [.. details.OrderBy(entry => entry.Field, StringComparer.Ordinal)
                .ThenBy(entry => entry.Message, StringComparer.Ordinal)]);

    public static Detail Fail(string field, string message) => new(field, message);

    public static int ComputeScore(int priority, string status)
    {
        var baseScore = priority * 10;
        return baseScore + StatusBonus[status];
    }

    public static void CheckString(string value, string fieldName, int maxLength, List<Detail> errors)
    {
        if (value.Length == 0) errors.Add(Fail(fieldName, $"{fieldName} is required"));
        else if (value.Length > maxLength) errors.Add(Fail(fieldName, $"{fieldName} is too long"));
    }

    public static void CheckPriority(int? value, List<Detail> errors)
    {
        if (value is null || value < MinPriority || value > MaxPriority)
            errors.Add(Fail("priority", "priority is out of range"));
    }

    public static void CheckStatus(string? value, List<Detail> errors)
    {
        if (value is null || !Statuses.Contains(value))
            errors.Add(Fail("status", "status is not valid"));
    }

    public static void CheckRole(string? value, List<Detail> errors)
    {
        if (value is null || !Roles.Contains(value)) errors.Add(Fail("role", "role is not valid"));
    }

    public static void CheckQuota(int? value, List<Detail> errors)
    {
        if (value is null || value < 0) errors.Add(Fail("quota", "quota is out of range"));
    }

    public static JsonElement? Node(JsonElement body, string field) =>
        body.TryGetProperty(field, out var node) ? node : null;

    /// <summary>A string field. A present value of another type is malformed.</summary>
    public static string Text(JsonElement body, string field, string fallback = "")
    {
        if (!body.TryGetProperty(field, out var node)) return fallback;
        if (node.ValueKind != JsonValueKind.String) throw BadRequest();
        return node.GetString() ?? "";
    }

    /// <summary>An integer field. A present value of another type is malformed.</summary>
    public static int? Whole(JsonElement body, string field, int? fallback)
    {
        if (!body.TryGetProperty(field, out var node)) return fallback;
        if (node.ValueKind == JsonValueKind.Null) return null;
        if (node.ValueKind == JsonValueKind.Number && node.TryGetInt32(out var value)) return value;
        throw BadRequest();
    }

    /// <summary>A string field whose wrong type is a validation problem, not a parse error.</summary>
    public static string? LooseText(JsonElement body, string field, string? fallback)
    {
        if (!body.TryGetProperty(field, out var node)) return fallback;
        return node.ValueKind == JsonValueKind.String ? node.GetString() : null;
    }

    /// <summary>An integer field whose wrong type is a validation problem, not a parse error.</summary>
    public static int? LooseWhole(JsonElement body, string field, int? fallback)
    {
        if (!body.TryGetProperty(field, out var node)) return fallback;
        return node.ValueKind == JsonValueKind.Number && node.TryGetInt32(out var value)
            ? value : null;
    }
}
