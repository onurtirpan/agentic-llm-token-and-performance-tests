// Task Service, large tier — the in-memory state and its repositories.
package com.example.taskservice

class Store {

    val users = sortedMapOf<Int, User>()
    val sessions = mutableMapOf<String, Session>()
    val projects = sortedMapOf<Int, Project>()
    val tasks = sortedMapOf<Int, Task>()
    val comments = sortedMapOf<Int, Comment>()
    val audit = mutableListOf<AuditEntry>()
    val outbox = mutableListOf<OutboxEvent>()
    val idempotency = mutableMapOf<Pair<String, String>, Recorded>()
    val byStatus = sortedMapOf<Int, Int>()
    val byRoute = sortedMapOf<String, Int>()

    var requests = 0
    var nextProjectId = 1
    var nextTaskId = 1
    var nextCommentId = 1
    var nextUserId = 5
    var nextSeq = 1

    fun seed() {
        users[1] = User(1, "admin", "admin-secret", "admin", DEFAULT_QUOTA)
        users[2] = User(2, "alice", "alice-secret", "user", DEFAULT_QUOTA)
        users[3] = User(3, "bob", "bob-secret", "user", DEFAULT_QUOTA)
        users[4] = User(4, "probe", "probe-secret", "user", PROBE_QUOTA)
    }

    fun takeSeq(): Int {
        val value = nextSeq
        nextSeq += 1
        return value
    }

    /** Append one audit entry and one outbox event for a successful write. */
    fun record(actorId: Int, action: String, resource: String, resourceId: Int) {
        audit.add(AuditEntry(takeSeq(), actorId, action, resource, resourceId))
        outbox.add(OutboxEvent(takeSeq(), "$resource.$action", resourceId))
    }

    fun countRequest(route: String, status: Int) {
        requests += 1
        byRoute[route] = (byRoute[route] ?: 0) + 1
        byStatus[status] = (byStatus[status] ?: 0) + 1
    }

    fun findUser(userId: Int?, includeDeleted: Boolean = false): User? =
        userId?.let { users[it] }?.takeIf { includeDeleted || !it.deleted }

    fun findByUsername(username: String): User? =
        users.values.find { it.username == username && !it.deleted }

    fun insertUser(username: String, password: String, role: String, quota: Int): User {
        val user = User(nextUserId, username, password, role, quota)
        users[user.id] = user
        nextUserId += 1
        return user
    }

    fun findProject(projectId: Int?, includeDeleted: Boolean = false): Project? =
        projectId?.let { projects[it] }?.takeIf { includeDeleted || !it.deleted }

    fun insertProject(name: String, ownerId: Int): Project {
        val project = Project(nextProjectId, name, ownerId)
        projects[project.id] = project
        nextProjectId += 1
        return project
    }

    fun findTask(taskId: Int?, includeDeleted: Boolean = false): Task? =
        taskId?.let { tasks[it] }?.takeIf { includeDeleted || !it.deleted }

    fun insertTask(
        projectId: Int,
        title: String,
        priority: Int,
        assigneeId: Int?,
        internalNote: String,
    ): Task {
        val task = Task(nextTaskId, projectId, title, priority, "todo", assigneeId, internalNote)
        tasks[task.id] = task
        nextTaskId += 1
        return task
    }

    fun findComment(commentId: Int): Comment? = comments[commentId]

    fun insertComment(taskId: Int, authorId: Int, body: String): Comment {
        val comment = Comment(nextCommentId, taskId, authorId, body)
        comments[comment.id] = comment
        nextCommentId += 1
        return comment
    }

    fun liveTasksOf(projectId: Int): List<Task> =
        tasks.values.filter { it.projectId == projectId && !it.deleted }

    fun taskCount(projectId: Int): Int = liveTasksOf(projectId).size

    fun outboxPending(): Int = outbox.count { !it.delivered }
}
