// Task Service, large tier — domain types, constants and pure rules.
package com.example.taskservice

const val MAX_TITLE_LENGTH = 80
const val MAX_NAME_LENGTH = 60
const val MAX_COMMENT_LENGTH = 200
const val MAX_BULK_ITEMS = 20
const val MIN_PRIORITY = 1
const val MAX_PRIORITY = 5
const val DEFAULT_LIMIT = 20
const val MAX_LIMIT = 100
const val DEFAULT_QUOTA = 10000
const val PROBE_QUOTA = 5

val ROLES = listOf("admin", "user")
val STATUSES = listOf("todo", "in_progress", "done", "archived")
val STATUS_BONUS = mapOf("todo" to 0, "in_progress" to 3, "done" to 5, "archived" to 0)
val TRANSITIONS = setOf(
    "todo" to "in_progress", "todo" to "archived", "in_progress" to "todo",
    "in_progress" to "done", "done" to "archived",
)
val PROJECT_SORTS = listOf("id", "name", "taskCount")
val TASK_SORTS = listOf("id", "title", "priority", "score", "status")
val USER_SORTS = listOf("id", "username", "role")
val COMMENT_SORTS = listOf("id", "authorId")
val SEQ_SORTS = listOf("seq")
val GROUP_BYS = listOf("assignee", "status", "project")

data class User(
    val id: Int,
    val username: String,
    val password: String,
    val role: String,
    val quota: Int,
    val version: Int = 1,
    val deleted: Boolean = false,
)

data class Session(val token: String, val userId: Int, val used: Int = 0)

data class Project(
    val id: Int,
    val name: String,
    val ownerId: Int,
    val version: Int = 1,
    val deleted: Boolean = false,
)

data class Task(
    val id: Int,
    val projectId: Int,
    val title: String,
    val priority: Int,
    val status: String,
    val assigneeId: Int?,
    val internalNote: String = "",
    val version: Int = 1,
    val deleted: Boolean = false,
)

data class Comment(val id: Int, val taskId: Int, val authorId: Int, val body: String)

data class AuditEntry(
    val seq: Int,
    val actorId: Int,
    val action: String,
    val resource: String,
    val resourceId: Int,
)

data class OutboxEvent(
    val seq: Int,
    val name: String,
    val resourceId: Int,
    val delivered: Boolean = false,
)

data class Caller(val user: User, val session: Session)

data class Page(val limit: Int, val offset: Int, val sort: String, val order: String)

data class Recorded(val status: Int, val body: Map<String, Any?>)

/** Every failure path throws this. The api layer turns it into the envelope. */
class AppError(
    val status: Int,
    val code: String,
    message: String,
    val details: List<Map<String, String>> = emptyList(),
) : RuntimeException(message)

fun badRequest() = AppError(400, "bad_request", "the request is malformed")

fun unauthorized() = AppError(401, "unauthorized", "authentication is required")

fun invalidCredentials() = AppError(401, "invalid_credentials", "the username or password is wrong")

fun forbidden() = AppError(403, "forbidden", "you may not access this resource")

fun notFound() = AppError(404, "not_found", "the resource does not exist")

fun conflict() = AppError(409, "conflict", "the resource already exists")

fun invalidTransition() = AppError(409, "invalid_transition", "the status change is not allowed")

fun preconditionFailed() = AppError(412, "precondition_failed", "the resource has changed")

fun preconditionRequired() =
    AppError(428, "precondition_required", "the If-Match header is required")

fun quotaExceeded() = AppError(429, "quota_exceeded", "the request quota is exhausted")

fun invalid(details: List<Map<String, String>>) = AppError(
    422,
    "validation_failed",
    "the request body is not valid",
    details.sortedWith(compareBy<Map<String, String>>({ it["field"] }, { it["message"] })),
)

fun fail(field: String, message: String) = mapOf("field" to field, "message" to message)

fun computeScore(priority: Int, status: String): Int {
    val baseScore = priority * 10
    return baseScore + STATUS_BONUS.getValue(status)
}

fun checkString(
    value: String,
    fieldName: String,
    maxLength: Int,
    errors: MutableList<Map<String, String>>,
) {
    if (value.isEmpty()) {
        errors.add(fail(fieldName, "$fieldName is required"))
    } else if (value.length > maxLength) {
        errors.add(fail(fieldName, "$fieldName is too long"))
    }
}

fun checkPriority(value: Int?, errors: MutableList<Map<String, String>>) {
    if (value == null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
        errors.add(fail("priority", "priority is out of range"))
    }
}

fun checkStatus(value: Any?, errors: MutableList<Map<String, String>>) {
    if (value !is String || value !in STATUSES) {
        errors.add(fail("status", "status is not valid"))
    }
}

fun checkRole(value: Any?, errors: MutableList<Map<String, String>>) {
    if (value !is String || value !in ROLES) {
        errors.add(fail("role", "role is not valid"))
    }
}

fun checkQuota(value: Any?, errors: MutableList<Map<String, String>>) {
    if (value !is Int || value < 0) {
        errors.add(fail("quota", "quota is out of range"))
    }
}
