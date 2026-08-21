// Task Service, large tier — business rules, authorization and audit emission.
package com.example.taskservice;

import static com.example.taskservice.Domain.MAX_BULK_ITEMS;
import static com.example.taskservice.Domain.MAX_COMMENT_LENGTH;
import static com.example.taskservice.Domain.MAX_NAME_LENGTH;
import static com.example.taskservice.Domain.MAX_TITLE_LENGTH;
import static com.example.taskservice.Domain.STATUSES;
import static com.example.taskservice.Domain.TRANSITIONS;
import static com.example.taskservice.Domain.badRequest;
import static com.example.taskservice.Domain.checkPriority;
import static com.example.taskservice.Domain.checkQuota;
import static com.example.taskservice.Domain.checkRole;
import static com.example.taskservice.Domain.checkStatus;
import static com.example.taskservice.Domain.checkString;
import static com.example.taskservice.Domain.computeScore;
import static com.example.taskservice.Domain.conflict;
import static com.example.taskservice.Domain.fail;
import static com.example.taskservice.Domain.forbidden;
import static com.example.taskservice.Domain.invalid;
import static com.example.taskservice.Domain.invalidCredentials;
import static com.example.taskservice.Domain.invalidTransition;
import static com.example.taskservice.Domain.notFound;
import static com.example.taskservice.Domain.preconditionFailed;
import static com.example.taskservice.Domain.preconditionRequired;
import static com.example.taskservice.Domain.quotaExceeded;
import static com.example.taskservice.Domain.unauthorized;

import com.example.taskservice.Domain.AuditEntry;
import com.example.taskservice.Domain.Caller;
import com.example.taskservice.Domain.Comment;
import com.example.taskservice.Domain.OutboxEvent;
import com.example.taskservice.Domain.Page;
import com.example.taskservice.Domain.Project;
import com.example.taskservice.Domain.Session;
import com.example.taskservice.Domain.Task;
import com.example.taskservice.Domain.User;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

final class Service {

    private final Store store;

    Service(Store store) {
        this.store = store;
    }

    // ------------------------------------------------------------- serializers

    static Map<String, Object> serializeUser(User user) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", user.id());
        row.put("username", user.username());
        row.put("role", user.role());
        row.put("quota", user.quota());
        row.put("version", user.version());
        row.put("deleted", user.deleted());
        return row;
    }

    Map<String, Object> serializeProject(Project project) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", project.id());
        row.put("name", project.name());
        row.put("ownerId", project.ownerId());
        row.put("taskCount", store.taskCount(project.id()));
        row.put("version", project.version());
        row.put("deleted", project.deleted());
        return row;
    }

    static Map<String, Object> serializeTask(Task task, String role) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", task.id());
        row.put("projectId", task.projectId());
        row.put("title", task.title());
        row.put("priority", task.priority());
        row.put("status", task.status());
        row.put("assigneeId", task.assigneeId());
        if (role.equals("admin")) row.put("internalNote", task.internalNote());
        row.put("version", task.version());
        row.put("deleted", task.deleted());
        row.put("score", computeScore(task.priority(), task.status()));
        return row;
    }

    static Map<String, Object> serializeComment(Comment comment) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("id", comment.id());
        row.put("taskId", comment.taskId());
        row.put("authorId", comment.authorId());
        row.put("body", comment.body());
        return row;
    }

    static Map<String, Object> serializeAudit(AuditEntry entry) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("seq", entry.seq());
        row.put("actorId", entry.actorId());
        row.put("action", entry.action());
        row.put("resource", entry.resource());
        row.put("resourceId", entry.resourceId());
        return row;
    }

    static Map<String, Object> serializeOutbox(OutboxEvent event) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("seq", event.seq());
        row.put("name", event.name());
        row.put("resourceId", event.resourceId());
        row.put("delivered", event.delivered());
        return row;
    }

    // ------------------------------------------------------------ access rules

    Caller authenticate(String header) {
        String token = header != null && header.startsWith("Bearer ") ? header.substring(7) : "";
        Session session = store.sessions.get(token);
        if (session == null) throw unauthorized();
        User user = store.findUser(session.userId(), false);
        if (user == null) throw unauthorized();
        return new Caller(user, session);
    }

    int chargeQuota(User user, Session session) {
        if (session.used() >= user.quota()) throw quotaExceeded();
        Session charged = new Session(session.token(), session.userId(), session.used() + 1);
        store.sessions.put(charged.token(), charged);
        return Math.max(user.quota() - charged.used(), 0);
    }

    static void requireAdmin(User user) {
        if (!user.role().equals("admin")) throw forbidden();
    }

    Project reachableProject(Integer projectId, User user, boolean includeDeleted) {
        Project project = store.findProject(projectId, includeDeleted);
        if (project == null) throw notFound();
        if (!user.role().equals("admin") && project.ownerId() != user.id()) throw forbidden();
        return project;
    }

    Task reachableTask(Integer taskId, User user, boolean includeDeleted) {
        Task task = store.findTask(taskId, includeDeleted);
        if (task == null) throw notFound();
        reachableProject(task.projectId(), user, true);
        return task;
    }

    static void checkIfMatch(String header, int version) {
        if (header == null || header.isEmpty()) throw preconditionRequired();
        if (!header.equals(String.valueOf(version))) throw preconditionFailed();
    }

    static boolean checkIncludeDeleted(String raw, User user) {
        if (raw == null) return false;
        if (!user.role().equals("admin")) throw forbidden();
        return raw.equals("true");
    }

    // -------------------------------------------------------------- pagination

    /** Sort by the tiebreak first, then stably by the requested field. */
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Map<String, Object> paginate(List<Map<String, Object>> rows, Page page) {
        String tiebreak = !rows.isEmpty() && rows.get(0).containsKey("seq") ? "seq" : "id";
        Comparator<Map<String, Object>> byTiebreak =
                (left, right) -> ((Comparable) left.get(tiebreak)).compareTo(right.get(tiebreak));
        Comparator<Map<String, Object>> byField = (left, right) ->
                ((Comparable) left.get(page.sort())).compareTo(right.get(page.sort()));
        rows.sort((page.order().equals("desc") ? byField.reversed() : byField)
                .thenComparing(byTiebreak));
        int from = Math.min(page.offset(), rows.size());
        int to = Math.min(from + page.limit(), rows.size());
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("items", rows.subList(from, to));
        body.put("total", rows.size());
        body.put("limit", page.limit());
        body.put("offset", page.offset());
        return body;
    }

    // -------------------------------------------------------------------- auth

    User login(String username, String password, String token) {
        User user = store.findByUsername(username);
        if (user == null || !user.password().equals(password)) throw invalidCredentials();
        store.sessions.put(token, new Session(token, user.id(), 0));
        return user;
    }

    // ---------------------------------------------------------------- projects

    Project createProject(User actor, String name, Integer ownerId) {
        List<Map<String, String>> errors = new ArrayList<>();
        checkString(name, "name", MAX_NAME_LENGTH, errors);
        if (store.findUser(ownerId, false) == null) {
            errors.add(fail("ownerId", "ownerId is not a known user"));
        }
        if (!errors.isEmpty()) throw invalid(errors);
        for (Project other : store.projects.values()) {
            if (other.ownerId() == ownerId && other.name().equals(name) && !other.deleted()) {
                throw conflict();
            }
        }
        Project project = store.insertProject(name, ownerId);
        store.record(actor.id(), "create", "project", project.id());
        return project;
    }

    Project renameProject(User actor, Project project, String name) {
        List<Map<String, String>> errors = new ArrayList<>();
        checkString(name, "name", MAX_NAME_LENGTH, errors);
        if (!errors.isEmpty()) throw invalid(errors);
        for (Project other : store.projects.values()) {
            if (other.ownerId() == project.ownerId() && other.name().equals(name)
                    && other.id() != project.id() && !other.deleted()) {
                throw conflict();
            }
        }
        Project renamed = new Project(project.id(), name, project.ownerId(),
                project.version() + 1, project.deleted());
        store.projects.put(renamed.id(), renamed);
        store.record(actor.id(), "update", "project", renamed.id());
        return renamed;
    }

    Project deleteProject(User actor, Project project) {
        Project deleted = new Project(project.id(), project.name(), project.ownerId(),
                project.version() + 1, true);
        store.projects.put(deleted.id(), deleted);
        store.record(actor.id(), "delete", "project", deleted.id());
        for (Task task : store.liveTasksOf(project.id())) {
            deleteTask(actor, task);
        }
        return deleted;
    }

    Project restoreProject(User actor, Project project) {
        if (!project.deleted()) throw conflict();
        Project restored = new Project(project.id(), project.name(), project.ownerId(),
                project.version() + 1, false);
        store.projects.put(restored.id(), restored);
        store.record(actor.id(), "restore", "project", restored.id());
        return restored;
    }

    // ------------------------------------------------------------------- tasks

    static String readNote(User actor, Map<String, Object> body,
            List<Map<String, String>> errors, String current) {
        if (!body.containsKey("internalNote")) return current;
        if (!actor.role().equals("admin")) throw forbidden();
        if (!(body.get("internalNote") instanceof String note)) throw badRequest();
        if (note.length() > MAX_TITLE_LENGTH) {
            errors.add(fail("internalNote", "internalNote is too long"));
        }
        return note;
    }

    Task createTask(User actor, Project project, String title, Integer priority,
            Integer assigneeId, String note, List<Map<String, String>> errors) {
        checkString(title, "title", MAX_TITLE_LENGTH, errors);
        checkPriority(priority, errors);
        if (assigneeId != null && store.findUser(assigneeId, false) == null) {
            errors.add(fail("assigneeId", "assigneeId is not a known user"));
        }
        if (!errors.isEmpty()) throw invalid(errors);
        Task task = store.insertTask(project.id(), title, priority, assigneeId, note);
        store.record(actor.id(), "create", "task", task.id());
        return task;
    }

    Task replaceTask(User actor, Task task, String title, Integer priority, Integer assigneeId,
            String note, List<Map<String, String>> errors) {
        checkString(title, "title", MAX_TITLE_LENGTH, errors);
        checkPriority(priority, errors);
        if (assigneeId != null && store.findUser(assigneeId, false) == null) {
            errors.add(fail("assigneeId", "assigneeId is not a known user"));
        }
        if (!errors.isEmpty()) throw invalid(errors);
        Task replaced = new Task(task.id(), task.projectId(), title, priority, task.status(),
                assigneeId, note, task.version() + 1, task.deleted());
        store.tasks.put(replaced.id(), replaced);
        store.record(actor.id(), "update", "task", replaced.id());
        return replaced;
    }

    Task moveStatus(User actor, Task task, Object status) {
        List<Map<String, String>> errors = new ArrayList<>();
        checkStatus(status, errors);
        if (!errors.isEmpty()) throw invalid(errors);
        String next = (String) status;
        if (!TRANSITIONS.contains(task.status() + ">" + next)) throw invalidTransition();
        Task moved = new Task(task.id(), task.projectId(), task.title(), task.priority(), next,
                task.assigneeId(), task.internalNote(), task.version() + 1, task.deleted());
        store.tasks.put(moved.id(), moved);
        store.record(actor.id(), "update", "task", moved.id());
        return moved;
    }

    Task deleteTask(User actor, Task task) {
        Task deleted = new Task(task.id(), task.projectId(), task.title(), task.priority(),
                task.status(), task.assigneeId(), task.internalNote(), task.version() + 1, true);
        store.tasks.put(deleted.id(), deleted);
        store.record(actor.id(), "delete", "task", deleted.id());
        return deleted;
    }

    Task restoreTask(User actor, Task task) {
        if (!task.deleted()) throw conflict();
        Task restored = new Task(task.id(), task.projectId(), task.title(), task.priority(),
                task.status(), task.assigneeId(), task.internalNote(), task.version() + 1, false);
        store.tasks.put(restored.id(), restored);
        store.record(actor.id(), "restore", "task", restored.id());
        return restored;
    }

    // ---------------------------------------------------------------- comments

    Comment createComment(User actor, Task task, String body) {
        List<Map<String, String>> errors = new ArrayList<>();
        checkString(body, "body", MAX_COMMENT_LENGTH, errors);
        if (!errors.isEmpty()) throw invalid(errors);
        Comment comment = store.insertComment(task.id(), actor.id(), body);
        store.record(actor.id(), "create", "comment", comment.id());
        return comment;
    }

    void removeComment(User actor, Comment comment) {
        if (!actor.role().equals("admin") && comment.authorId() != actor.id()) throw forbidden();
        store.comments.remove(comment.id());
        store.record(actor.id(), "delete", "comment", comment.id());
    }

    // ------------------------------------------------------------------- users

    User createUser(User actor, String username, String password, Object role, Object quota) {
        List<Map<String, String>> errors = new ArrayList<>();
        checkString(username, "username", MAX_NAME_LENGTH, errors);
        checkString(password, "password", MAX_NAME_LENGTH, errors);
        checkRole(role, errors);
        checkQuota(quota, errors);
        if (!errors.isEmpty()) throw invalid(errors);
        if (store.findByUsername(username) != null) throw conflict();
        User user = store.insertUser(username, password, (String) role, (Integer) quota);
        store.record(actor.id(), "create", "user", user.id());
        return user;
    }

    User updateUser(User actor, User user, Map<String, Object> body) {
        List<Map<String, String>> errors = new ArrayList<>();
        if (body.containsKey("role")) checkRole(body.get("role"), errors);
        if (body.containsKey("quota")) checkQuota(body.get("quota"), errors);
        if (!errors.isEmpty()) throw invalid(errors);
        String role = body.containsKey("role") ? (String) body.get("role") : user.role();
        int quota = body.containsKey("quota") ? (Integer) body.get("quota") : user.quota();
        User updated = new User(user.id(), user.username(), user.password(), role, quota,
                user.version() + 1, user.deleted());
        store.users.put(updated.id(), updated);
        store.record(actor.id(), "update", "user", updated.id());
        return updated;
    }

    User deleteUser(User actor, User user) {
        if (user.id() == actor.id()) throw conflict();
        User deleted = new User(user.id(), user.username(), user.password(), user.role(),
                user.quota(), user.version() + 1, true);
        store.users.put(deleted.id(), deleted);
        store.record(actor.id(), "delete", "user", deleted.id());
        return deleted;
    }

    // -------------------------------------------------------- queries, reports

    List<Project> visibleProjects(User user, boolean includeDeleted) {
        List<Project> rows = new ArrayList<>();
        for (Project project : store.projects.values()) {
            if (project.deleted() && !includeDeleted) continue;
            if (user.role().equals("admin") || project.ownerId() == user.id()) rows.add(project);
        }
        return rows;
    }

    List<Task> visibleTasks(User user, boolean includeDeleted) {
        TreeSet<Integer> allowed = new TreeSet<>();
        for (Project project : visibleProjects(user, true)) allowed.add(project.id());
        List<Task> rows = new ArrayList<>();
        for (Task task : store.tasks.values()) {
            if (!allowed.contains(task.projectId())) continue;
            if (task.deleted() && !includeDeleted) continue;
            rows.add(task);
        }
        return rows;
    }

    Map<String, Object> search(User user, String query) {
        String needle = query.toLowerCase();
        List<Map<String, Object>> results = new ArrayList<>();
        for (Project project : visibleProjects(user, false)) {
            if (project.name().toLowerCase().contains(needle)) {
                results.add(hit("project", project.id(), project.name()));
            }
        }
        for (Task task : visibleTasks(user, false)) {
            if (task.title().toLowerCase().contains(needle)) {
                results.add(hit("task", task.id(), task.title()));
            }
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("results", results);
        body.put("total", results.size());
        return body;
    }

    private static Map<String, Object> hit(String type, int id, String label) {
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("type", type);
        row.put("id", id);
        row.put("label", label);
        return row;
    }

    Map<String, Object> workload(User user, String groupBy) {
        List<Task> rows = visibleTasks(user, false);
        List<Map<String, Object>> groups = new ArrayList<>();
        if (groupBy.equals("status")) {
            for (String status : STATUSES) {
                List<Task> picked = new ArrayList<>();
                for (Task task : rows) {
                    if (task.status().equals(status)) picked.add(task);
                }
                groups.add(group(status, picked));
            }
        } else if (groupBy.equals("assignee")) {
            TreeSet<Integer> named = new TreeSet<>();
            for (Task task : rows) {
                if (task.assigneeId() != null) named.add(task.assigneeId());
            }
            for (Integer assignee : named) {
                List<Task> picked = new ArrayList<>();
                for (Task task : rows) {
                    if (assignee.equals(task.assigneeId())) picked.add(task);
                }
                groups.add(group(String.valueOf(assignee), picked));
            }
            List<Task> loose = new ArrayList<>();
            for (Task task : rows) {
                if (task.assigneeId() == null) loose.add(task);
            }
            if (!loose.isEmpty()) groups.add(group("unassigned", loose));
        } else {
            for (Project project : visibleProjects(user, false)) {
                List<Task> picked = new ArrayList<>();
                for (Task task : rows) {
                    if (task.projectId() == project.id()) picked.add(task);
                }
                groups.add(group(project.name(), picked));
            }
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("groupBy", groupBy);
        body.put("groups", groups);
        return body;
    }

    private static Map<String, Object> group(String key, List<Task> picked) {
        int totalScore = 0;
        for (Task task : picked) totalScore += computeScore(task.priority(), task.status());
        LinkedHashMap<String, Object> row = new LinkedHashMap<>();
        row.put("key", key);
        row.put("tasks", picked.size());
        row.put("totalScore", totalScore);
        return row;
    }

    int flushOutbox() {
        int flushed = 0;
        for (int index = 0; index < store.outbox.size(); index += 1) {
            OutboxEvent event = store.outbox.get(index);
            if (event.delivered()) continue;
            store.outbox.set(index, new OutboxEvent(event.seq(), event.name(),
                    event.resourceId(), true));
            flushed += 1;
        }
        return flushed;
    }

    Map<String, Object> metrics() {
        LinkedHashMap<String, Object> byStatus = new LinkedHashMap<>();
        for (Map.Entry<Integer, Integer> entry : store.byStatus.entrySet()) {
            byStatus.put(String.valueOf(entry.getKey()), entry.getValue());
        }
        List<Map<String, Object>> byRoute = new ArrayList<>();
        for (Map.Entry<String, Integer> entry : store.byRoute.entrySet()) {
            LinkedHashMap<String, Object> row = new LinkedHashMap<>();
            row.put("route", entry.getKey());
            row.put("count", entry.getValue());
            byRoute.add(row);
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("requests", store.requests);
        body.put("byStatus", byStatus);
        body.put("byRoute", byRoute);
        body.put("auditEntries", store.audit.size());
        body.put("outboxPending", store.outboxPending());
        return body;
    }

    Map<String, Object> stats() {
        LinkedHashMap<String, Object> byStatus = new LinkedHashMap<>();
        for (String status : STATUSES) byStatus.put(status, 0);
        int total = 0;
        int sumScore = 0;
        for (Task task : store.tasks.values()) {
            if (task.deleted()) continue;
            byStatus.put(task.status(), (Integer) byStatus.get(task.status()) + 1);
            sumScore += computeScore(task.priority(), task.status());
            total += 1;
        }
        int projectCount = 0;
        Project best = null;
        for (Project project : store.projects.values()) {
            if (project.deleted()) continue;
            projectCount += 1;
            if (best == null || store.taskCount(project.id()) > store.taskCount(best.id())) {
                best = project;
            }
        }
        int userCount = 0;
        for (User user : store.users.values()) {
            if (!user.deleted()) userCount += 1;
        }
        LinkedHashMap<String, Object> body = new LinkedHashMap<>();
        body.put("projects", projectCount);
        body.put("tasks", total);
        body.put("users", userCount);
        body.put("sessions", store.sessions.size());
        body.put("comments", store.comments.size());
        body.put("byStatus", byStatus);
        body.put("avgScore", total == 0 ? 0.0
                : Math.round((double) sumScore / total * 100) / 100.0);
        body.put("topProjectName", best == null ? null : best.name());
        body.put("auditEntries", store.audit.size());
        body.put("outboxPending", store.outboxPending());
        return body;
    }

    static void checkBulkSize(Object operations) {
        if (!(operations instanceof List<?> items) || items.isEmpty()
                || items.size() > MAX_BULK_ITEMS) {
            List<Map<String, String>> errors = new ArrayList<>();
            errors.add(fail("operations", "operations is out of range"));
            throw invalid(errors);
        }
    }
}
