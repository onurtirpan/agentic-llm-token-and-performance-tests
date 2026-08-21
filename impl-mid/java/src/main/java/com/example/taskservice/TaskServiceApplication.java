// Task Service, mid tier — Spring Boot implementation.
package com.example.taskservice;

import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.servlet.Filter;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PatchMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@SpringBootApplication
@RestController
public class TaskServiceApplication {

    static final int MAX_TITLE_LENGTH = 80;
    static final int MAX_NAME_LENGTH = 60;
    static final int MIN_PRIORITY = 1;
    static final int MAX_PRIORITY = 5;
    static final int DEFAULT_LIMIT = 20;
    static final int MAX_LIMIT = 100;

    static final Map<String, Integer> STATUS_BONUS = new LinkedHashMap<>();

    static {
        STATUS_BONUS.put("todo", 0);
        STATUS_BONUS.put("in_progress", 3);
        STATUS_BONUS.put("done", 5);
        STATUS_BONUS.put("archived", 0);
    }

    static final Set<String> TRANSITIONS = Set.of("todo>in_progress", "todo>archived",
            "in_progress>todo", "in_progress>done", "done>archived");
    static final List<String> PROJECT_SORTS = List.of("id", "name", "taskCount");
    static final List<String> TASK_SORTS = List.of("id", "title", "priority", "score", "status");

    static final ThreadLocal<String> REQUEST_ID = new ThreadLocal<>();
    static final ThreadLocal<Integer> USER_ID = new ThreadLocal<>();

    record User(int id, String username, String password, String role) {}

    record Project(int id, String name, int ownerId) {}

    record Task(int id, int projectId, String title, int priority, String status,
            Integer assigneeId, int score) {}

    record Page(int limit, int offset, String sort, String order) {}

    static class AppError extends RuntimeException {

        final int status;
        final String code;
        final List<Map<String, String>> details;

        AppError(int status, String code, String message, List<Map<String, String>> details) {
            super(message);
            this.status = status;
            this.code = code;
            this.details = details;
        }

        AppError(int status, String code, String message) {
            this(status, code, message, List.of());
        }
    }

    private final Map<Integer, User> users = new TreeMap<>(Map.of(
            1, new User(1, "admin", "admin-secret", "admin"),
            2, new User(2, "alice", "alice-secret", "user"),
            3, new User(3, "bob", "bob-secret", "user")));
    private final Map<String, Integer> sessions = new HashMap<>();
    private final Map<Integer, Project> projects = new TreeMap<>();
    private final Map<Integer, Task> tasks = new TreeMap<>();
    private final ObjectMapper mapper = new ObjectMapper();
    private int nextProjectId = 1;
    private int nextTaskId = 1;

    static int computeScore(int priority, String status) {
        int baseScore = priority * 10;
        return baseScore + STATUS_BONUS.get(status);
    }

    int taskCount(int projectId) {
        int count = 0;
        for (Task task : tasks.values()) {
            if (task.projectId() == projectId) count += 1;
        }
        return count;
    }

    Map<String, Object> serializeProject(Project project) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", project.id());
        row.put("name", project.name());
        row.put("ownerId", project.ownerId());
        row.put("taskCount", taskCount(project.id()));
        return row;
    }

    static Map<String, Object> serializeTask(Task task) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", task.id());
        row.put("projectId", task.projectId());
        row.put("title", task.title());
        row.put("priority", task.priority());
        row.put("status", task.status());
        row.put("assigneeId", task.assigneeId());
        row.put("score", task.score());
        return row;
    }

    static AppError badRequest() {
        return new AppError(400, "bad_request", "the request is malformed");
    }

    static AppError notFound() {
        return new AppError(404, "not_found", "the resource does not exist");
    }

    static AppError forbidden() {
        return new AppError(403, "forbidden", "you may not access this resource");
    }

    static AppError conflict() {
        return new AppError(409, "conflict", "the resource already exists");
    }

    static AppError invalid(List<Map<String, String>> details) {
        details.sort(Comparator.comparing((Map<String, String> entry) -> entry.get("field"))
                .thenComparing(entry -> entry.get("message")));
        return new AppError(422, "validation_failed", "the request body is not valid", details);
    }

    static Map<String, String> fail(String field, String message) {
        return Map.of("field", field, "message", message);
    }

    @SuppressWarnings("unchecked")
    Map<String, Object> readBody(String body) {
        if (body == null || body.isBlank()) return Map.of();
        Object parsed;
        try {
            parsed = mapper.readValue(body, Object.class);
        } catch (Exception error) {
            throw badRequest();
        }
        if (!(parsed instanceof Map)) throw badRequest();
        return (Map<String, Object>) parsed;
    }

    static Integer readInt(Map<String, Object> body, String field, Integer defaultValue) {
        Object value = body.containsKey(field) ? body.get(field) : defaultValue;
        if (value == null || value instanceof Integer) return (Integer) value;
        throw badRequest();
    }

    static int readInt(String raw, int defaultValue) {
        try {
            return Integer.parseInt(raw);
        } catch (NumberFormatException error) {
            return defaultValue;
        }
    }

    static String readString(Map<String, Object> body, String field,
            List<Map<String, String>> errors, int maxLength, boolean required) {
        Object value = body.containsKey(field) ? body.get(field) : "";
        if (!(value instanceof String text)) throw badRequest();
        if (text.isEmpty()) {
            if (required) errors.add(fail(field, field + " is required"));
        } else if (text.length() > maxLength) {
            errors.add(fail(field, field + " is too long"));
        }
        return text;
    }

    static int readPriority(Map<String, Object> body, List<Map<String, String>> errors) {
        Integer value = readInt(body, "priority", 0);
        if (value == null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
            errors.add(fail("priority", "priority is out of range"));
        }
        return value == null ? 0 : value;
    }

    Integer readUserRef(Map<String, Object> body, String field,
            List<Map<String, String>> errors, Integer defaultValue) {
        Integer value = readInt(body, field, defaultValue);
        if (value != null && !users.containsKey(value)) {
            errors.add(fail(field, field + " is not a known user"));
        }
        return value;
    }

    static int parseId(String raw) {
        try {
            return Integer.parseInt(raw);
        } catch (NumberFormatException error) {
            throw badRequest();
        }
    }

    static Page readPage(HttpServletRequest request, List<String> allowed) {
        List<Map<String, String>> errors = new ArrayList<>();
        String rawLimit = request.getParameter("limit");
        String rawOffset = request.getParameter("offset");
        String sort = request.getParameter("sort") == null ? "id" : request.getParameter("sort");
        String order = request.getParameter("order") == null ? "asc" : request.getParameter("order");
        int limit = DEFAULT_LIMIT;
        int offset = 0;
        if (rawLimit != null) {
            limit = readInt(rawLimit, -1);
            if (limit < 1 || limit > MAX_LIMIT) errors.add(fail("limit", "limit is out of range"));
        }
        if (rawOffset != null) {
            offset = readInt(rawOffset, -1);
            if (offset < 0) errors.add(fail("offset", "offset is out of range"));
        }
        if (!allowed.contains(sort)) errors.add(fail("sort", "sort is not a valid field"));
        if (!order.equals("asc") && !order.equals("desc")) {
            errors.add(fail("order", "order must be asc or desc"));
        }
        if (!errors.isEmpty()) throw invalid(errors);
        return new Page(limit, offset, sort, order);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    static Map<String, Object> paginate(List<Map<String, Object>> rows, Page page) {
        Comparator<Map<String, Object>> byId = Comparator.comparingInt(row -> (Integer) row.get("id"));
        Comparator<Map<String, Object>> byField =
                (left, right) -> ((Comparable) left.get(page.sort())).compareTo(right.get(page.sort()));
        rows.sort((page.order().equals("desc") ? byField.reversed() : byField).thenComparing(byId));
        int from = Math.min(page.offset(), rows.size());
        int to = Math.min(from + page.limit(), rows.size());
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("items", rows.subList(from, to));
        body.put("total", rows.size());
        body.put("limit", page.limit());
        body.put("offset", page.offset());
        return body;
    }

    User authenticate(HttpServletRequest request) {
        String header = request.getHeader("Authorization");
        Integer session = header != null && header.startsWith("Bearer ")
                ? sessions.get(header.substring(7)) : null;
        if (session == null) {
            throw new AppError(401, "unauthorized", "authentication is required");
        }
        USER_ID.set(session);
        return users.get(session);
    }

    static void requireAdmin(User user) {
        if (!user.role().equals("admin")) throw forbidden();
    }

    Project reachableProject(int projectId, User user) {
        Project project = projects.get(projectId);
        if (project == null) throw notFound();
        if (!user.role().equals("admin") && project.ownerId() != user.id()) throw forbidden();
        return project;
    }

    Task reachableTask(int taskId, User user) {
        Task task = tasks.get(taskId);
        if (task == null) throw notFound();
        reachableProject(task.projectId(), user);
        return task;
    }

    @Bean
    Filter observe() {
        return (request, response, chain) -> {
            HttpServletRequest inbound = (HttpServletRequest) request;
            HttpServletResponse outbound = (HttpServletResponse) response;
            String header = inbound.getHeader("X-Request-Id");
            String requestId = header == null || header.isEmpty()
                    ? UUID.randomUUID().toString().replace("-", "").substring(0, 12) : header;
            REQUEST_ID.set(requestId);
            USER_ID.set(null);
            outbound.setHeader("X-Request-Id", requestId);
            long started = System.nanoTime();
            chain.doFilter(request, response);
            int status = outbound.getStatus();
            LinkedHashMap<String, Object> entry = new LinkedHashMap<>();
            entry.put("level", status >= 500 ? "error" : status >= 400 ? "warn" : "info");
            entry.put("requestId", requestId);
            entry.put("method", inbound.getMethod());
            entry.put("path", inbound.getRequestURI());
            entry.put("status", status);
            entry.put("durationMs", (int) ((System.nanoTime() - started) / 1000000));
            entry.put("userId", USER_ID.get());
            System.out.print(mapper.writeValueAsString(entry) + "\n");
        };
    }

    @ExceptionHandler(AppError.class)
    ResponseEntity<Object> onAppError(AppError error) {
        LinkedHashMap<String, Object> envelope = new LinkedHashMap<>();
        envelope.put("code", error.code);
        envelope.put("message", error.getMessage());
        envelope.put("requestId", REQUEST_ID.get());
        envelope.put("details", error.details);
        return ResponseEntity.status(error.status).body(Map.of("error", envelope));
    }

    @GetMapping("/health")
    ResponseEntity<Object> getHealth() {
        return ResponseEntity.ok(Map.of("status", "ok", "projects", projects.size(),
                "tasks", tasks.size()));
    }

    @PostMapping("/auth/login")
    ResponseEntity<Object> login(@RequestBody(required = false) String body) {
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String username = readString(parsed, "username", errors, MAX_NAME_LENGTH, true);
        String password = readString(parsed, "password", errors, MAX_NAME_LENGTH, true);
        if (!errors.isEmpty()) throw invalid(errors);
        for (User user : users.values()) {
            if (user.username().equals(username) && user.password().equals(password)) {
                String token = UUID.randomUUID().toString().replace("-", "");
                sessions.put(token, user.id());
                return ResponseEntity.ok(Map.of("token", token, "userId", user.id(),
                        "role", user.role()));
            }
        }
        throw new AppError(401, "invalid_credentials", "the username or password is wrong");
    }

    @PostMapping("/auth/logout")
    ResponseEntity<Object> logout(HttpServletRequest request) {
        authenticate(request);
        sessions.remove(request.getHeader("Authorization").substring(7));
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/me")
    ResponseEntity<Object> getMe(HttpServletRequest request) {
        User user = authenticate(request);
        return ResponseEntity.ok(Map.of("userId", user.id(), "username", user.username(),
                "role", user.role()));
    }

    @GetMapping("/projects")
    ResponseEntity<Object> listProjects(HttpServletRequest request) {
        User user = authenticate(request);
        Page page = readPage(request, PROJECT_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Project project : projects.values()) {
            if (user.role().equals("admin") || project.ownerId() == user.id()) {
                rows.add(serializeProject(project));
            }
        }
        return ResponseEntity.ok(paginate(rows, page));
    }

    @PostMapping("/projects")
    ResponseEntity<Object> createProject(HttpServletRequest request,
            @RequestBody(required = false) String body) {
        User user = authenticate(request);
        requireAdmin(user);
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String name = readString(parsed, "name", errors, MAX_NAME_LENGTH, true);
        Integer ownerId = readUserRef(parsed, "ownerId", errors, user.id());
        if (!errors.isEmpty()) throw invalid(errors);
        for (Project other : projects.values()) {
            if (other.ownerId() == ownerId && other.name().equals(name)) throw conflict();
        }
        Project project = new Project(nextProjectId, name, ownerId);
        projects.put(nextProjectId, project);
        nextProjectId += 1;
        return ResponseEntity.status(HttpStatus.CREATED).body(serializeProject(project));
    }

    @GetMapping("/projects/{id}")
    ResponseEntity<Object> getProject(HttpServletRequest request, @PathVariable String id) {
        User user = authenticate(request);
        return ResponseEntity.ok(serializeProject(reachableProject(parseId(id), user)));
    }

    @PatchMapping("/projects/{id}")
    ResponseEntity<Object> updateProject(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        User user = authenticate(request);
        requireAdmin(user);
        Project project = reachableProject(parseId(id), user);
        Map<String, Object> parsed = readBody(body);
        if (!parsed.containsKey("name")) return ResponseEntity.ok(serializeProject(project));
        List<Map<String, String>> errors = new ArrayList<>();
        String name = readString(parsed, "name", errors, MAX_NAME_LENGTH, true);
        if (!errors.isEmpty()) throw invalid(errors);
        for (Project other : projects.values()) {
            if (other.ownerId() == project.ownerId() && other.name().equals(name)
                    && other.id() != project.id()) throw conflict();
        }
        Project renamed = new Project(project.id(), name, project.ownerId());
        projects.put(renamed.id(), renamed);
        return ResponseEntity.ok(serializeProject(renamed));
    }

    @DeleteMapping("/projects/{id}")
    ResponseEntity<Object> deleteProject(HttpServletRequest request, @PathVariable String id) {
        User user = authenticate(request);
        requireAdmin(user);
        Project project = reachableProject(parseId(id), user);
        tasks.values().removeIf(task -> task.projectId() == project.id());
        projects.remove(project.id());
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/projects/{id}/tasks")
    ResponseEntity<Object> listTasks(HttpServletRequest request, @PathVariable String id) {
        User user = authenticate(request);
        Project project = reachableProject(parseId(id), user);
        Page page = readPage(request, TASK_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Task task : tasks.values()) {
            if (task.projectId() == project.id()) rows.add(serializeTask(task));
        }
        return ResponseEntity.ok(paginate(rows, page));
    }

    @PostMapping("/projects/{id}/tasks")
    ResponseEntity<Object> createTask(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        User user = authenticate(request);
        Project project = reachableProject(parseId(id), user);
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String title = readString(parsed, "title", errors, MAX_TITLE_LENGTH, true);
        int priority = readPriority(parsed, errors);
        Integer assigneeId = readUserRef(parsed, "assigneeId", errors, null);
        if (!errors.isEmpty()) throw invalid(errors);
        Task task = new Task(nextTaskId, project.id(), title, priority, "todo", assigneeId,
                computeScore(priority, "todo"));
        tasks.put(nextTaskId, task);
        nextTaskId += 1;
        return ResponseEntity.status(HttpStatus.CREATED).body(serializeTask(task));
    }

    @GetMapping("/tasks/{id}")
    ResponseEntity<Object> getTask(HttpServletRequest request, @PathVariable String id) {
        User user = authenticate(request);
        return ResponseEntity.ok(serializeTask(reachableTask(parseId(id), user)));
    }

    @PutMapping("/tasks/{id}")
    ResponseEntity<Object> replaceTask(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        User user = authenticate(request);
        Task task = reachableTask(parseId(id), user);
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String title = readString(parsed, "title", errors, MAX_TITLE_LENGTH, true);
        int priority = readPriority(parsed, errors);
        Integer assigneeId = readUserRef(parsed, "assigneeId", errors, null);
        if (!errors.isEmpty()) throw invalid(errors);
        Task replaced = new Task(task.id(), task.projectId(), title, priority, task.status(),
                assigneeId, computeScore(priority, task.status()));
        tasks.put(replaced.id(), replaced);
        return ResponseEntity.ok(serializeTask(replaced));
    }

    @PatchMapping("/tasks/{id}/status")
    ResponseEntity<Object> updateStatus(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        User user = authenticate(request);
        Task task = reachableTask(parseId(id), user);
        Map<String, Object> parsed = readBody(body);
        Object status = parsed.get("status");
        if (!(status instanceof String next) || !STATUS_BONUS.containsKey(next)) {
            throw invalid(new ArrayList<>(List.of(fail("status", "status is not valid"))));
        }
        if (!TRANSITIONS.contains(task.status() + ">" + next)) {
            throw new AppError(409, "invalid_transition", "the status change is not allowed");
        }
        Task moved = new Task(task.id(), task.projectId(), task.title(), task.priority(), next,
                task.assigneeId(), computeScore(task.priority(), next));
        tasks.put(moved.id(), moved);
        return ResponseEntity.ok(serializeTask(moved));
    }

    @DeleteMapping("/tasks/{id}")
    ResponseEntity<Object> deleteTask(HttpServletRequest request, @PathVariable String id) {
        User user = authenticate(request);
        tasks.remove(reachableTask(parseId(id), user).id());
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/stats")
    ResponseEntity<Object> getStats(HttpServletRequest request) {
        User user = authenticate(request);
        requireAdmin(user);
        LinkedHashMap<String, Object> byStatus = new LinkedHashMap<>();
        for (String name : STATUS_BONUS.keySet()) byStatus.put(name, 0);
        int sumScore = 0;
        for (Task task : tasks.values()) {
            byStatus.put(task.status(), (Integer) byStatus.get(task.status()) + 1);
            sumScore += task.score();
        }
        int total = tasks.size();
        double avgScore = total == 0 ? 0.0
                : Math.round((double) sumScore / total * 100) / 100.0;
        Project best = null;
        for (Project project : projects.values()) {
            if (best == null || taskCount(project.id()) > taskCount(best.id())) best = project;
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("projects", projects.size());
        body.put("tasks", total);
        body.put("users", users.size());
        body.put("sessions", sessions.size());
        body.put("byStatus", byStatus);
        body.put("avgScore", avgScore);
        body.put("topProjectName", best == null ? null : best.name());
        return ResponseEntity.ok(body);
    }

    @RequestMapping("/**")
    ResponseEntity<Object> fallback() {
        throw notFound();
    }

    public static void main(String[] args) {
        SpringApplication.run(TaskServiceApplication.class, args);
    }
}
