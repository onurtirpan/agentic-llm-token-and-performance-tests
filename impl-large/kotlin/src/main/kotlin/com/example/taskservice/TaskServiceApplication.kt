// Task Service, large tier — HTTP routing, middleware and the entry point.
package com.example.taskservice

import com.fasterxml.jackson.databind.ObjectMapper
import jakarta.servlet.Filter
import jakarta.servlet.http.HttpServletRequest
import jakarta.servlet.http.HttpServletResponse
import java.util.UUID
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.context.annotation.Bean
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.DeleteMapping
import org.springframework.web.bind.annotation.ExceptionHandler
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PatchMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.PutMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RequestMapping
import org.springframework.web.bind.annotation.RestController
import org.springframework.web.servlet.HandlerMapping

val REQUEST_ID = ThreadLocal<String>()
val USER_ID = ThreadLocal<Int?>()
val QUOTA_REMAINING = ThreadLocal<Int?>()
val RESPONSE = ThreadLocal<HttpServletResponse>()

// ------------------------------------------------------------------------- helpers

fun envelope(error: AppError): Map<String, Any?> = mapOf(
    "error" to mapOf(
        "code" to error.code,
        "message" to error.message,
        "requestId" to REQUEST_ID.get(),
        "details" to error.details,
    )
)

fun whole(body: Map<String, Any?>, field: String, defaultValue: Int?): Int? =
    when (val value = if (field in body) body[field] else defaultValue) {
        null -> null
        is Int -> value
        else -> throw badRequest()
    }

fun text(body: Map<String, Any?>, field: String): String =
    (if (field in body) body[field] else "") as? String ?: throw badRequest()

fun parseId(raw: String): Int = raw.toIntOrNull() ?: throw badRequest()

fun readPage(request: HttpServletRequest, allowed: List<String>): Page {
    val errors = mutableListOf<Map<String, String>>()
    val rawLimit = request.getParameter("limit")
    val rawOffset = request.getParameter("offset")
    val sort = request.getParameter("sort") ?: allowed[0]
    val order = request.getParameter("order") ?: "asc"
    var limit = DEFAULT_LIMIT
    var offset = 0
    if (rawLimit != null) {
        limit = rawLimit.toIntOrNull() ?: -1
        if (limit < 1 || limit > MAX_LIMIT) errors.add(fail("limit", "limit is out of range"))
    }
    if (rawOffset != null) {
        offset = rawOffset.toIntOrNull() ?: -1
        if (offset < 0) errors.add(fail("offset", "offset is out of range"))
    }
    if (sort !in allowed) errors.add(fail("sort", "sort is not a valid field"))
    if (order != "asc" && order != "desc") errors.add(fail("order", "order must be asc or desc"))
    if (errors.isNotEmpty()) throw invalid(errors)
    return Page(limit, offset, sort, order)
}

/** The raw header dodges the quotes that Spring pads onto a ResponseEntity ETag. */
fun tagged(body: Map<String, Any?>, version: Int): ResponseEntity<Any> {
    RESPONSE.get().setHeader("ETag", version.toString())
    return ResponseEntity.ok(body)
}

fun ifMatch(request: HttpServletRequest, version: Int) {
    checkIfMatch(request.getHeader("If-Match"), version)
}

/** A single-resource body carries its version, so the ETag comes for free. */
fun responded(status: Int, body: Map<String, Any?>): ResponseEntity<Any> {
    val version = body["version"]
    if (version != null) RESPONSE.get().setHeader("ETag", version.toString())
    return ResponseEntity.status(status).body(body)
}

fun taskFilters(request: HttpServletRequest, rows: List<Task>): List<Task> {
    val errors = mutableListOf<Map<String, String>>()
    val status = request.getParameter("status")
    val assignee = request.getParameter("assigneeId")
    var wanted: Int? = null
    if (status != null && status !in STATUSES) errors.add(fail("status", "status is not valid"))
    if (assignee != null) {
        wanted = assignee.toIntOrNull()
        if (wanted == null) errors.add(fail("assigneeId", "assigneeId is not a known user"))
    }
    if (errors.isNotEmpty()) throw invalid(errors)
    return rows.filter {
        (status == null || it.status == status) && (wanted == null || it.assigneeId == wanted)
    }
}

fun outcome(status: Int, id: Int): Map<String, Any?> =
    mapOf("status" to status, "id" to id, "error" to null)

@SpringBootApplication
@RestController
class TaskServiceApplication {

    private val mapper = ObjectMapper()
    private val store = Store().apply { seed() }
    private val service = Service(store)

    // ---------------------------------------------------------------- middleware

    @Bean
    fun observe(): Filter = Filter { request, response, chain ->
        val inbound = request as HttpServletRequest
        val outbound = response as HttpServletResponse
        val requestId = inbound.getHeader("X-Request-Id")?.takeIf { it.isNotEmpty() }
            ?: UUID.randomUUID().toString().replace("-", "").substring(0, 12)
        REQUEST_ID.set(requestId)
        USER_ID.set(null)
        QUOTA_REMAINING.set(null)
        RESPONSE.set(outbound)
        outbound.setHeader("X-Request-Id", requestId)
        val before = store.audit.size
        val started = System.nanoTime()
        chain.doFilter(request, response)
        val status = outbound.status
        val pattern = inbound.getAttribute(HandlerMapping.BEST_MATCHING_PATTERN_ATTRIBUTE)
        store.countRequest(
            if (pattern == null) "unmatched" else "${inbound.method} $pattern", status
        )
        val entry = mapOf(
            "level" to when {
                status >= 500 -> "error"
                status >= 400 -> "warn"
                else -> "info"
            },
            "requestId" to requestId,
            "method" to inbound.method,
            "path" to inbound.requestURI,
            "status" to status,
            "durationMs" to ((System.nanoTime() - started) / 1_000_000).toInt(),
            "userId" to USER_ID.get(),
            "quotaRemaining" to QUOTA_REMAINING.get(),
            "auditSeq" to store.audit.size - before,
        )
        println(mapper.writeValueAsString(entry))
    }

    @ExceptionHandler(AppError::class)
    fun onAppError(error: AppError): ResponseEntity<Any> =
        ResponseEntity.status(error.status).body(envelope(error))

    // ------------------------------------------------------------------- helpers

    /** Authenticate, charge the quota, then check the role. This order is fixed. */
    fun begin(request: HttpServletRequest, admin: Boolean = false): Caller {
        val caller = service.authenticate(request.getHeader("Authorization"))
        USER_ID.set(caller.user.id)
        val remaining = service.chargeQuota(caller.user, caller.session)
        QUOTA_REMAINING.set(remaining)
        RESPONSE.get().setHeader("X-Quota-Remaining", remaining.toString())
        if (admin) requireAdmin(caller.user)
        return caller
    }

    @Suppress("UNCHECKED_CAST")
    fun readBody(body: String?): Map<String, Any?> {
        if (body.isNullOrBlank()) return emptyMap()
        val parsed = try {
            mapper.readValue(body, Any::class.java)
        } catch (error: Exception) {
            throw badRequest()
        }
        return parsed as? Map<String, Any?> ?: throw badRequest()
    }

    /** Run produce once per Idempotency-Key, then replay the recorded outcome. */
    fun idempotent(
        request: HttpServletRequest,
        session: Session,
        produce: () -> Recorded,
    ): ResponseEntity<Any> {
        val key = request.getHeader("Idempotency-Key")
            ?: return produce().let { responded(it.status, it.body) }
        val slot = session.token to key
        store.idempotency[slot]?.let { seen ->
            RESPONSE.get().setHeader("Idempotency-Replayed", "true")
            return responded(seen.status, seen.body)
        }
        val made = try {
            produce()
        } catch (error: AppError) {
            store.idempotency[slot] = Recorded(error.status, envelope(error))
            throw error
        }
        store.idempotency[slot] = made
        return responded(made.status, made.body)
    }

    // -------------------------------------------------------------- health, auth

    @GetMapping("/health")
    fun getHealth(): ResponseEntity<Any> = ResponseEntity.ok(
        mapOf(
            "status" to "ok",
            "projects" to store.projects.values.count { !it.deleted },
            "tasks" to store.tasks.values.count { !it.deleted },
            "comments" to store.comments.size,
        )
    )

    @PostMapping("/auth/login")
    fun login(@RequestBody(required = false) body: String?): ResponseEntity<Any> {
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val username = text(parsed, "username")
        val password = text(parsed, "password")
        if (username.isEmpty()) errors.add(fail("username", "username is required"))
        if (password.isEmpty()) errors.add(fail("password", "password is required"))
        if (errors.isNotEmpty()) throw invalid(errors)
        val token = UUID.randomUUID().toString().replace("-", "")
        val user = service.login(username, password, token)
        return ResponseEntity.ok(mapOf("token" to token, "userId" to user.id, "role" to user.role))
    }

    @PostMapping("/auth/logout")
    fun logout(request: HttpServletRequest): ResponseEntity<Any> {
        val caller = begin(request)
        store.sessions.remove(caller.session.token)
        return ResponseEntity.noContent().build()
    }

    @GetMapping("/me")
    fun getMe(request: HttpServletRequest): ResponseEntity<Any> {
        val user = begin(request).user
        return ResponseEntity.ok(
            mapOf("userId" to user.id, "username" to user.username, "role" to user.role)
        )
    }

    // --------------------------------------------------------------------- users

    @GetMapping("/users")
    fun listUsers(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        val page = readPage(request, USER_SORTS)
        val rows = store.users.values.filter { !it.deleted }.map { serializeUser(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/users")
    fun createUser(
        request: HttpServletRequest,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val parsed = readBody(body)
        return idempotent(request, caller.session) {
            val role = if ("role" in parsed) parsed["role"] else "user"
            val quota = if ("quota" in parsed) parsed["quota"] else DEFAULT_QUOTA
            val made = service.createUser(
                caller.user, text(parsed, "username"), text(parsed, "password"), role, quota
            )
            Recorded(201, serializeUser(made))
        }
    }

    @GetMapping("/users/{id}")
    fun getUser(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        begin(request, admin = true)
        val user = store.findUser(parseId(id)) ?: throw notFound()
        return tagged(serializeUser(user), user.version)
    }

    @PatchMapping("/users/{id}")
    fun updateUser(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val user = store.findUser(parseId(id)) ?: throw notFound()
        ifMatch(request, user.version)
        val updated = service.updateUser(caller.user, user, readBody(body))
        return tagged(serializeUser(updated), updated.version)
    }

    @DeleteMapping("/users/{id}")
    fun deleteUser(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val user = store.findUser(parseId(id)) ?: throw notFound()
        ifMatch(request, user.version)
        val deleted = service.deleteUser(caller.user, user)
        return tagged(serializeUser(deleted), deleted.version)
    }

    // ------------------------------------------------------------------ projects

    @GetMapping("/projects")
    fun listProjects(request: HttpServletRequest): ResponseEntity<Any> {
        val user = begin(request).user
        val include = checkIncludeDeleted(request.getParameter("includeDeleted"), user)
        val page = readPage(request, PROJECT_SORTS)
        val rows = service.visibleProjects(user, include).map { service.serializeProject(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/projects")
    fun createProject(
        request: HttpServletRequest,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val parsed = readBody(body)
        return idempotent(request, caller.session) {
            val project = service.createProject(
                caller.user, text(parsed, "name"), whole(parsed, "ownerId", caller.user.id)
            )
            Recorded(201, service.serializeProject(project))
        }
    }

    @GetMapping("/projects/{id}")
    fun getProject(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = begin(request).user
        val project = service.reachableProject(parseId(id), user)
        return tagged(service.serializeProject(project), project.version)
    }

    @PatchMapping("/projects/{id}")
    fun updateProject(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        var project = service.reachableProject(parseId(id), caller.user)
        ifMatch(request, project.version)
        val parsed = readBody(body)
        if ("name" in parsed) {
            project = service.renameProject(caller.user, project, text(parsed, "name"))
        }
        return tagged(service.serializeProject(project), project.version)
    }

    @DeleteMapping("/projects/{id}")
    fun deleteProject(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val project = service.reachableProject(parseId(id), caller.user)
        ifMatch(request, project.version)
        val deleted = service.deleteProject(caller.user, project)
        return tagged(service.serializeProject(deleted), deleted.version)
    }

    @PostMapping("/projects/{id}/restore")
    fun restoreProject(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request, admin = true)
        val project = service.reachableProject(parseId(id), caller.user, true)
        ifMatch(request, project.version)
        val restored = service.restoreProject(caller.user, project)
        return tagged(service.serializeProject(restored), restored.version)
    }

    // --------------------------------------------------------------------- tasks

    @GetMapping("/tasks")
    fun listAllTasks(request: HttpServletRequest): ResponseEntity<Any> {
        val user = begin(request).user
        val include = checkIncludeDeleted(request.getParameter("includeDeleted"), user)
        val page = readPage(request, TASK_SORTS)
        val rows = taskFilters(request, service.visibleTasks(user, include))
            .map { serializeTask(it, user.role) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @GetMapping("/projects/{id}/tasks")
    fun listTasks(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = begin(request).user
        val project = service.reachableProject(parseId(id), user)
        val page = readPage(request, TASK_SORTS)
        val rows = store.liveTasksOf(project.id).map { serializeTask(it, user.role) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/projects/{id}/tasks")
    fun createTask(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request)
        val project = service.reachableProject(parseId(id), caller.user)
        val parsed = readBody(body)
        return idempotent(request, caller.session) {
            val errors = mutableListOf<Map<String, String>>()
            val note = readNote(caller.user, parsed, errors, "")
            val task = service.createTask(
                caller.user, project, text(parsed, "title"), whole(parsed, "priority", 0),
                whole(parsed, "assigneeId", null), note, errors,
            )
            Recorded(201, serializeTask(task, caller.user.role))
        }
    }

    @GetMapping("/tasks/{id}")
    fun getTask(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = begin(request).user
        val task = service.reachableTask(parseId(id), user)
        return tagged(serializeTask(task, user.role), task.version)
    }

    @PutMapping("/tasks/{id}")
    fun replaceTask(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request)
        val task = service.reachableTask(parseId(id), caller.user)
        ifMatch(request, task.version)
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val note = readNote(caller.user, parsed, errors, task.internalNote)
        val replaced = service.replaceTask(
            caller.user, task, text(parsed, "title"), whole(parsed, "priority", 0),
            whole(parsed, "assigneeId", null), note, errors,
        )
        return tagged(serializeTask(replaced, caller.user.role), replaced.version)
    }

    @PatchMapping("/tasks/{id}/status")
    fun updateStatus(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request)
        val task = service.reachableTask(parseId(id), caller.user)
        ifMatch(request, task.version)
        val moved = service.moveStatus(caller.user, task, readBody(body)["status"])
        return tagged(serializeTask(moved, caller.user.role), moved.version)
    }

    @DeleteMapping("/tasks/{id}")
    fun deleteTask(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request)
        val task = service.reachableTask(parseId(id), caller.user)
        ifMatch(request, task.version)
        val deleted = service.deleteTask(caller.user, task)
        return tagged(serializeTask(deleted, caller.user.role), deleted.version)
    }

    @PostMapping("/tasks/{id}/restore")
    fun restoreTask(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request)
        val task = service.reachableTask(parseId(id), caller.user, true)
        ifMatch(request, task.version)
        val restored = service.restoreTask(caller.user, task)
        return tagged(serializeTask(restored, caller.user.role), restored.version)
    }

    @PostMapping("/tasks/bulk")
    @Suppress("UNCHECKED_CAST")
    fun bulkTasks(
        request: HttpServletRequest,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request)
        val operations = readBody(body)["operations"]
        checkBulkSize(operations)
        val results = (operations as List<*>).mapIndexed { index, item ->
            try {
                val entry = item as? Map<String, Any?> ?: throw badRequest()
                mapOf<String, Any?>("index" to index) + applyBulk(caller.user, entry)
            } catch (error: AppError) {
                mapOf(
                    "index" to index, "status" to error.status, "id" to null,
                    "error" to error.code,
                )
            }
        }
        return ResponseEntity.ok(mapOf("results" to results))
    }

    fun applyBulk(actor: User, item: Map<String, Any?>): Map<String, Any?> = when (item["op"]) {
        "create" -> {
            val project = service.reachableProject(whole(item, "projectId", 0), actor)
            val task = service.createTask(
                actor, project, text(item, "title"), whole(item, "priority", 0), null, "",
                mutableListOf(),
            )
            outcome(201, task.id)
        }
        "status" -> {
            val task = service.reachableTask(whole(item, "id", 0), actor)
            checkIfMatch(item["version"].toString(), task.version)
            service.moveStatus(actor, task, item["status"])
            outcome(200, task.id)
        }
        "delete" -> {
            val task = service.reachableTask(whole(item, "id", 0), actor)
            checkIfMatch(item["version"].toString(), task.version)
            service.deleteTask(actor, task)
            outcome(200, task.id)
        }
        else -> throw invalid(listOf(fail("op", "op is not valid")))
    }

    // ------------------------------------------------------------------ comments

    @GetMapping("/tasks/{id}/comments")
    fun listComments(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = begin(request).user
        val task = service.reachableTask(parseId(id), user)
        val page = readPage(request, COMMENT_SORTS)
        val rows = store.comments.values
            .filter { it.taskId == task.id }
            .map { serializeComment(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/tasks/{id}/comments")
    fun createComment(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val caller = begin(request)
        val task = service.reachableTask(parseId(id), caller.user)
        val parsed = readBody(body)
        return idempotent(request, caller.session) {
            val comment = service.createComment(caller.user, task, text(parsed, "body"))
            Recorded(201, serializeComment(comment))
        }
    }

    @DeleteMapping("/comments/{id}")
    fun deleteComment(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val caller = begin(request)
        val comment = store.findComment(parseId(id)) ?: throw notFound()
        service.reachableTask(comment.taskId, caller.user, true)
        service.removeComment(caller.user, comment)
        return ResponseEntity.noContent().build()
    }

    // ------------------------------------------- search, reports and telemetry

    @GetMapping("/search")
    fun search(request: HttpServletRequest): ResponseEntity<Any> {
        val user = begin(request).user
        val query = request.getParameter("q") ?: ""
        if (query.isEmpty()) throw invalid(listOf(fail("q", "q is required")))
        return ResponseEntity.ok(service.search(user, query))
    }

    @GetMapping("/reports/workload")
    fun workload(request: HttpServletRequest): ResponseEntity<Any> {
        val user = begin(request).user
        val groupBy = request.getParameter("groupBy") ?: "status"
        if (groupBy !in GROUP_BYS) throw invalid(listOf(fail("groupBy", "groupBy is not valid")))
        return ResponseEntity.ok(service.workload(user, groupBy))
    }

    @GetMapping("/audit")
    fun listAudit(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        val page = readPage(request, SEQ_SORTS)
        val actorId = request.getParameter("actorId")
        val resource = request.getParameter("resource")
        val action = request.getParameter("action")
        val rows = store.audit
            .filter { actorId == null || it.actorId.toString() == actorId }
            .filter { resource == null || it.resource == resource }
            .filter { action == null || it.action == action }
            .map { serializeAudit(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @GetMapping("/outbox")
    fun listOutbox(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        val page = readPage(request, SEQ_SORTS)
        val wanted = request.getParameter("delivered")
        val rows = store.outbox
            .filter { wanted == null || it.delivered == (wanted == "true") }
            .map { serializeOutbox(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/outbox/flush")
    fun flushOutbox(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        return ResponseEntity.ok(mapOf("flushed" to service.flushOutbox()))
    }

    @GetMapping("/metrics")
    fun getMetrics(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        return ResponseEntity.ok(service.metrics())
    }

    @GetMapping("/stats")
    fun getStats(request: HttpServletRequest): ResponseEntity<Any> {
        begin(request, admin = true)
        return ResponseEntity.ok(service.stats())
    }

    @RequestMapping("/**")
    fun fallback(): ResponseEntity<Any> = throw notFound()
}

fun main(args: Array<String>) {
    runApplication<TaskServiceApplication>(*args)
}
