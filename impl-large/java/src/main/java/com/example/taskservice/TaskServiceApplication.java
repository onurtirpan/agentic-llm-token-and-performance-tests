// Task Service, large tier — HTTP routing, middleware and the entry point.
package com.example.taskservice;

import static com.example.taskservice.Domain.COMMENT_SORTS;
import static com.example.taskservice.Domain.DEFAULT_LIMIT;
import static com.example.taskservice.Domain.DEFAULT_QUOTA;
import static com.example.taskservice.Domain.GROUP_BYS;
import static com.example.taskservice.Domain.MAX_LIMIT;
import static com.example.taskservice.Domain.PROJECT_SORTS;
import static com.example.taskservice.Domain.SEQ_SORTS;
import static com.example.taskservice.Domain.STATUSES;
import static com.example.taskservice.Domain.TASK_SORTS;
import static com.example.taskservice.Domain.USER_SORTS;
import static com.example.taskservice.Domain.badRequest;
import static com.example.taskservice.Domain.fail;
import static com.example.taskservice.Domain.invalid;
import static com.example.taskservice.Domain.notFound;

import com.example.taskservice.Domain.AppError;
import com.example.taskservice.Domain.AuditEntry;
import com.example.taskservice.Domain.Caller;
import com.example.taskservice.Domain.Comment;
import com.example.taskservice.Domain.OutboxEvent;
import com.example.taskservice.Domain.Page;
import com.example.taskservice.Domain.Project;
import com.example.taskservice.Domain.Recorded;
import com.example.taskservice.Domain.Session;
import com.example.taskservice.Domain.Task;
import com.example.taskservice.Domain.User;
import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.servlet.DispatcherType;
import jakarta.servlet.Filter;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.function.Supplier;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.web.servlet.FilterRegistrationBean;
import org.springframework.context.annotation.Bean;
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
import org.springframework.web.servlet.HandlerMapping;

@SpringBootApplication
@RestController
public class TaskServiceApplication {

    static final ThreadLocal<String> REQUEST_ID = new ThreadLocal<>();
    static final ThreadLocal<Integer> USER_ID = new ThreadLocal<>();
    static final ThreadLocal<Integer> QUOTA_REMAINING = new ThreadLocal<>();
    static final ThreadLocal<HttpServletResponse> RESPONSE = new ThreadLocal<>();

    private final ObjectMapper mapper = new ObjectMapper();
    private final Store store = new Store();
    private final Service service = new Service(store);

    TaskServiceApplication() {
        store.seed();
    }

    // ----------------------------------------------------------------- middleware

    @Bean
    FilterRegistrationBean<Filter> observe() {
        FilterRegistrationBean<Filter> registration = new FilterRegistrationBean<>(
                (request, response, chain) -> {
                    HttpServletRequest inbound = (HttpServletRequest) request;
                    HttpServletResponse outbound = (HttpServletResponse) response;
                    String header = inbound.getHeader("X-Request-Id");
                    String requestId = header == null || header.isEmpty()
                            ? UUID.randomUUID().toString().replace("-", "").substring(0, 12)
                            : header;
                    REQUEST_ID.set(requestId);
                    USER_ID.set(null);
                    QUOTA_REMAINING.set(null);
                    RESPONSE.set(outbound);
                    outbound.setHeader("X-Request-Id", requestId);
                    int before = store.audit.size();
                    long started = System.nanoTime();
                    chain.doFilter(request, response);
                    int status = outbound.getStatus();
                    Object pattern =
                            inbound.getAttribute(HandlerMapping.BEST_MATCHING_PATTERN_ATTRIBUTE);
                    store.countRequest(pattern == null ? "unmatched"
                            : inbound.getMethod() + " " + pattern, status);
                    LinkedHashMap<String, Object> entry = new LinkedHashMap<>();
                    entry.put("level", status >= 500 ? "error" : status >= 400 ? "warn" : "info");
                    entry.put("requestId", requestId);
                    entry.put("method", inbound.getMethod());
                    entry.put("path", inbound.getRequestURI());
                    entry.put("status", status);
                    entry.put("durationMs", (int) ((System.nanoTime() - started) / 1000000));
                    entry.put("userId", USER_ID.get());
                    entry.put("quotaRemaining", QUOTA_REMAINING.get());
                    entry.put("auditSeq", store.audit.size() - before);
                    System.out.print(mapper.writeValueAsString(entry) + "\n");
                    System.out.flush();
                });
        registration.setDispatcherTypes(DispatcherType.REQUEST);
        return registration;
    }

    Map<String, Object> envelope(AppError error) {
        LinkedHashMap<String, Object> inner = new LinkedHashMap<>();
        inner.put("code", error.code);
        inner.put("message", error.getMessage());
        inner.put("requestId", REQUEST_ID.get());
        inner.put("details", error.details);
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("error", inner);
        return body;
    }

    @ExceptionHandler(AppError.class)
    ResponseEntity<Object> onAppError(AppError error) {
        return ResponseEntity.status(error.status).body(envelope(error));
    }

    // -------------------------------------------------------------------- helpers

    /** Authenticate, charge the quota, then check the role. This order is fixed. */
    Caller begin(HttpServletRequest request, boolean admin) {
        Caller caller = service.authenticate(request.getHeader("Authorization"));
        USER_ID.set(caller.user().id());
        int remaining = service.chargeQuota(caller.user(), caller.session());
        QUOTA_REMAINING.set(remaining);
        RESPONSE.get().setHeader("X-Quota-Remaining", String.valueOf(remaining));
        if (admin) Service.requireAdmin(caller.user());
        return caller;
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

    static Integer whole(Map<String, Object> body, String field, Integer defaultValue) {
        Object value = body.containsKey(field) ? body.get(field) : defaultValue;
        if (value == null || value instanceof Integer) return (Integer) value;
        throw badRequest();
    }

    static String text(Map<String, Object> body, String field) {
        Object value = body.containsKey(field) ? body.get(field) : "";
        if (value instanceof String parsed) return parsed;
        throw badRequest();
    }

    static int parseId(String raw) {
        try {
            return Integer.parseInt(raw);
        } catch (NumberFormatException error) {
            throw badRequest();
        }
    }

    static int readInt(String raw, int defaultValue) {
        try {
            return Integer.parseInt(raw);
        } catch (NumberFormatException error) {
            return defaultValue;
        }
    }

    static Page readPage(HttpServletRequest request, List<String> allowed) {
        List<Map<String, String>> errors = new ArrayList<>();
        String rawLimit = request.getParameter("limit");
        String rawOffset = request.getParameter("offset");
        String sort = request.getParameter("sort") == null
                ? allowed.get(0) : request.getParameter("sort");
        String order = request.getParameter("order") == null
                ? "asc" : request.getParameter("order");
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

    /** The raw header dodges the quotes that Spring pads onto a ResponseEntity ETag. */
    static ResponseEntity<Object> tagged(Map<String, Object> body, int version) {
        RESPONSE.get().setHeader("ETag", String.valueOf(version));
        return ResponseEntity.ok(body);
    }

    void ifMatch(HttpServletRequest request, int version) {
        Service.checkIfMatch(request.getHeader("If-Match"), version);
    }

    /** A single-resource body carries its version, so the ETag comes for free. */
    static ResponseEntity<Object> responded(int status, Map<String, Object> body) {
        Object version = body.get("version");
        if (version != null) RESPONSE.get().setHeader("ETag", String.valueOf(version));
        return ResponseEntity.status(status).body(body);
    }

    /** Run produce once per Idempotency-Key, then replay the recorded outcome. */
    ResponseEntity<Object> idempotent(HttpServletRequest request, Session session,
            Supplier<Recorded> produce) {
        String key = request.getHeader("Idempotency-Key");
        if (key == null) {
            Recorded made = produce.get();
            return responded(made.status(), made.body());
        }
        String slot = session.token() + "\n" + key;
        Recorded seen = store.idempotency.get(slot);
        if (seen != null) {
            RESPONSE.get().setHeader("Idempotency-Replayed", "true");
            return responded(seen.status(), seen.body());
        }
        Recorded made;
        try {
            made = produce.get();
        } catch (AppError error) {
            store.idempotency.put(slot, new Recorded(error.status, envelope(error)));
            throw error;
        }
        store.idempotency.put(slot, made);
        return responded(made.status(), made.body());
    }

    // -------------------------------------------------------------- health, auth

    @GetMapping("/health")
    ResponseEntity<Object> getHealth() {
        int projects = 0;
        for (Project project : store.projects.values()) {
            if (!project.deleted()) projects += 1;
        }
        int tasks = 0;
        for (Task task : store.tasks.values()) {
            if (!task.deleted()) tasks += 1;
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("status", "ok");
        body.put("projects", projects);
        body.put("tasks", tasks);
        body.put("comments", store.comments.size());
        return ResponseEntity.ok(body);
    }

    @PostMapping("/auth/login")
    ResponseEntity<Object> login(@RequestBody(required = false) String body) {
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String username = text(parsed, "username");
        String password = text(parsed, "password");
        if (username.isEmpty()) errors.add(fail("username", "username is required"));
        if (password.isEmpty()) errors.add(fail("password", "password is required"));
        if (!errors.isEmpty()) throw invalid(errors);
        String token = UUID.randomUUID().toString().replace("-", "");
        User user = service.login(username, password, token);
        LinkedHashMap<String, Object> out = new LinkedHashMap<>();
        out.put("token", token);
        out.put("userId", user.id());
        out.put("role", user.role());
        return ResponseEntity.ok(out);
    }

    @PostMapping("/auth/logout")
    ResponseEntity<Object> logout(HttpServletRequest request) {
        Caller caller = begin(request, false);
        store.sessions.remove(caller.session().token());
        return ResponseEntity.noContent().build();
    }

    @GetMapping("/me")
    ResponseEntity<Object> getMe(HttpServletRequest request) {
        User user = begin(request, false).user();
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("userId", user.id());
        body.put("username", user.username());
        body.put("role", user.role());
        return ResponseEntity.ok(body);
    }

    // --------------------------------------------------------------------- users

    @GetMapping("/users")
    ResponseEntity<Object> listUsers(HttpServletRequest request) {
        begin(request, true);
        Page page = readPage(request, USER_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (User user : store.users.values()) {
            if (!user.deleted()) rows.add(Service.serializeUser(user));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @PostMapping("/users")
    ResponseEntity<Object> createUser(HttpServletRequest request,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, true);
        Map<String, Object> parsed = readBody(body);
        return idempotent(request, caller.session(), () -> {
            User made = service.createUser(caller.user(), text(parsed, "username"),
                    text(parsed, "password"), parsed.getOrDefault("role", "user"),
                    parsed.getOrDefault("quota", DEFAULT_QUOTA));
            return new Recorded(201, Service.serializeUser(made));
        });
    }

    @GetMapping("/users/{id}")
    ResponseEntity<Object> getUser(HttpServletRequest request, @PathVariable String id) {
        begin(request, true);
        User user = store.findUser(parseId(id), false);
        if (user == null) throw notFound();
        return tagged(Service.serializeUser(user), user.version());
    }

    @PatchMapping("/users/{id}")
    ResponseEntity<Object> updateUser(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, true);
        User user = store.findUser(parseId(id), false);
        if (user == null) throw notFound();
        ifMatch(request, user.version());
        User updated = service.updateUser(caller.user(), user, readBody(body));
        return tagged(Service.serializeUser(updated), updated.version());
    }

    @DeleteMapping("/users/{id}")
    ResponseEntity<Object> deleteUser(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, true);
        User user = store.findUser(parseId(id), false);
        if (user == null) throw notFound();
        ifMatch(request, user.version());
        User deleted = service.deleteUser(caller.user(), user);
        return tagged(Service.serializeUser(deleted), deleted.version());
    }

    // ------------------------------------------------------------------ projects

    @GetMapping("/projects")
    ResponseEntity<Object> listProjects(HttpServletRequest request) {
        User user = begin(request, false).user();
        boolean include =
                Service.checkIncludeDeleted(request.getParameter("includeDeleted"), user);
        Page page = readPage(request, PROJECT_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Project project : service.visibleProjects(user, include)) {
            rows.add(service.serializeProject(project));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @PostMapping("/projects")
    ResponseEntity<Object> createProject(HttpServletRequest request,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, true);
        Map<String, Object> parsed = readBody(body);
        return idempotent(request, caller.session(), () -> {
            Project project = service.createProject(caller.user(), text(parsed, "name"),
                    whole(parsed, "ownerId", caller.user().id()));
            return new Recorded(201, service.serializeProject(project));
        });
    }

    @GetMapping("/projects/{id}")
    ResponseEntity<Object> getProject(HttpServletRequest request, @PathVariable String id) {
        User user = begin(request, false).user();
        Project project = service.reachableProject(parseId(id), user, false);
        return tagged(service.serializeProject(project), project.version());
    }

    @PatchMapping("/projects/{id}")
    ResponseEntity<Object> updateProject(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, true);
        Project project = service.reachableProject(parseId(id), caller.user(), false);
        ifMatch(request, project.version());
        Map<String, Object> parsed = readBody(body);
        if (parsed.containsKey("name")) {
            project = service.renameProject(caller.user(), project, text(parsed, "name"));
        }
        return tagged(service.serializeProject(project), project.version());
    }

    @DeleteMapping("/projects/{id}")
    ResponseEntity<Object> deleteProject(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, true);
        Project project = service.reachableProject(parseId(id), caller.user(), false);
        ifMatch(request, project.version());
        Project deleted = service.deleteProject(caller.user(), project);
        return tagged(service.serializeProject(deleted), deleted.version());
    }

    @PostMapping("/projects/{id}/restore")
    ResponseEntity<Object> restoreProject(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, true);
        Project project = service.reachableProject(parseId(id), caller.user(), true);
        ifMatch(request, project.version());
        Project restored = service.restoreProject(caller.user(), project);
        return tagged(service.serializeProject(restored), restored.version());
    }

    // --------------------------------------------------------------------- tasks

    static List<Task> taskFilters(HttpServletRequest request, List<Task> rows) {
        List<Map<String, String>> errors = new ArrayList<>();
        String status = request.getParameter("status");
        String assignee = request.getParameter("assigneeId");
        Integer wanted = null;
        if (status != null && !STATUSES.contains(status)) {
            errors.add(fail("status", "status is not valid"));
        }
        if (assignee != null) {
            try {
                wanted = Integer.valueOf(assignee);
            } catch (NumberFormatException error) {
                errors.add(fail("assigneeId", "assigneeId is not a known user"));
            }
        }
        if (!errors.isEmpty()) throw invalid(errors);
        List<Task> kept = new ArrayList<>();
        for (Task task : rows) {
            if (status != null && !task.status().equals(status)) continue;
            if (wanted != null && !wanted.equals(task.assigneeId())) continue;
            kept.add(task);
        }
        return kept;
    }

    @GetMapping("/tasks")
    ResponseEntity<Object> listAllTasks(HttpServletRequest request) {
        User user = begin(request, false).user();
        boolean include =
                Service.checkIncludeDeleted(request.getParameter("includeDeleted"), user);
        Page page = readPage(request, TASK_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Task task : taskFilters(request, service.visibleTasks(user, include))) {
            rows.add(Service.serializeTask(task, user.role()));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @GetMapping("/projects/{id}/tasks")
    ResponseEntity<Object> listTasks(HttpServletRequest request, @PathVariable String id) {
        User user = begin(request, false).user();
        Project project = service.reachableProject(parseId(id), user, false);
        Page page = readPage(request, TASK_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Task task : store.liveTasksOf(project.id())) {
            rows.add(Service.serializeTask(task, user.role()));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @PostMapping("/projects/{id}/tasks")
    ResponseEntity<Object> createTask(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, false);
        Project project = service.reachableProject(parseId(id), caller.user(), false);
        Map<String, Object> parsed = readBody(body);
        return idempotent(request, caller.session(), () -> {
            List<Map<String, String>> errors = new ArrayList<>();
            String note = Service.readNote(caller.user(), parsed, errors, "");
            Task task = service.createTask(caller.user(), project, text(parsed, "title"),
                    whole(parsed, "priority", 0), whole(parsed, "assigneeId", null), note, errors);
            return new Recorded(201, Service.serializeTask(task, caller.user().role()));
        });
    }

    @GetMapping("/tasks/{id}")
    ResponseEntity<Object> getTask(HttpServletRequest request, @PathVariable String id) {
        User user = begin(request, false).user();
        Task task = service.reachableTask(parseId(id), user, false);
        return tagged(Service.serializeTask(task, user.role()), task.version());
    }

    @PutMapping("/tasks/{id}")
    ResponseEntity<Object> replaceTask(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, false);
        Task task = service.reachableTask(parseId(id), caller.user(), false);
        ifMatch(request, task.version());
        Map<String, Object> parsed = readBody(body);
        List<Map<String, String>> errors = new ArrayList<>();
        String note = Service.readNote(caller.user(), parsed, errors, task.internalNote());
        Task replaced = service.replaceTask(caller.user(), task, text(parsed, "title"),
                whole(parsed, "priority", 0), whole(parsed, "assigneeId", null), note, errors);
        return tagged(Service.serializeTask(replaced, caller.user().role()), replaced.version());
    }

    @PatchMapping("/tasks/{id}/status")
    ResponseEntity<Object> updateStatus(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, false);
        Task task = service.reachableTask(parseId(id), caller.user(), false);
        ifMatch(request, task.version());
        Task moved = service.moveStatus(caller.user(), task, readBody(body).get("status"));
        return tagged(Service.serializeTask(moved, caller.user().role()), moved.version());
    }

    @DeleteMapping("/tasks/{id}")
    ResponseEntity<Object> deleteTask(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, false);
        Task task = service.reachableTask(parseId(id), caller.user(), false);
        ifMatch(request, task.version());
        Task deleted = service.deleteTask(caller.user(), task);
        return tagged(Service.serializeTask(deleted, caller.user().role()), deleted.version());
    }

    @PostMapping("/tasks/{id}/restore")
    ResponseEntity<Object> restoreTask(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, false);
        Task task = service.reachableTask(parseId(id), caller.user(), true);
        ifMatch(request, task.version());
        Task restored = service.restoreTask(caller.user(), task);
        return tagged(Service.serializeTask(restored, caller.user().role()), restored.version());
    }

    @PostMapping("/tasks/bulk")
    @SuppressWarnings("unchecked")
    ResponseEntity<Object> bulkTasks(HttpServletRequest request,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, false);
        Object operations = readBody(body).get("operations");
        Service.checkBulkSize(operations);
        List<?> items = (List<?>) operations;
        List<Map<String, Object>> results = new ArrayList<>();
        for (int index = 0; index < items.size(); index += 1) {
            LinkedHashMap<String, Object> result = new LinkedHashMap<>();
            result.put("index", index);
            try {
                if (!(items.get(index) instanceof Map)) throw badRequest();
                result.putAll(applyBulk(caller.user(), (Map<String, Object>) items.get(index)));
            } catch (AppError error) {
                result.put("status", error.status);
                result.put("id", null);
                result.put("error", error.code);
            }
            results.add(result);
        }
        return ResponseEntity.ok(Map.of("results", results));
    }

    Map<String, Object> applyBulk(User actor, Map<String, Object> item) {
        Object operation = item.get("op");
        int id;
        if ("create".equals(operation)) {
            Project project = service.reachableProject(whole(item, "projectId", 0), actor, false);
            id = service.createTask(actor, project, text(item, "title"),
                    whole(item, "priority", 0), null, "", new ArrayList<>()).id();
            return outcome(201, id);
        }
        if ("status".equals(operation)) {
            Task task = service.reachableTask(whole(item, "id", 0), actor, false);
            Service.checkIfMatch(String.valueOf(item.get("version")), task.version());
            service.moveStatus(actor, task, item.get("status"));
            return outcome(200, task.id());
        }
        if ("delete".equals(operation)) {
            Task task = service.reachableTask(whole(item, "id", 0), actor, false);
            Service.checkIfMatch(String.valueOf(item.get("version")), task.version());
            service.deleteTask(actor, task);
            return outcome(200, task.id());
        }
        List<Map<String, String>> errors = new ArrayList<>();
        errors.add(fail("op", "op is not valid"));
        throw invalid(errors);
    }

    static Map<String, Object> outcome(int status, int id) {
        LinkedHashMap<String, Object> result = new LinkedHashMap<>();
        result.put("status", status);
        result.put("id", id);
        result.put("error", null);
        return result;
    }

    // ------------------------------------------------------------------ comments

    @GetMapping("/tasks/{id}/comments")
    ResponseEntity<Object> listComments(HttpServletRequest request, @PathVariable String id) {
        User user = begin(request, false).user();
        Task task = service.reachableTask(parseId(id), user, false);
        Page page = readPage(request, COMMENT_SORTS);
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Comment comment : store.comments.values()) {
            if (comment.taskId() == task.id()) rows.add(Service.serializeComment(comment));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @PostMapping("/tasks/{id}/comments")
    ResponseEntity<Object> createComment(HttpServletRequest request, @PathVariable String id,
            @RequestBody(required = false) String body) {
        Caller caller = begin(request, false);
        Task task = service.reachableTask(parseId(id), caller.user(), false);
        Map<String, Object> parsed = readBody(body);
        return idempotent(request, caller.session(), () -> {
            Comment comment = service.createComment(caller.user(), task, text(parsed, "body"));
            return new Recorded(201, Service.serializeComment(comment));
        });
    }

    @DeleteMapping("/comments/{id}")
    ResponseEntity<Object> deleteComment(HttpServletRequest request, @PathVariable String id) {
        Caller caller = begin(request, false);
        Comment comment = store.findComment(parseId(id));
        if (comment == null) throw notFound();
        service.reachableTask(comment.taskId(), caller.user(), true);
        service.removeComment(caller.user(), comment);
        return ResponseEntity.noContent().build();
    }

    // ----------------------------------------- search, reports and telemetry

    @GetMapping("/search")
    ResponseEntity<Object> search(HttpServletRequest request) {
        User user = begin(request, false).user();
        String query = request.getParameter("q") == null ? "" : request.getParameter("q");
        if (query.isEmpty()) {
            List<Map<String, String>> errors = new ArrayList<>();
            errors.add(fail("q", "q is required"));
            throw invalid(errors);
        }
        return ResponseEntity.ok(service.search(user, query));
    }

    @GetMapping("/reports/workload")
    ResponseEntity<Object> workload(HttpServletRequest request) {
        User user = begin(request, false).user();
        String groupBy = request.getParameter("groupBy") == null
                ? "status" : request.getParameter("groupBy");
        if (!GROUP_BYS.contains(groupBy)) {
            List<Map<String, String>> errors = new ArrayList<>();
            errors.add(fail("groupBy", "groupBy is not valid"));
            throw invalid(errors);
        }
        return ResponseEntity.ok(service.workload(user, groupBy));
    }

    @GetMapping("/audit")
    ResponseEntity<Object> listAudit(HttpServletRequest request) {
        begin(request, true);
        Page page = readPage(request, SEQ_SORTS);
        String actorId = request.getParameter("actorId");
        String resource = request.getParameter("resource");
        String action = request.getParameter("action");
        List<Map<String, Object>> rows = new ArrayList<>();
        for (AuditEntry entry : store.audit) {
            if (actorId != null && !String.valueOf(entry.actorId()).equals(actorId)) continue;
            if (resource != null && !entry.resource().equals(resource)) continue;
            if (action != null && !entry.action().equals(action)) continue;
            rows.add(Service.serializeAudit(entry));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @GetMapping("/outbox")
    ResponseEntity<Object> listOutbox(HttpServletRequest request) {
        begin(request, true);
        Page page = readPage(request, SEQ_SORTS);
        String wanted = request.getParameter("delivered");
        List<Map<String, Object>> rows = new ArrayList<>();
        for (OutboxEvent event : store.outbox) {
            if (wanted != null && event.delivered() != wanted.equals("true")) continue;
            rows.add(Service.serializeOutbox(event));
        }
        return ResponseEntity.ok(Service.paginate(rows, page));
    }

    @PostMapping("/outbox/flush")
    ResponseEntity<Object> flushOutbox(HttpServletRequest request) {
        begin(request, true);
        return ResponseEntity.ok(Map.of("flushed", service.flushOutbox()));
    }

    @GetMapping("/metrics")
    ResponseEntity<Object> getMetrics(HttpServletRequest request) {
        begin(request, true);
        return ResponseEntity.ok(service.metrics());
    }

    @GetMapping("/stats")
    ResponseEntity<Object> getStats(HttpServletRequest request) {
        begin(request, true);
        return ResponseEntity.ok(service.stats());
    }

    @RequestMapping("/**")
    ResponseEntity<Object> fallback() {
        throw notFound();
    }

    public static void main(String[] args) {
        SpringApplication.run(TaskServiceApplication.class, args);
    }
}
