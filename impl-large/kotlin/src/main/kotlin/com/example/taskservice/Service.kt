// Task Service, large tier — business rules, authorization and audit emission.
package com.example.taskservice

// --------------------------------------------------------------------- serializers

fun serializeUser(user: User): Map<String, Any?> = mapOf(
    "id" to user.id,
    "username" to user.username,
    "role" to user.role,
    "quota" to user.quota,
    "version" to user.version,
    "deleted" to user.deleted,
)

fun serializeTask(task: Task, role: String): Map<String, Any?> = buildMap {
    put("id", task.id)
    put("projectId", task.projectId)
    put("title", task.title)
    put("priority", task.priority)
    put("status", task.status)
    put("assigneeId", task.assigneeId)
    if (role == "admin") put("internalNote", task.internalNote)
    put("version", task.version)
    put("deleted", task.deleted)
    put("score", computeScore(task.priority, task.status))
}

fun serializeComment(comment: Comment): Map<String, Any?> = mapOf(
    "id" to comment.id,
    "taskId" to comment.taskId,
    "authorId" to comment.authorId,
    "body" to comment.body,
)

fun serializeAudit(entry: AuditEntry): Map<String, Any?> = mapOf(
    "seq" to entry.seq,
    "actorId" to entry.actorId,
    "action" to entry.action,
    "resource" to entry.resource,
    "resourceId" to entry.resourceId,
)

fun serializeOutbox(event: OutboxEvent): Map<String, Any?> = mapOf(
    "seq" to event.seq,
    "name" to event.name,
    "resourceId" to event.resourceId,
    "delivered" to event.delivered,
)

// -------------------------------------------------------------------- access rules

fun requireAdmin(user: User) {
    if (user.role != "admin") throw forbidden()
}

fun checkIfMatch(header: String?, version: Int) {
    if (header.isNullOrEmpty()) throw preconditionRequired()
    if (header != version.toString()) throw preconditionFailed()
}

fun checkIncludeDeleted(raw: String?, user: User): Boolean {
    if (raw == null) return false
    if (user.role != "admin") throw forbidden()
    return raw == "true"
}

// ---------------------------------------------------------------------- pagination

/** Sort by the tiebreak first, then stably by the requested field. */
fun paginate(rows: List<Map<String, Any?>>, page: Page): Map<String, Any?> {
    val tiebreak = if (rows.isNotEmpty() && "seq" in rows[0]) "seq" else "id"
    val byField = compareBy<Map<String, Any?>> { it[page.sort] as Comparable<*>? }
    val byTiebreak = compareBy<Map<String, Any?>> { it[tiebreak] as Comparable<*>? }
    val ordered = rows.sortedWith(
        (if (page.order == "desc") byField.reversed() else byField).then(byTiebreak)
    )
    return mapOf(
        "items" to ordered.drop(page.offset).take(page.limit),
        "total" to ordered.size,
        "limit" to page.limit,
        "offset" to page.offset,
    )
}

// --------------------------------------------------------------------------- tasks

fun readNote(
    actor: User,
    body: Map<String, Any?>,
    errors: MutableList<Map<String, String>>,
    current: String,
): String {
    if ("internalNote" !in body) return current
    if (actor.role != "admin") throw forbidden()
    val note = body["internalNote"] as? String ?: throw badRequest()
    if (note.length > MAX_TITLE_LENGTH) {
        errors.add(fail("internalNote", "internalNote is too long"))
    }
    return note
}

// ---------------------------------------------------------------- queries, reports

private fun hit(type: String, id: Int, label: String): Map<String, Any?> =
    mapOf("type" to type, "id" to id, "label" to label)

private fun group(key: String, picked: List<Task>): Map<String, Any?> = mapOf(
    "key" to key,
    "tasks" to picked.size,
    "totalScore" to picked.sumOf { computeScore(it.priority, it.status) },
)

fun checkBulkSize(operations: Any?) {
    if (operations !is List<*> || operations.isEmpty() || operations.size > MAX_BULK_ITEMS) {
        throw invalid(listOf(fail("operations", "operations is out of range")))
    }
}

class Service(private val store: Store) {

    // ----------------------------------------------------------------- serializers

    fun serializeProject(project: Project): Map<String, Any?> = mapOf(
        "id" to project.id,
        "name" to project.name,
        "ownerId" to project.ownerId,
        "taskCount" to store.taskCount(project.id),
        "version" to project.version,
        "deleted" to project.deleted,
    )

    // ---------------------------------------------------------------- access rules

    fun authenticate(header: String?): Caller {
        val token = if (header != null && header.startsWith("Bearer ")) header.substring(7) else ""
        val session = store.sessions[token] ?: throw unauthorized()
        val user = store.findUser(session.userId) ?: throw unauthorized()
        return Caller(user, session)
    }

    fun chargeQuota(user: User, session: Session): Int {
        if (session.used >= user.quota) throw quotaExceeded()
        val charged = session.copy(used = session.used + 1)
        store.sessions[charged.token] = charged
        return maxOf(user.quota - charged.used, 0)
    }

    fun reachableProject(projectId: Int?, user: User, includeDeleted: Boolean = false): Project {
        val project = store.findProject(projectId, includeDeleted) ?: throw notFound()
        if (user.role != "admin" && project.ownerId != user.id) throw forbidden()
        return project
    }

    fun reachableTask(taskId: Int?, user: User, includeDeleted: Boolean = false): Task {
        val task = store.findTask(taskId, includeDeleted) ?: throw notFound()
        reachableProject(task.projectId, user, true)
        return task
    }

    // ------------------------------------------------------------------------ auth

    fun login(username: String, password: String, token: String): User {
        val user = store.findByUsername(username)
        if (user == null || user.password != password) throw invalidCredentials()
        store.sessions[token] = Session(token, user.id)
        return user
    }

    // -------------------------------------------------------------------- projects

    fun createProject(actor: User, name: String, ownerId: Int?): Project {
        val errors = mutableListOf<Map<String, String>>()
        checkString(name, "name", MAX_NAME_LENGTH, errors)
        if (store.findUser(ownerId) == null) {
            errors.add(fail("ownerId", "ownerId is not a known user"))
        }
        if (errors.isNotEmpty()) throw invalid(errors)
        val taken = store.projects.values.any {
            it.ownerId == ownerId && it.name == name && !it.deleted
        }
        if (taken) throw conflict()
        val project = store.insertProject(name, ownerId!!)
        store.record(actor.id, "create", "project", project.id)
        return project
    }

    fun renameProject(actor: User, project: Project, name: String): Project {
        val errors = mutableListOf<Map<String, String>>()
        checkString(name, "name", MAX_NAME_LENGTH, errors)
        if (errors.isNotEmpty()) throw invalid(errors)
        val taken = store.projects.values.any {
            it.ownerId == project.ownerId && it.name == name && it.id != project.id && !it.deleted
        }
        if (taken) throw conflict()
        val renamed = project.copy(name = name, version = project.version + 1)
        store.projects[renamed.id] = renamed
        store.record(actor.id, "update", "project", renamed.id)
        return renamed
    }

    fun deleteProject(actor: User, project: Project): Project {
        val deleted = project.copy(version = project.version + 1, deleted = true)
        store.projects[deleted.id] = deleted
        store.record(actor.id, "delete", "project", deleted.id)
        for (task in store.liveTasksOf(project.id)) {
            deleteTask(actor, task)
        }
        return deleted
    }

    fun restoreProject(actor: User, project: Project): Project {
        if (!project.deleted) throw conflict()
        val restored = project.copy(version = project.version + 1, deleted = false)
        store.projects[restored.id] = restored
        store.record(actor.id, "restore", "project", restored.id)
        return restored
    }

    // ----------------------------------------------------------------------- tasks

    fun createTask(
        actor: User,
        project: Project,
        title: String,
        priority: Int?,
        assigneeId: Int?,
        note: String,
        errors: MutableList<Map<String, String>>,
    ): Task {
        checkString(title, "title", MAX_TITLE_LENGTH, errors)
        checkPriority(priority, errors)
        if (assigneeId != null && store.findUser(assigneeId) == null) {
            errors.add(fail("assigneeId", "assigneeId is not a known user"))
        }
        if (errors.isNotEmpty()) throw invalid(errors)
        val task = store.insertTask(project.id, title, priority!!, assigneeId, note)
        store.record(actor.id, "create", "task", task.id)
        return task
    }

    fun replaceTask(
        actor: User,
        task: Task,
        title: String,
        priority: Int?,
        assigneeId: Int?,
        note: String,
        errors: MutableList<Map<String, String>>,
    ): Task {
        checkString(title, "title", MAX_TITLE_LENGTH, errors)
        checkPriority(priority, errors)
        if (assigneeId != null && store.findUser(assigneeId) == null) {
            errors.add(fail("assigneeId", "assigneeId is not a known user"))
        }
        if (errors.isNotEmpty()) throw invalid(errors)
        val replaced = task.copy(
            title = title, priority = priority!!, assigneeId = assigneeId, internalNote = note,
            version = task.version + 1,
        )
        store.tasks[replaced.id] = replaced
        store.record(actor.id, "update", "task", replaced.id)
        return replaced
    }

    fun moveStatus(actor: User, task: Task, status: Any?): Task {
        val errors = mutableListOf<Map<String, String>>()
        checkStatus(status, errors)
        if (errors.isNotEmpty()) throw invalid(errors)
        val next = status as String
        if ((task.status to next) !in TRANSITIONS) throw invalidTransition()
        val moved = task.copy(status = next, version = task.version + 1)
        store.tasks[moved.id] = moved
        store.record(actor.id, "update", "task", moved.id)
        return moved
    }

    fun deleteTask(actor: User, task: Task): Task {
        val deleted = task.copy(version = task.version + 1, deleted = true)
        store.tasks[deleted.id] = deleted
        store.record(actor.id, "delete", "task", deleted.id)
        return deleted
    }

    fun restoreTask(actor: User, task: Task): Task {
        if (!task.deleted) throw conflict()
        val restored = task.copy(version = task.version + 1, deleted = false)
        store.tasks[restored.id] = restored
        store.record(actor.id, "restore", "task", restored.id)
        return restored
    }

    // -------------------------------------------------------------------- comments

    fun createComment(actor: User, task: Task, body: String): Comment {
        val errors = mutableListOf<Map<String, String>>()
        checkString(body, "body", MAX_COMMENT_LENGTH, errors)
        if (errors.isNotEmpty()) throw invalid(errors)
        val comment = store.insertComment(task.id, actor.id, body)
        store.record(actor.id, "create", "comment", comment.id)
        return comment
    }

    fun removeComment(actor: User, comment: Comment) {
        if (actor.role != "admin" && comment.authorId != actor.id) throw forbidden()
        store.comments.remove(comment.id)
        store.record(actor.id, "delete", "comment", comment.id)
    }

    // ----------------------------------------------------------------------- users

    fun createUser(actor: User, username: String, password: String, role: Any?, quota: Any?): User {
        val errors = mutableListOf<Map<String, String>>()
        checkString(username, "username", MAX_NAME_LENGTH, errors)
        checkString(password, "password", MAX_NAME_LENGTH, errors)
        checkRole(role, errors)
        checkQuota(quota, errors)
        if (errors.isNotEmpty()) throw invalid(errors)
        if (store.findByUsername(username) != null) throw conflict()
        val user = store.insertUser(username, password, role as String, quota as Int)
        store.record(actor.id, "create", "user", user.id)
        return user
    }

    fun updateUser(actor: User, user: User, body: Map<String, Any?>): User {
        val errors = mutableListOf<Map<String, String>>()
        if ("role" in body) checkRole(body["role"], errors)
        if ("quota" in body) checkQuota(body["quota"], errors)
        if (errors.isNotEmpty()) throw invalid(errors)
        val updated = user.copy(
            role = if ("role" in body) body["role"] as String else user.role,
            quota = if ("quota" in body) body["quota"] as Int else user.quota,
            version = user.version + 1,
        )
        store.users[updated.id] = updated
        store.record(actor.id, "update", "user", updated.id)
        return updated
    }

    fun deleteUser(actor: User, user: User): User {
        if (user.id == actor.id) throw conflict()
        val deleted = user.copy(version = user.version + 1, deleted = true)
        store.users[deleted.id] = deleted
        store.record(actor.id, "delete", "user", deleted.id)
        return deleted
    }

    // ------------------------------------------------------------ queries, reports

    fun visibleProjects(user: User, includeDeleted: Boolean): List<Project> =
        store.projects.values.filter {
            (includeDeleted || !it.deleted) && (user.role == "admin" || it.ownerId == user.id)
        }

    fun visibleTasks(user: User, includeDeleted: Boolean): List<Task> {
        val allowed = visibleProjects(user, true).map { it.id }.toSet()
        return store.tasks.values.filter {
            it.projectId in allowed && (includeDeleted || !it.deleted)
        }
    }

    fun search(user: User, query: String): Map<String, Any?> {
        val needle = query.lowercase()
        val results = visibleProjects(user, false)
            .filter { needle in it.name.lowercase() }
            .map { hit("project", it.id, it.name) } +
            visibleTasks(user, false)
                .filter { needle in it.title.lowercase() }
                .map { hit("task", it.id, it.title) }
        return mapOf("results" to results, "total" to results.size)
    }

    fun workload(user: User, groupBy: String): Map<String, Any?> {
        val rows = visibleTasks(user, false)
        val groups = when (groupBy) {
            "status" -> STATUSES.map { status ->
                group(status, rows.filter { it.status == status })
            }
            "assignee" -> {
                val named = rows.mapNotNull { it.assigneeId }.distinct().sorted()
                val loose = rows.filter { it.assigneeId == null }
                named.map { assignee ->
                    group(assignee.toString(), rows.filter { it.assigneeId == assignee })
                } + if (loose.isEmpty()) emptyList() else listOf(group("unassigned", loose))
            }
            else -> visibleProjects(user, false).map { project ->
                group(project.name, rows.filter { it.projectId == project.id })
            }
        }
        return mapOf("groupBy" to groupBy, "groups" to groups)
    }

    fun flushOutbox(): Int {
        val pending = store.outbox.withIndex().filter { !it.value.delivered }
        pending.forEach { (index, event) -> store.outbox[index] = event.copy(delivered = true) }
        return pending.size
    }

    fun metrics(): Map<String, Any?> = mapOf(
        "requests" to store.requests,
        "byStatus" to store.byStatus.entries.associate { (code, count) ->
            code.toString() to count
        },
        "byRoute" to store.byRoute.map { (route, count) ->
            mapOf("route" to route, "count" to count)
        },
        "auditEntries" to store.audit.size,
        "outboxPending" to store.outboxPending(),
    )

    fun stats(): Map<String, Any?> {
        val live = store.tasks.values.filter { !it.deleted }
        val byStatus = STATUSES.associateWith { status -> live.count { it.status == status } }
        val sumScore = live.sumOf { computeScore(it.priority, it.status) }
        val best = store.projects.values
            .filter { !it.deleted }
            .maxByOrNull { store.taskCount(it.id) }
        return mapOf(
            "projects" to store.projects.values.count { !it.deleted },
            "tasks" to live.size,
            "users" to store.users.values.count { !it.deleted },
            "sessions" to store.sessions.size,
            "comments" to store.comments.size,
            "byStatus" to byStatus,
            "avgScore" to if (live.isEmpty()) 0.0
            else Math.round(sumScore.toDouble() / live.size * 100) / 100.0,
            "topProjectName" to best?.name,
            "auditEntries" to store.audit.size,
            "outboxPending" to store.outboxPending(),
        )
    }
}
