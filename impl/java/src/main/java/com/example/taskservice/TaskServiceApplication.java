// Task Service — Spring Boot implementation.
package com.example.taskservice;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

@SpringBootApplication
@RestController
public class TaskServiceApplication {

    static final int MAX_TITLE_LENGTH = 80;
    static final int MIN_PRIORITY = 1;
    static final int MAX_PRIORITY = 5;

    record Task(int id, String title, int priority, boolean done, int score) {}

    record TaskInput(String title, int priority, boolean done) {}

    record RawInput(String title, Integer priority, Boolean done) {}

    private final TreeMap<Integer, Task> tasks = new TreeMap<>();
    private final ObjectMapper mapper = new ObjectMapper();
    private int nextId = 1;

    static int computeScore(int priority, boolean done) {
        int baseScore = priority * 10;
        return done ? baseScore : baseScore + 5;
    }

    static String validate(String title, int priority) {
        if (title.isEmpty()) return "title is required";
        if (title.length() > MAX_TITLE_LENGTH) return "title is too long";
        if (priority < MIN_PRIORITY || priority > MAX_PRIORITY) return "priority is out of range";
        return null;
    }

    static ResponseEntity<Object> fail(HttpStatus status, String message) {
        return ResponseEntity.status(status).body(Map.of("error", message));
    }

    static Integer parseId(String raw) {
        try {
            return Integer.valueOf(raw);
        } catch (NumberFormatException error) {
            return null;
        }
    }

    TaskInput readInput(String body) {
        try {
            RawInput raw = mapper.readValue(body, RawInput.class);
            return new TaskInput(
                    raw.title() == null ? "" : raw.title(),
                    raw.priority() == null ? 0 : raw.priority(),
                    raw.done() != null && raw.done());
        } catch (Exception error) {
            return null;
        }
    }

    @GetMapping("/health")
    ResponseEntity<Object> getHealth() {
        return ResponseEntity.ok(Map.of("status", "ok", "count", tasks.size()));
    }

    @GetMapping("/tasks")
    ResponseEntity<Object> listTasks(@RequestParam(required = false) String done) {
        if (done != null && !done.equals("true") && !done.equals("false")) {
            return fail(HttpStatus.BAD_REQUEST, "done must be true or false");
        }
        ArrayList<Task> selected = new ArrayList<>();
        for (Task task : tasks.values()) {
            if (done == null || task.done() == done.equals("true")) selected.add(task);
        }
        selected.sort(Comparator.comparingInt(Task::score).reversed()
                .thenComparingInt(Task::id));
        return ResponseEntity.ok(Map.of("tasks", selected, "total", selected.size()));
    }

    @GetMapping("/tasks/{id}")
    ResponseEntity<Object> getTask(@PathVariable String id) {
        Integer taskId = parseId(id);
        if (taskId == null) return fail(HttpStatus.BAD_REQUEST, "invalid id");
        Task task = tasks.get(taskId);
        if (task == null) return fail(HttpStatus.NOT_FOUND, "task not found");
        return ResponseEntity.ok(task);
    }

    @PostMapping("/tasks")
    ResponseEntity<Object> createTask(@RequestBody(required = false) String body) {
        TaskInput input = readInput(body);
        if (input == null) return fail(HttpStatus.BAD_REQUEST, "invalid json");
        String error = validate(input.title(), input.priority());
        if (error != null) return fail(HttpStatus.BAD_REQUEST, error);
        Task task = new Task(nextId, input.title(), input.priority(), false,
                computeScore(input.priority(), false));
        tasks.put(nextId, task);
        nextId += 1;
        return ResponseEntity.status(HttpStatus.CREATED).body(task);
    }

    @PutMapping("/tasks/{id}")
    ResponseEntity<Object> updateTask(@PathVariable String id,
            @RequestBody(required = false) String body) {
        Integer taskId = parseId(id);
        if (taskId == null) return fail(HttpStatus.BAD_REQUEST, "invalid id");
        if (!tasks.containsKey(taskId)) return fail(HttpStatus.NOT_FOUND, "task not found");
        TaskInput input = readInput(body);
        if (input == null) return fail(HttpStatus.BAD_REQUEST, "invalid json");
        String error = validate(input.title(), input.priority());
        if (error != null) return fail(HttpStatus.BAD_REQUEST, error);
        Task task = new Task(taskId, input.title(), input.priority(), input.done(),
                computeScore(input.priority(), input.done()));
        tasks.put(taskId, task);
        return ResponseEntity.ok(task);
    }

    @DeleteMapping("/tasks/{id}")
    ResponseEntity<Object> deleteTask(@PathVariable String id) {
        Integer taskId = parseId(id);
        if (taskId == null) return fail(HttpStatus.BAD_REQUEST, "invalid id");
        if (tasks.remove(taskId) == null) return fail(HttpStatus.NOT_FOUND, "task not found");
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/stats")
    ResponseEntity<Object> getStats() {
        int total = tasks.size();
        int doneCount = 0;
        int sumScore = 0;
        Task best = null;
        for (Task task : tasks.values()) {
            if (task.done()) doneCount += 1;
            sumScore += task.score();
            if (!task.done() && (best == null || task.priority() > best.priority())) best = task;
        }
        double avgScore = total == 0 ? 0.0 : Math.round((double) sumScore / total * 100) / 100.0;
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("total", total);
        body.put("doneCount", doneCount);
        body.put("openCount", total - doneCount);
        body.put("avgScore", avgScore);
        body.put("topOpenTitle", best == null ? null : best.title());
        return ResponseEntity.ok(body);
    }

    @RequestMapping("/**")
    ResponseEntity<Object> fallback() {
        return fail(HttpStatus.NOT_FOUND, "not found");
    }

    public static void main(String[] args) {
        SpringApplication.run(TaskServiceApplication.class, args);
    }
}
