// Task Service, mid tier — Spring Boot implementation.
package com.example.taskservice

import com.fasterxml.jackson.databind.ObjectMapper
import jakarta.servlet.Filter
import jakarta.servlet.http.HttpServletRequest
import jakarta.servlet.http.HttpServletResponse
import java.util.UUID
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.context.annotation.Bean
import org.springframework.http.HttpStatus
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

const val MAX_TITLE_LENGTH = 80
const val MAX_NAME_LENGTH = 60
const val MIN_PRIORITY = 1
const val MAX_PRIORITY = 5
const val DEFAULT_LIMIT = 20
const val MAX_LIMIT = 100

val STATUS_BONUS = mapOf("todo" to 0, "in_progress" to 3, "done" to 5, "archived" to 0)
val TRANSITIONS = setOf(
    "todo" to "in_progress", "todo" to "archived", "in_progress" to "todo",
    "in_progress" to "done", "done" to "archived",
)
val PROJECT_SORTS = listOf("id", "name", "taskCount")
val TASK_SORTS = listOf("id", "title", "priority", "score", "status")

val REQUEST_ID = ThreadLocal<String>()
val USER_ID = ThreadLocal<Int?>()

data class User(val id: Int, val username: String, val password: String, val role: String)

data class Project(val id: Int, val name: String, val ownerId: Int)

data class Task(
    val id: Int,
    val projectId: Int,
    val title: String,
    val priority: Int,
    val status: String,
    val assigneeId: Int?,
    val score: Int,
)

data class Page(val limit: Int, val offset: Int, val sort: String, val order: String)

class AppError(
    val status: Int,
    val code: String,
    message: String,
    val details: List<Map<String, String>> = emptyList(),
) : RuntimeException(message)

fun computeScore(priority: Int, status: String): Int {
    val baseScore = priority * 10
    return baseScore + STATUS_BONUS.getValue(status)
}

fun serializeTask(task: Task): Map<String, Any?> = mapOf(
    "id" to task.id,
    "projectId" to task.projectId,
    "title" to task.title,
    "priority" to task.priority,
    "status" to task.status,
    "assigneeId" to task.assigneeId,
    "score" to task.score,
)

fun badRequest() = AppError(400, "bad_request", "the request is malformed")

fun notFound() = AppError(404, "not_found", "the resource does not exist")

fun forbidden() = AppError(403, "forbidden", "you may not access this resource")

fun conflict() = AppError(409, "conflict", "the resource already exists")

fun invalid(details: List<Map<String, String>>) = AppError(
    422,
    "validation_failed",
    "the request body is not valid",
    details.sortedWith(compareBy<Map<String, String>>({ it["field"] }, { it["message"] })),
)

fun fail(field: String, message: String) = mapOf("field" to field, "message" to message)

fun readInt(body: Map<String, Any?>, field: String, defaultValue: Int?): Int? =
    when (val value = if (field in body) body[field] else defaultValue) {
        null -> null
        is Int -> value
        else -> throw badRequest()
    }

fun readString(
    body: Map<String, Any?>,
    field: String,
    errors: MutableList<Map<String, String>>,
    maxLength: Int,
    required: Boolean,
): String {
    val value = if (field in body) body[field] else ""
    if (value !is String) throw badRequest()
    if (value.isEmpty()) {
        if (required) errors.add(fail(field, "$field is required"))
    } else if (value.length > maxLength) {
        errors.add(fail(field, "$field is too long"))
    }
    return value
}

fun readPriority(body: Map<String, Any?>, errors: MutableList<Map<String, String>>): Int {
    val value = readInt(body, "priority", 0)
    if (value == null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
        errors.add(fail("priority", "priority is out of range"))
    }
    return value ?: 0
}

fun parseId(raw: String): Int = raw.toIntOrNull() ?: throw badRequest()

fun readPage(request: HttpServletRequest, allowed: List<String>): Page {
    val errors = mutableListOf<Map<String, String>>()
    val rawLimit = request.getParameter("limit")
    val rawOffset = request.getParameter("offset")
    val sort = request.getParameter("sort") ?: "id"
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

fun paginate(rows: List<Map<String, Any?>>, page: Page): Map<String, Any?> {
    val byField = compareBy<Map<String, Any?>> { it[page.sort] as Comparable<*>? }
    val ordered = rows.sortedWith(
        (if (page.order == "desc") byField.reversed() else byField).thenBy { it["id"] as Int }
    )
    return mapOf(
        "items" to ordered.drop(page.offset).take(page.limit),
        "total" to ordered.size,
        "limit" to page.limit,
        "offset" to page.offset,
    )
}

fun requireAdmin(user: User) {
    if (user.role != "admin") throw forbidden()
}

@SpringBootApplication
@RestController
class TaskServiceApplication {

    private val users = sortedMapOf(
        1 to User(1, "admin", "admin-secret", "admin"),
        2 to User(2, "alice", "alice-secret", "user"),
        3 to User(3, "bob", "bob-secret", "user"),
    )
    private val sessions = mutableMapOf<String, Int>()
    private val projects = sortedMapOf<Int, Project>()
    private val tasks = sortedMapOf<Int, Task>()
    private val mapper = ObjectMapper()
    private var nextProjectId = 1
    private var nextTaskId = 1

    fun taskCount(projectId: Int): Int = tasks.values.count { it.projectId == projectId }

    fun serializeProject(project: Project): Map<String, Any?> = mapOf(
        "id" to project.id,
        "name" to project.name,
        "ownerId" to project.ownerId,
        "taskCount" to taskCount(project.id),
    )

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

    fun readUserRef(
        body: Map<String, Any?>,
        field: String,
        errors: MutableList<Map<String, String>>,
        defaultValue: Int?,
    ): Int? {
        val value = readInt(body, field, defaultValue)
        if (value != null && value !in users) errors.add(fail(field, "$field is not a known user"))
        return value
    }

    fun authenticate(request: HttpServletRequest): User {
        val header = request.getHeader("Authorization").orEmpty()
        val session = if (header.startsWith("Bearer ")) sessions[header.substring(7)] else null
        if (session == null) throw AppError(401, "unauthorized", "authentication is required")
        USER_ID.set(session)
        return users.getValue(session)
    }

    fun reachableProject(projectId: Int, user: User): Project {
        val project = projects[projectId] ?: throw notFound()
        if (user.role != "admin" && project.ownerId != user.id) throw forbidden()
        return project
    }

    fun reachableTask(taskId: Int, user: User): Task {
        val task = tasks[taskId] ?: throw notFound()
        reachableProject(task.projectId, user)
        return task
    }

    @Bean
    fun observe(): Filter = Filter { request, response, chain ->
        val inbound = request as HttpServletRequest
        val outbound = response as HttpServletResponse
        val requestId = inbound.getHeader("X-Request-Id")?.takeIf { it.isNotEmpty() }
            ?: UUID.randomUUID().toString().replace("-", "").substring(0, 12)
        REQUEST_ID.set(requestId)
        USER_ID.set(null)
        outbound.setHeader("X-Request-Id", requestId)
        val started = System.nanoTime()
        chain.doFilter(request, response)
        val status = outbound.status
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
        )
        println(mapper.writeValueAsString(entry))
    }

    @ExceptionHandler(AppError::class)
    fun onAppError(error: AppError): ResponseEntity<Any> = ResponseEntity.status(error.status).body(
        mapOf(
            "error" to mapOf(
                "code" to error.code,
                "message" to error.message,
                "requestId" to REQUEST_ID.get(),
                "details" to error.details,
            )
        )
    )

    @GetMapping("/health")
    fun getHealth(): ResponseEntity<Any> = ResponseEntity.ok(
        mapOf("status" to "ok", "projects" to projects.size, "tasks" to tasks.size)
    )

    @PostMapping("/auth/login")
    fun login(@RequestBody(required = false) body: String?): ResponseEntity<Any> {
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val username = readString(parsed, "username", errors, MAX_NAME_LENGTH, true)
        val password = readString(parsed, "password", errors, MAX_NAME_LENGTH, true)
        if (errors.isNotEmpty()) throw invalid(errors)
        val user = users.values.find { it.username == username && it.password == password }
            ?: throw AppError(401, "invalid_credentials", "the username or password is wrong")
        val token = UUID.randomUUID().toString().replace("-", "")
        sessions[token] = user.id
        return ResponseEntity.ok(mapOf("token" to token, "userId" to user.id, "role" to user.role))
    }

    @PostMapping("/auth/logout")
    fun logout(request: HttpServletRequest): ResponseEntity<Any> {
        authenticate(request)
        sessions.remove(request.getHeader("Authorization").substring(7))
        return ResponseEntity.noContent().build()
    }

    @GetMapping("/me")
    fun getMe(request: HttpServletRequest): ResponseEntity<Any> {
        val user = authenticate(request)
        return ResponseEntity.ok(
            mapOf("userId" to user.id, "username" to user.username, "role" to user.role)
        )
    }

    @GetMapping("/projects")
    fun listProjects(request: HttpServletRequest): ResponseEntity<Any> {
        val user = authenticate(request)
        val page = readPage(request, PROJECT_SORTS)
        val rows = projects.values
            .filter { user.role == "admin" || it.ownerId == user.id }
            .map { serializeProject(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/projects")
    fun createProject(
        request: HttpServletRequest,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val user = authenticate(request)
        requireAdmin(user)
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val name = readString(parsed, "name", errors, MAX_NAME_LENGTH, true)
        val ownerId = readUserRef(parsed, "ownerId", errors, user.id)
        if (errors.isNotEmpty()) throw invalid(errors)
        if (projects.values.any { it.ownerId == ownerId && it.name == name }) throw conflict()
        val project = Project(nextProjectId, name, ownerId!!)
        projects[nextProjectId] = project
        nextProjectId += 1
        return ResponseEntity.status(HttpStatus.CREATED).body(serializeProject(project))
    }

    @GetMapping("/projects/{id}")
    fun getProject(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = authenticate(request)
        return ResponseEntity.ok(serializeProject(reachableProject(parseId(id), user)))
    }

    @PatchMapping("/projects/{id}")
    fun updateProject(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val user = authenticate(request)
        requireAdmin(user)
        val project = reachableProject(parseId(id), user)
        val parsed = readBody(body)
        if ("name" !in parsed) return ResponseEntity.ok(serializeProject(project))
        val errors = mutableListOf<Map<String, String>>()
        val name = readString(parsed, "name", errors, MAX_NAME_LENGTH, true)
        if (errors.isNotEmpty()) throw invalid(errors)
        val taken = projects.values.any {
            it.ownerId == project.ownerId && it.name == name && it.id != project.id
        }
        if (taken) throw conflict()
        val renamed = project.copy(name = name)
        projects[renamed.id] = renamed
        return ResponseEntity.ok(serializeProject(renamed))
    }

    @DeleteMapping("/projects/{id}")
    fun deleteProject(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = authenticate(request)
        requireAdmin(user)
        val project = reachableProject(parseId(id), user)
        tasks.values.removeAll { it.projectId == project.id }
        projects.remove(project.id)
        return ResponseEntity.noContent().build()
    }

    @GetMapping("/projects/{id}/tasks")
    fun listTasks(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = authenticate(request)
        val project = reachableProject(parseId(id), user)
        val page = readPage(request, TASK_SORTS)
        val rows = tasks.values.filter { it.projectId == project.id }.map { serializeTask(it) }
        return ResponseEntity.ok(paginate(rows, page))
    }

    @PostMapping("/projects/{id}/tasks")
    fun createTask(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val user = authenticate(request)
        val project = reachableProject(parseId(id), user)
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val title = readString(parsed, "title", errors, MAX_TITLE_LENGTH, true)
        val priority = readPriority(parsed, errors)
        val assigneeId = readUserRef(parsed, "assigneeId", errors, null)
        if (errors.isNotEmpty()) throw invalid(errors)
        val task = Task(
            nextTaskId, project.id, title, priority, "todo", assigneeId,
            computeScore(priority, "todo"),
        )
        tasks[nextTaskId] = task
        nextTaskId += 1
        return ResponseEntity.status(HttpStatus.CREATED).body(serializeTask(task))
    }

    @GetMapping("/tasks/{id}")
    fun getTask(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = authenticate(request)
        return ResponseEntity.ok(serializeTask(reachableTask(parseId(id), user)))
    }

    @PutMapping("/tasks/{id}")
    fun replaceTask(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val user = authenticate(request)
        val task = reachableTask(parseId(id), user)
        val parsed = readBody(body)
        val errors = mutableListOf<Map<String, String>>()
        val title = readString(parsed, "title", errors, MAX_TITLE_LENGTH, true)
        val priority = readPriority(parsed, errors)
        val assigneeId = readUserRef(parsed, "assigneeId", errors, null)
        if (errors.isNotEmpty()) throw invalid(errors)
        val replaced = task.copy(
            title = title, priority = priority, assigneeId = assigneeId,
            score = computeScore(priority, task.status),
        )
        tasks[replaced.id] = replaced
        return ResponseEntity.ok(serializeTask(replaced))
    }

    @PatchMapping("/tasks/{id}/status")
    fun updateStatus(
        request: HttpServletRequest,
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val user = authenticate(request)
        val task = reachableTask(parseId(id), user)
        val parsed = readBody(body)
        val next = parsed["status"] as? String
        if (next == null || next !in STATUS_BONUS) {
            throw invalid(listOf(fail("status", "status is not valid")))
        }
        if ((task.status to next) !in TRANSITIONS) {
            throw AppError(409, "invalid_transition", "the status change is not allowed")
        }
        val moved = task.copy(status = next, score = computeScore(task.priority, next))
        tasks[moved.id] = moved
        return ResponseEntity.ok(serializeTask(moved))
    }

    @DeleteMapping("/tasks/{id}")
    fun deleteTask(request: HttpServletRequest, @PathVariable id: String): ResponseEntity<Any> {
        val user = authenticate(request)
        tasks.remove(reachableTask(parseId(id), user).id)
        return ResponseEntity.noContent().build()
    }

    @GetMapping("/stats")
    fun getStats(request: HttpServletRequest): ResponseEntity<Any> {
        val user = authenticate(request)
        requireAdmin(user)
        val byStatus = STATUS_BONUS.keys.associateWith { name ->
            tasks.values.count { it.status == name }
        }
        val total = tasks.size
        val sumScore = tasks.values.sumOf { it.score }
        val avgScore =
            if (total == 0) 0.0 else Math.round(sumScore.toDouble() / total * 100) / 100.0
        val best = projects.values.maxByOrNull { taskCount(it.id) }
        return ResponseEntity.ok(
            mapOf(
                "projects" to projects.size,
                "tasks" to total,
                "users" to users.size,
                "sessions" to sessions.size,
                "byStatus" to byStatus,
                "avgScore" to avgScore,
                "topProjectName" to best?.name,
            )
        )
    }

    @RequestMapping("/**")
    fun fallback(): ResponseEntity<Any> = throw notFound()
}

fun main(args: Array<String>) {
    runApplication<TaskServiceApplication>(*args)
}
