// Task Service, large tier — the in-memory state and its repositories.
package com.example.taskservice;

import static com.example.taskservice.Domain.DEFAULT_QUOTA;
import static com.example.taskservice.Domain.PROBE_QUOTA;

import com.example.taskservice.Domain.AuditEntry;
import com.example.taskservice.Domain.Comment;
import com.example.taskservice.Domain.OutboxEvent;
import com.example.taskservice.Domain.Project;
import com.example.taskservice.Domain.Recorded;
import com.example.taskservice.Domain.Session;
import com.example.taskservice.Domain.Task;
import com.example.taskservice.Domain.User;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

final class Store {

    final Map<Integer, User> users = new TreeMap<>();
    final Map<String, Session> sessions = new LinkedHashMap<>();
    final Map<Integer, Project> projects = new TreeMap<>();
    final Map<Integer, Task> tasks = new TreeMap<>();
    final Map<Integer, Comment> comments = new TreeMap<>();
    final List<AuditEntry> audit = new ArrayList<>();
    final List<OutboxEvent> outbox = new ArrayList<>();
    final Map<String, Recorded> idempotency = new HashMap<>();
    final Map<Integer, Integer> byStatus = new TreeMap<>();
    final Map<String, Integer> byRoute = new TreeMap<>();

    int requests = 0;
    int nextProjectId = 1;
    int nextTaskId = 1;
    int nextCommentId = 1;
    int nextUserId = 5;
    int nextSeq = 1;

    void seed() {
        users.put(1, new User(1, "admin", "admin-secret", "admin", DEFAULT_QUOTA, 1, false));
        users.put(2, new User(2, "alice", "alice-secret", "user", DEFAULT_QUOTA, 1, false));
        users.put(3, new User(3, "bob", "bob-secret", "user", DEFAULT_QUOTA, 1, false));
        users.put(4, new User(4, "probe", "probe-secret", "user", PROBE_QUOTA, 1, false));
    }

    int takeSeq() {
        int value = nextSeq;
        nextSeq += 1;
        return value;
    }

    /** Append one audit entry and one outbox event for a successful write. */
    void record(int actorId, String action, String resource, int resourceId) {
        audit.add(new AuditEntry(takeSeq(), actorId, action, resource, resourceId));
        outbox.add(new OutboxEvent(takeSeq(), resource + "." + action, resourceId, false));
    }

    void countRequest(String route, int status) {
        requests += 1;
        byRoute.merge(route, 1, Integer::sum);
        byStatus.merge(status, 1, Integer::sum);
    }

    User findUser(Integer userId, boolean includeDeleted) {
        User user = userId == null ? null : users.get(userId);
        if (user == null || (user.deleted() && !includeDeleted)) return null;
        return user;
    }

    User findByUsername(String username) {
        for (User user : users.values()) {
            if (user.username().equals(username) && !user.deleted()) return user;
        }
        return null;
    }

    User insertUser(String username, String password, String role, int quota) {
        User user = new User(nextUserId, username, password, role, quota, 1, false);
        users.put(user.id(), user);
        nextUserId += 1;
        return user;
    }

    Project findProject(Integer projectId, boolean includeDeleted) {
        Project project = projectId == null ? null : projects.get(projectId);
        if (project == null || (project.deleted() && !includeDeleted)) return null;
        return project;
    }

    Project insertProject(String name, int ownerId) {
        Project project = new Project(nextProjectId, name, ownerId, 1, false);
        projects.put(project.id(), project);
        nextProjectId += 1;
        return project;
    }

    Task findTask(Integer taskId, boolean includeDeleted) {
        Task task = taskId == null ? null : tasks.get(taskId);
        if (task == null || (task.deleted() && !includeDeleted)) return null;
        return task;
    }

    Task insertTask(int projectId, String title, int priority, Integer assigneeId,
            String internalNote) {
        Task task = new Task(nextTaskId, projectId, title, priority, "todo", assigneeId,
                internalNote, 1, false);
        tasks.put(task.id(), task);
        nextTaskId += 1;
        return task;
    }

    Comment findComment(int commentId) {
        return comments.get(commentId);
    }

    Comment insertComment(int taskId, int authorId, String body) {
        Comment comment = new Comment(nextCommentId, taskId, authorId, body);
        comments.put(comment.id(), comment);
        nextCommentId += 1;
        return comment;
    }

    List<Task> liveTasksOf(int projectId) {
        List<Task> rows = new ArrayList<>();
        for (Task task : tasks.values()) {
            if (task.projectId() == projectId && !task.deleted()) rows.add(task);
        }
        return rows;
    }

    int taskCount(int projectId) {
        return liveTasksOf(projectId).size();
    }

    int outboxPending() {
        int pending = 0;
        for (OutboxEvent event : outbox) {
            if (!event.delivered()) pending += 1;
        }
        return pending;
    }
}
