// Task Service — Spring Boot implementation.
package com.example.taskservice

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.http.HttpStatus
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.DeleteMapping
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.PutMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RequestMapping
import org.springframework.web.bind.annotation.RequestParam
import org.springframework.web.bind.annotation.RestController

const val MAX_TITLE_LENGTH = 80
const val MIN_PRIORITY = 1
const val MAX_PRIORITY = 5

data class Task(val id: Int, val title: String, val priority: Int, val done: Boolean, val score: Int)

data class TaskInput(val title: String, val priority: Int, val done: Boolean)

data class RawInput(val title: String?, val priority: Int?, val done: Boolean?)

fun computeScore(priority: Int, done: Boolean): Int {
    val baseScore = priority * 10
    return if (done) baseScore else baseScore + 5
}

fun validate(title: String, priority: Int): String? = when {
    title.isEmpty() -> "title is required"
    title.length > MAX_TITLE_LENGTH -> "title is too long"
    priority < MIN_PRIORITY || priority > MAX_PRIORITY -> "priority is out of range"
    else -> null
}

fun fail(status: HttpStatus, message: String): ResponseEntity<Any> =
    ResponseEntity.status(status).body(mapOf("error" to message))

@SpringBootApplication
@RestController
class TaskServiceApplication {

    private val tasks = sortedMapOf<Int, Task>()
    private val mapper = ObjectMapper().registerKotlinModule()
    private var nextId = 1

    fun readInput(body: String?): TaskInput? = try {
        val raw = mapper.readValue(body, RawInput::class.java)
        TaskInput(raw.title ?: "", raw.priority ?: 0, raw.done ?: false)
    } catch (error: Exception) {
        null
    }

    @GetMapping("/health")
    fun getHealth(): ResponseEntity<Any> =
        ResponseEntity.ok(mapOf("status" to "ok", "count" to tasks.size))

    @GetMapping("/tasks")
    fun listTasks(@RequestParam(required = false) done: String?): ResponseEntity<Any> {
        if (done != null && done != "true" && done != "false") {
            return fail(HttpStatus.BAD_REQUEST, "done must be true or false")
        }
        val selected = tasks.values
            .filter { done == null || it.done == (done == "true") }
            .sortedWith(compareByDescending<Task> { it.score }.thenBy { it.id })
        return ResponseEntity.ok(mapOf("tasks" to selected, "total" to selected.size))
    }

    @GetMapping("/tasks/{id}")
    fun getTask(@PathVariable id: String): ResponseEntity<Any> {
        val taskId = id.toIntOrNull() ?: return fail(HttpStatus.BAD_REQUEST, "invalid id")
        val task = tasks[taskId] ?: return fail(HttpStatus.NOT_FOUND, "task not found")
        return ResponseEntity.ok(task)
    }

    @PostMapping("/tasks")
    fun createTask(@RequestBody(required = false) body: String?): ResponseEntity<Any> {
        val input = readInput(body) ?: return fail(HttpStatus.BAD_REQUEST, "invalid json")
        validate(input.title, input.priority)?.let { return fail(HttpStatus.BAD_REQUEST, it) }
        val task = Task(nextId, input.title, input.priority, false,
            computeScore(input.priority, false))
        tasks[nextId] = task
        nextId += 1
        return ResponseEntity.status(HttpStatus.CREATED).body(task)
    }

    @PutMapping("/tasks/{id}")
    fun updateTask(
        @PathVariable id: String,
        @RequestBody(required = false) body: String?,
    ): ResponseEntity<Any> {
        val taskId = id.toIntOrNull() ?: return fail(HttpStatus.BAD_REQUEST, "invalid id")
        if (taskId !in tasks) return fail(HttpStatus.NOT_FOUND, "task not found")
        val input = readInput(body) ?: return fail(HttpStatus.BAD_REQUEST, "invalid json")
        validate(input.title, input.priority)?.let { return fail(HttpStatus.BAD_REQUEST, it) }
        val task = Task(taskId, input.title, input.priority, input.done,
            computeScore(input.priority, input.done))
        tasks[taskId] = task
        return ResponseEntity.ok(task)
    }

    @DeleteMapping("/tasks/{id}")
    fun deleteTask(@PathVariable id: String): ResponseEntity<Any> {
        val taskId = id.toIntOrNull() ?: return fail(HttpStatus.BAD_REQUEST, "invalid id")
        tasks.remove(taskId) ?: return fail(HttpStatus.NOT_FOUND, "task not found")
        return ResponseEntity.noContent().build()
    }

    @GetMapping("/stats")
    fun getStats(): ResponseEntity<Any> {
        val all = tasks.values
        val total = all.size
        val doneCount = all.count { it.done }
        val sumScore = all.sumOf { it.score }
        val avgScore =
            if (total == 0) 0.0 else Math.round(sumScore.toDouble() / total * 100) / 100.0
        var best: Task? = null
        for (task in all) {
            if (!task.done && (best == null || task.priority > best.priority)) best = task
        }
        return ResponseEntity.ok(
            linkedMapOf(
                "total" to total,
                "doneCount" to doneCount,
                "openCount" to total - doneCount,
                "avgScore" to avgScore,
                "topOpenTitle" to best?.title,
            )
        )
    }

    @RequestMapping("/**")
    fun fallback(): ResponseEntity<Any> = fail(HttpStatus.NOT_FOUND, "not found")
}

fun main(args: Array<String>) {
    runApplication<TaskServiceApplication>(*args)
}
