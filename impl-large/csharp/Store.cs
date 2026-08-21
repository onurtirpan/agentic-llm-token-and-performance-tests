// Task Service, large tier — the in-memory state and its repositories.

static class Store
{
    public static readonly Dictionary<int, User> Users = [];
    public static readonly Dictionary<string, Session> Sessions = [];
    public static readonly Dictionary<int, Project> Projects = [];
    public static readonly Dictionary<int, TaskItem> Tasks = [];
    public static readonly Dictionary<int, Comment> Comments = [];
    public static readonly List<AuditEntry> Audit = [];
    public static readonly List<OutboxEvent> Outbox = [];
    public static readonly Dictionary<(string, string), (int Status, Dictionary<string, object?> Body)>
        Idempotency = [];
    public static readonly Dictionary<int, int> ByStatus = [];
    public static readonly Dictionary<string, int> ByRoute = [];

    public static int Requests;

    static int nextProjectId = 1;
    static int nextTaskId = 1;
    static int nextCommentId = 1;
    static int nextUserId = 5;
    static int nextSeq = 1;

    public static void Seed()
    {
        Users[1] = new User(1, "admin", "admin-secret")
            { Role = "admin", Quota = Domain.DefaultQuota };
        Users[2] = new User(2, "alice", "alice-secret")
            { Role = "user", Quota = Domain.DefaultQuota };
        Users[3] = new User(3, "bob", "bob-secret")
            { Role = "user", Quota = Domain.DefaultQuota };
        Users[4] = new User(4, "probe", "probe-secret")
            { Role = "user", Quota = Domain.ProbeQuota };
    }

    public static int TakeSeq()
    {
        var value = nextSeq;
        nextSeq += 1;
        return value;
    }

    /// <summary>Append one audit entry and one outbox event for a successful write.</summary>
    public static void Record(int actorId, string action, string resource, int resourceId)
    {
        Audit.Add(new AuditEntry(TakeSeq(), actorId, action, resource, resourceId));
        Outbox.Add(new OutboxEvent(TakeSeq(), $"{resource}.{action}", resourceId));
    }

    public static void CountRequest(string route, int status)
    {
        Requests += 1;
        ByRoute[route] = ByRoute.GetValueOrDefault(route) + 1;
        ByStatus[status] = ByStatus.GetValueOrDefault(status) + 1;
    }

    public static User? FindUser(int? userId, bool includeDeleted = false)
    {
        if (userId is null || !Users.TryGetValue(userId.Value, out var user)) return null;
        return user.Deleted && !includeDeleted ? null : user;
    }

    public static User? FindByUsername(string username) =>
        Users.Values.FirstOrDefault(user => user.Username == username && !user.Deleted);

    public static User InsertUser(string username, string password, string role, int quota)
    {
        var user = new User(nextUserId, username, password) { Role = role, Quota = quota };
        Users[user.Id] = user;
        nextUserId += 1;
        return user;
    }

    public static Project? FindProject(int? projectId, bool includeDeleted = false)
    {
        if (projectId is null || !Projects.TryGetValue(projectId.Value, out var project)) return null;
        return project.Deleted && !includeDeleted ? null : project;
    }

    public static Project InsertProject(string name, int ownerId)
    {
        var project = new Project(nextProjectId, ownerId) { Name = name };
        Projects[project.Id] = project;
        nextProjectId += 1;
        return project;
    }

    public static TaskItem? FindTask(int? taskId, bool includeDeleted = false)
    {
        if (taskId is null || !Tasks.TryGetValue(taskId.Value, out var task)) return null;
        return task.Deleted && !includeDeleted ? null : task;
    }

    public static TaskItem InsertTask(int projectId, string title, int priority, int? assigneeId,
        string internalNote)
    {
        var task = new TaskItem(nextTaskId, projectId)
        {
            Title = title, Priority = priority, Status = "todo",
            AssigneeId = assigneeId, InternalNote = internalNote,
        };
        Tasks[task.Id] = task;
        nextTaskId += 1;
        return task;
    }

    public static Comment? FindComment(int commentId) =>
        Comments.TryGetValue(commentId, out var comment) ? comment : null;

    public static Comment InsertComment(int taskId, int authorId, string body)
    {
        var comment = new Comment(nextCommentId, taskId, authorId, body);
        Comments[comment.Id] = comment;
        nextCommentId += 1;
        return comment;
    }

    public static List<TaskItem> LiveTasksOf(int projectId) =>
        [.. Tasks.Values.Where(task => task.ProjectId == projectId && !task.Deleted)];

    public static int TaskCount(int projectId) => LiveTasksOf(projectId).Count;

    public static int OutboxPending() => Outbox.Count(evt => !evt.Delivered);
}
