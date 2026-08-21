// Task Service, large tier — business rules, authorization and audit emission.

using System.Text.Json;

static class Service
{
    static readonly IComparer<object> ByField = Comparer<object>.Create((left, right) =>
        left is string text
            ? string.CompareOrdinal(text, (string)right)
            : Comparer<object>.Default.Compare(left, right));

    // ------------------------------------------------------------- serializers

    public static Dictionary<string, object?> SerializeUser(User user) => new()
    {
        ["id"] = user.Id, ["username"] = user.Username, ["role"] = user.Role,
        ["quota"] = user.Quota, ["version"] = user.Version, ["deleted"] = user.Deleted,
    };

    public static Dictionary<string, object?> SerializeProject(Project project) => new()
    {
        ["id"] = project.Id, ["name"] = project.Name, ["ownerId"] = project.OwnerId,
        ["taskCount"] = Store.TaskCount(project.Id), ["version"] = project.Version,
        ["deleted"] = project.Deleted,
    };

    public static Dictionary<string, object?> SerializeTask(TaskItem task, string role)
    {
        var body = new Dictionary<string, object?>
        {
            ["id"] = task.Id, ["projectId"] = task.ProjectId, ["title"] = task.Title,
            ["priority"] = task.Priority, ["status"] = task.Status,
            ["assigneeId"] = task.AssigneeId,
        };
        if (role == "admin") body["internalNote"] = task.InternalNote;
        body["version"] = task.Version;
        body["deleted"] = task.Deleted;
        body["score"] = Domain.ComputeScore(task.Priority, task.Status);
        return body;
    }

    public static Dictionary<string, object?> SerializeComment(Comment comment) => new()
    {
        ["id"] = comment.Id, ["taskId"] = comment.TaskId,
        ["authorId"] = comment.AuthorId, ["body"] = comment.Body,
    };

    public static Dictionary<string, object?> SerializeAudit(AuditEntry entry) => new()
    {
        ["seq"] = entry.Seq, ["actorId"] = entry.ActorId, ["action"] = entry.Action,
        ["resource"] = entry.Resource, ["resourceId"] = entry.ResourceId,
    };

    public static Dictionary<string, object?> SerializeOutbox(OutboxEvent evt) => new()
    {
        ["seq"] = evt.Seq, ["name"] = evt.Name, ["resourceId"] = evt.ResourceId,
        ["delivered"] = evt.Delivered,
    };

    // ------------------------------------------------------------ access rules

    public static (User, Session) Authenticate(string header)
    {
        var token = header.StartsWith("Bearer ") ? header[7..] : "";
        if (!Store.Sessions.TryGetValue(token, out var session)) throw Domain.Unauthorized();
        var user = Store.FindUser(session.UserId) ?? throw Domain.Unauthorized();
        return (user, session);
    }

    public static int ChargeQuota(User user, Session session)
    {
        if (session.Used >= user.Quota) throw Domain.QuotaExceeded();
        session.Used += 1;
        return Math.Max(user.Quota - session.Used, 0);
    }

    public static void RequireAdmin(User user)
    {
        if (user.Role != "admin") throw Domain.Forbidden();
    }

    public static Project ReachableProject(int? projectId, User user, bool includeDeleted = false)
    {
        var project = Store.FindProject(projectId, includeDeleted) ?? throw Domain.NotFound();
        if (user.Role != "admin" && project.OwnerId != user.Id) throw Domain.Forbidden();
        return project;
    }

    public static TaskItem ReachableTask(int? taskId, User user, bool includeDeleted = false)
    {
        var task = Store.FindTask(taskId, includeDeleted) ?? throw Domain.NotFound();
        ReachableProject(task.ProjectId, user, true);
        return task;
    }

    public static void CheckIfMatch(string? header, int version)
    {
        if (string.IsNullOrEmpty(header)) throw Domain.PreconditionRequired();
        if (header != version.ToString()) throw Domain.PreconditionFailed();
    }

    /// <summary>A bulk item states its version inline, so an absent one is simply stale.</summary>
    public static void CheckVersion(int? version, int current)
    {
        if (version != current) throw Domain.PreconditionFailed();
    }

    public static bool CheckIncludeDeleted(string? raw, User user)
    {
        if (raw is null) return false;
        if (user.Role != "admin") throw Domain.Forbidden();
        return raw == "true";
    }

    // -------------------------------------------------------------- pagination

    /// <summary>Sort by the tiebreak first, then stably by the requested field.</summary>
    public static object Paginate(List<Dictionary<string, object?>> rows, int limit, int offset,
        string sort, string order)
    {
        var tiebreak = rows.Count > 0 && rows[0].ContainsKey("seq") ? "seq" : "id";
        var sorted = order == "desc"
            ? rows.OrderByDescending(row => row[sort]!, ByField).ThenBy(row => row[tiebreak]!, ByField)
            : rows.OrderBy(row => row[sort]!, ByField).ThenBy(row => row[tiebreak]!, ByField);
        return new { items = sorted.Skip(offset).Take(limit).ToList(), total = rows.Count, limit, offset };
    }

    // -------------------------------------------------------------------- auth

    public static User Login(string username, string password, string token)
    {
        var user = Store.FindByUsername(username);
        if (user is null || user.Password != password) throw Domain.InvalidCredentials();
        Store.Sessions[token] = new Session(token, user.Id);
        return user;
    }

    // ---------------------------------------------------------------- projects

    public static Project CreateProject(User actor, string name, int? ownerId)
    {
        var errors = new List<Detail>();
        Domain.CheckString(name, "name", Domain.MaxNameLength, errors);
        if (Store.FindUser(ownerId) is null)
            errors.Add(Domain.Fail("ownerId", "ownerId is not a known user"));
        if (errors.Count > 0) throw Domain.Invalid(errors);
        if (Store.Projects.Values.Any(other =>
            other.OwnerId == ownerId && other.Name == name && !other.Deleted)) throw Domain.Conflict();
        var project = Store.InsertProject(name, ownerId!.Value);
        Store.Record(actor.Id, "create", "project", project.Id);
        return project;
    }

    public static Project RenameProject(User actor, Project project, string name)
    {
        var errors = new List<Detail>();
        Domain.CheckString(name, "name", Domain.MaxNameLength, errors);
        if (errors.Count > 0) throw Domain.Invalid(errors);
        if (Store.Projects.Values.Any(other => other.OwnerId == project.OwnerId
            && other.Name == name && other.Id != project.Id && !other.Deleted)) throw Domain.Conflict();
        project.Name = name;
        project.Version += 1;
        Store.Record(actor.Id, "update", "project", project.Id);
        return project;
    }

    public static Project DeleteProject(User actor, Project project)
    {
        project.Deleted = true;
        project.Version += 1;
        Store.Record(actor.Id, "delete", "project", project.Id);
        foreach (var task in Store.LiveTasksOf(project.Id))
        {
            task.Deleted = true;
            task.Version += 1;
            Store.Record(actor.Id, "delete", "task", task.Id);
        }
        return project;
    }

    public static Project RestoreProject(User actor, Project project)
    {
        if (!project.Deleted) throw Domain.Conflict();
        project.Deleted = false;
        project.Version += 1;
        Store.Record(actor.Id, "restore", "project", project.Id);
        return project;
    }

    // ------------------------------------------------------------------- tasks

    public static string ReadNote(User actor, JsonElement body, List<Detail> errors, string current)
    {
        if (!body.TryGetProperty("internalNote", out var node)) return current;
        if (actor.Role != "admin") throw Domain.Forbidden();
        if (node.ValueKind != JsonValueKind.String) throw Domain.BadRequest();
        var note = node.GetString() ?? "";
        if (note.Length > Domain.MaxTitleLength)
            errors.Add(Domain.Fail("internalNote", "internalNote is too long"));
        return note;
    }

    public static TaskItem CreateTask(User actor, Project project, string title, int? priority,
        int? assigneeId, string note, List<Detail> errors)
    {
        Domain.CheckString(title, "title", Domain.MaxTitleLength, errors);
        Domain.CheckPriority(priority, errors);
        if (assigneeId is not null && Store.FindUser(assigneeId) is null)
            errors.Add(Domain.Fail("assigneeId", "assigneeId is not a known user"));
        if (errors.Count > 0) throw Domain.Invalid(errors);
        var task = Store.InsertTask(project.Id, title, priority!.Value, assigneeId, note);
        Store.Record(actor.Id, "create", "task", task.Id);
        return task;
    }

    public static TaskItem ReplaceTask(User actor, TaskItem task, string title, int? priority,
        int? assigneeId, string note, List<Detail> errors)
    {
        Domain.CheckString(title, "title", Domain.MaxTitleLength, errors);
        Domain.CheckPriority(priority, errors);
        if (assigneeId is not null && Store.FindUser(assigneeId) is null)
            errors.Add(Domain.Fail("assigneeId", "assigneeId is not a known user"));
        if (errors.Count > 0) throw Domain.Invalid(errors);
        task.Title = title;
        task.Priority = priority!.Value;
        task.AssigneeId = assigneeId;
        task.InternalNote = note;
        task.Version += 1;
        Store.Record(actor.Id, "update", "task", task.Id);
        return task;
    }

    public static TaskItem MoveStatus(User actor, TaskItem task, string? status)
    {
        var errors = new List<Detail>();
        Domain.CheckStatus(status, errors);
        if (errors.Count > 0) throw Domain.Invalid(errors);
        if (!Domain.Transitions.Contains((task.Status, status!))) throw Domain.InvalidTransition();
        task.Status = status!;
        task.Version += 1;
        Store.Record(actor.Id, "update", "task", task.Id);
        return task;
    }

    public static TaskItem DeleteTask(User actor, TaskItem task)
    {
        task.Deleted = true;
        task.Version += 1;
        Store.Record(actor.Id, "delete", "task", task.Id);
        return task;
    }

    public static TaskItem RestoreTask(User actor, TaskItem task)
    {
        if (!task.Deleted) throw Domain.Conflict();
        task.Deleted = false;
        task.Version += 1;
        Store.Record(actor.Id, "restore", "task", task.Id);
        return task;
    }

    // ---------------------------------------------------------------- comments

    public static Comment CreateComment(User actor, TaskItem task, string body)
    {
        var errors = new List<Detail>();
        Domain.CheckString(body, "body", Domain.MaxCommentLength, errors);
        if (errors.Count > 0) throw Domain.Invalid(errors);
        var comment = Store.InsertComment(task.Id, actor.Id, body);
        Store.Record(actor.Id, "create", "comment", comment.Id);
        return comment;
    }

    public static void RemoveComment(User actor, Comment comment)
    {
        if (actor.Role != "admin" && comment.AuthorId != actor.Id) throw Domain.Forbidden();
        Store.Comments.Remove(comment.Id);
        Store.Record(actor.Id, "delete", "comment", comment.Id);
    }

    // ------------------------------------------------------------------- users

    public static User CreateUser(User actor, string username, string password, string? role,
        int? quota)
    {
        var errors = new List<Detail>();
        Domain.CheckString(username, "username", Domain.MaxNameLength, errors);
        Domain.CheckString(password, "password", Domain.MaxNameLength, errors);
        Domain.CheckRole(role, errors);
        Domain.CheckQuota(quota, errors);
        if (errors.Count > 0) throw Domain.Invalid(errors);
        if (Store.FindByUsername(username) is not null) throw Domain.Conflict();
        var user = Store.InsertUser(username, password, role!, quota!.Value);
        Store.Record(actor.Id, "create", "user", user.Id);
        return user;
    }

    public static User UpdateUser(User actor, User user, JsonElement body)
    {
        var errors = new List<Detail>();
        var hasRole = body.TryGetProperty("role", out _);
        var hasQuota = body.TryGetProperty("quota", out _);
        var role = Domain.LooseText(body, "role", user.Role);
        var quota = Domain.LooseWhole(body, "quota", user.Quota);
        if (hasRole) Domain.CheckRole(role, errors);
        if (hasQuota) Domain.CheckQuota(quota, errors);
        if (errors.Count > 0) throw Domain.Invalid(errors);
        if (hasRole) user.Role = role!;
        if (hasQuota) user.Quota = quota!.Value;
        user.Version += 1;
        Store.Record(actor.Id, "update", "user", user.Id);
        return user;
    }

    public static User DeleteUser(User actor, User user)
    {
        if (user.Id == actor.Id) throw Domain.Conflict();
        user.Deleted = true;
        user.Version += 1;
        Store.Record(actor.Id, "delete", "user", user.Id);
        return user;
    }

    // ------------------------------------------------------ queries and reports

    public static List<Project> VisibleProjects(User user, bool includeDeleted) =>
        [.. Store.Projects.Values.Where(project => (includeDeleted || !project.Deleted)
            && (user.Role == "admin" || project.OwnerId == user.Id))];

    public static List<TaskItem> VisibleTasks(User user, bool includeDeleted)
    {
        var allowed = VisibleProjects(user, true).Select(project => project.Id).ToHashSet();
        return [.. Store.Tasks.Values.Where(task => allowed.Contains(task.ProjectId)
            && (includeDeleted || !task.Deleted))];
    }

    public static object Search(User user, string query)
    {
        var needle = query.ToLowerInvariant();
        var results = new List<Dictionary<string, object?>>();
        foreach (var project in VisibleProjects(user, false))
            if (project.Name.ToLowerInvariant().Contains(needle))
                results.Add(new() { ["type"] = "project", ["id"] = project.Id, ["label"] = project.Name });
        foreach (var task in VisibleTasks(user, false))
            if (task.Title.ToLowerInvariant().Contains(needle))
                results.Add(new() { ["type"] = "task", ["id"] = task.Id, ["label"] = task.Title });
        return new { results, total = results.Count };
    }

    public static object Workload(User user, string groupBy)
    {
        var rows = VisibleTasks(user, false);
        var groups = new List<Dictionary<string, object?>>();
        if (groupBy == "status")
            foreach (var status in Domain.Statuses)
                groups.Add(Group(status, rows.Where(task => task.Status == status)));
        else if (groupBy == "assignee")
        {
            foreach (var assignee in rows.Where(task => task.AssigneeId is not null)
                .Select(task => task.AssigneeId!.Value).Distinct().Order())
                groups.Add(Group(assignee.ToString(),
                    rows.Where(task => task.AssigneeId == assignee)));
            var loose = rows.Where(task => task.AssigneeId is null).ToList();
            if (loose.Count > 0) groups.Add(Group("unassigned", loose));
        }
        else
            foreach (var project in VisibleProjects(user, false).OrderBy(project => project.Id))
                groups.Add(Group(project.Name, rows.Where(task => task.ProjectId == project.Id)));
        return new { groupBy, groups };
    }

    static Dictionary<string, object?> Group(string key, IEnumerable<TaskItem> picked)
    {
        var rows = picked.ToList();
        return new()
        {
            ["key"] = key, ["tasks"] = rows.Count,
            ["totalScore"] = rows.Sum(task => Domain.ComputeScore(task.Priority, task.Status)),
        };
    }

    public static int FlushOutbox()
    {
        var pending = Store.Outbox.Where(evt => !evt.Delivered).ToList();
        foreach (var evt in pending) evt.Delivered = true;
        return pending.Count;
    }

    public static object Metrics() => new
    {
        requests = Store.Requests,
        byStatus = Store.ByStatus.OrderBy(entry => entry.Key)
            .ToDictionary(entry => entry.Key.ToString(), entry => entry.Value),
        byRoute = Store.ByRoute.OrderBy(entry => entry.Key, StringComparer.Ordinal)
            .Select(entry => new { route = entry.Key, count = entry.Value }).ToList(),
        auditEntries = Store.Audit.Count,
        outboxPending = Store.OutboxPending(),
    };

    public static object Stats()
    {
        var live = Store.Tasks.Values.Where(task => !task.Deleted).ToList();
        var byStatus = Domain.Statuses.ToDictionary(status => status,
            status => live.Count(task => task.Status == status));
        var total = live.Count;
        var scores = live.Sum(task => Domain.ComputeScore(task.Priority, task.Status));
        Project? best = null;
        foreach (var project in Store.Projects.Values)
        {
            if (project.Deleted) continue;
            if (best is null || Store.TaskCount(project.Id) > Store.TaskCount(best.Id)) best = project;
        }
        return new
        {
            projects = Store.Projects.Values.Count(project => !project.Deleted),
            tasks = total,
            users = Store.Users.Values.Count(user => !user.Deleted),
            sessions = Store.Sessions.Count,
            comments = Store.Comments.Count,
            byStatus,
            avgScore = total == 0 ? 0d : Math.Round((double)scores / total, 2),
            topProjectName = best?.Name,
            auditEntries = Store.Audit.Count,
            outboxPending = Store.OutboxPending(),
        };
    }

    public static void CheckBulkSize(JsonElement? operations)
    {
        if (operations is null || operations.Value.ValueKind != JsonValueKind.Array
            || operations.Value.GetArrayLength() < 1
            || operations.Value.GetArrayLength() > Domain.MaxBulkItems)
            throw Domain.Invalid([Domain.Fail("operations", "operations is out of range")]);
    }
}
