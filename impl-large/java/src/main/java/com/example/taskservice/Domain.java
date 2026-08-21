// Task Service, large tier — domain types, constants and pure rules.
package com.example.taskservice;

import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

final class Domain {

    static final int MAX_TITLE_LENGTH = 80;
    static final int MAX_NAME_LENGTH = 60;
    static final int MAX_COMMENT_LENGTH = 200;
    static final int MAX_BULK_ITEMS = 20;
    static final int MIN_PRIORITY = 1;
    static final int MAX_PRIORITY = 5;
    static final int DEFAULT_LIMIT = 20;
    static final int MAX_LIMIT = 100;
    static final int DEFAULT_QUOTA = 10000;
    static final int PROBE_QUOTA = 5;

    static final List<String> ROLES = List.of("admin", "user");
    static final List<String> STATUSES = List.of("todo", "in_progress", "done", "archived");
    static final Map<String, Integer> STATUS_BONUS =
            Map.of("todo", 0, "in_progress", 3, "done", 5, "archived", 0);
    static final Set<String> TRANSITIONS = Set.of("todo>in_progress", "todo>archived",
            "in_progress>todo", "in_progress>done", "done>archived");
    static final List<String> PROJECT_SORTS = List.of("id", "name", "taskCount");
    static final List<String> TASK_SORTS = List.of("id", "title", "priority", "score", "status");
    static final List<String> USER_SORTS = List.of("id", "username", "role");
    static final List<String> COMMENT_SORTS = List.of("id", "authorId");
    static final List<String> SEQ_SORTS = List.of("seq");
    static final List<String> GROUP_BYS = List.of("assignee", "status", "project");

    private Domain() {}

    record User(int id, String username, String password, String role, int quota, int version,
            boolean deleted) {}

    record Session(String token, int userId, int used) {}

    record Project(int id, String name, int ownerId, int version, boolean deleted) {}

    record Task(int id, int projectId, String title, int priority, String status,
            Integer assigneeId, String internalNote, int version, boolean deleted) {}

    record Comment(int id, int taskId, int authorId, String body) {}

    record AuditEntry(int seq, int actorId, String action, String resource, int resourceId) {}

    record OutboxEvent(int seq, String name, int resourceId, boolean delivered) {}

    record Caller(User user, Session session) {}

    record Page(int limit, int offset, String sort, String order) {}

    record Recorded(int status, Map<String, Object> body) {}

    /** Every failure path throws this. The api layer turns it into the envelope. */
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

    static AppError badRequest() {
        return new AppError(400, "bad_request", "the request is malformed");
    }

    static AppError unauthorized() {
        return new AppError(401, "unauthorized", "authentication is required");
    }

    static AppError invalidCredentials() {
        return new AppError(401, "invalid_credentials", "the username or password is wrong");
    }

    static AppError forbidden() {
        return new AppError(403, "forbidden", "you may not access this resource");
    }

    static AppError notFound() {
        return new AppError(404, "not_found", "the resource does not exist");
    }

    static AppError conflict() {
        return new AppError(409, "conflict", "the resource already exists");
    }

    static AppError invalidTransition() {
        return new AppError(409, "invalid_transition", "the status change is not allowed");
    }

    static AppError preconditionFailed() {
        return new AppError(412, "precondition_failed", "the resource has changed");
    }

    static AppError preconditionRequired() {
        return new AppError(428, "precondition_required", "the If-Match header is required");
    }

    static AppError quotaExceeded() {
        return new AppError(429, "quota_exceeded", "the request quota is exhausted");
    }

    static AppError invalid(List<Map<String, String>> details) {
        details.sort(Comparator.comparing((Map<String, String> entry) -> entry.get("field"))
                .thenComparing(entry -> entry.get("message")));
        return new AppError(422, "validation_failed", "the request body is not valid", details);
    }

    static Map<String, String> fail(String field, String message) {
        LinkedHashMap<String, String> entry = new LinkedHashMap<>();
        entry.put("field", field);
        entry.put("message", message);
        return entry;
    }

    static int computeScore(int priority, String status) {
        int baseScore = priority * 10;
        return baseScore + STATUS_BONUS.get(status);
    }

    static void checkString(String value, String fieldName, int maxLength,
            List<Map<String, String>> errors) {
        if (value.isEmpty()) {
            errors.add(fail(fieldName, fieldName + " is required"));
        } else if (value.length() > maxLength) {
            errors.add(fail(fieldName, fieldName + " is too long"));
        }
    }

    static void checkPriority(Integer value, List<Map<String, String>> errors) {
        if (value == null || value < MIN_PRIORITY || value > MAX_PRIORITY) {
            errors.add(fail("priority", "priority is out of range"));
        }
    }

    static void checkStatus(Object value, List<Map<String, String>> errors) {
        if (!(value instanceof String text) || !STATUSES.contains(text)) {
            errors.add(fail("status", "status is not valid"));
        }
    }

    static void checkRole(Object value, List<Map<String, String>> errors) {
        if (!(value instanceof String text) || !ROLES.contains(text)) {
            errors.add(fail("role", "role is not valid"));
        }
    }

    static void checkQuota(Object value, List<Map<String, String>> errors) {
        if (!(value instanceof Integer number) || number < 0) {
            errors.add(fail("quota", "quota is out of range"));
        }
    }
}
