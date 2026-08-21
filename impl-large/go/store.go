// Task Service, large tier — the in-memory state and its repositories.
package main

import "slices"

type idempotencyKey struct {
	token string
	key   string
}

type recorded struct {
	status int
	body   any
}

var (
	users       = map[int]*User{}
	sessions    = map[string]*Session{}
	projects    = map[int]*Project{}
	tasks       = map[int]*Task{}
	comments    = map[int]*Comment{}
	audit       = []*AuditEntry{}
	outbox      = []*OutboxEvent{}
	idempotency = map[idempotencyKey]recorded{}
	byStatus    = map[int]int{}
	byRoute     = map[string]int{}

	requests      = 0
	nextProjectID = 1
	nextTaskID    = 1
	nextCommentID = 1
	nextUserID    = 5
	nextSeq       = 1
)

func seed() {
	for _, user := range []*User{
		{ID: 1, Username: "admin", Password: "admin-secret", Role: "admin",
			Quota: defaultQuota, Version: 1},
		{ID: 2, Username: "alice", Password: "alice-secret", Role: "user",
			Quota: defaultQuota, Version: 1},
		{ID: 3, Username: "bob", Password: "bob-secret", Role: "user",
			Quota: defaultQuota, Version: 1},
		{ID: 4, Username: "probe", Password: "probe-secret", Role: "user",
			Quota: probeQuota, Version: 1},
	} {
		users[user.ID] = user
	}
}

// sortedKeys gives the ascending id order every iteration in this service uses.
func sortedKeys[T any](items map[int]T) []int {
	keys := make([]int, 0, len(items))
	for key := range items {
		keys = append(keys, key)
	}
	slices.Sort(keys)
	return keys
}

// liveCount counts the rows a predicate keeps, in ascending id order.
func liveCount[T any](items map[int]T, keep func(T) bool) int {
	total := 0
	for _, id := range sortedKeys(items) {
		if keep(items[id]) {
			total += 1
		}
	}
	return total
}

func takeSeq() int {
	seq := nextSeq
	nextSeq += 1
	return seq
}

// record appends one audit entry and one outbox event for a successful write.
func record(actorID int, action, resource string, resourceID int) {
	audit = append(audit, &AuditEntry{takeSeq(), actorID, action, resource, resourceID})
	outbox = append(outbox, &OutboxEvent{takeSeq(), resource + "." + action, resourceID, false})
}

func countRequest(matched string, status int) {
	requests += 1
	byRoute[matched] += 1
	byStatus[status] += 1
}

func findUser(userID int, includeDeleted bool) *User {
	user, ok := users[userID]
	if !ok || (user.Deleted && !includeDeleted) {
		return nil
	}
	return user
}

func findByUsername(username string) *User {
	for _, id := range sortedKeys(users) {
		if users[id].Username == username && !users[id].Deleted {
			return users[id]
		}
	}
	return nil
}

func insertUser(username, password, role string, quota int) *User {
	user := &User{ID: nextUserID, Username: username, Password: password, Role: role,
		Quota: quota, Version: 1}
	users[user.ID] = user
	nextUserID += 1
	return user
}

func findProject(projectID int, includeDeleted bool) *Project {
	project, ok := projects[projectID]
	if !ok || (project.Deleted && !includeDeleted) {
		return nil
	}
	return project
}

func insertProject(name string, ownerID int) *Project {
	project := &Project{ID: nextProjectID, Name: name, OwnerID: ownerID, Version: 1}
	projects[project.ID] = project
	nextProjectID += 1
	return project
}

func findTask(taskID int, includeDeleted bool) *Task {
	task, ok := tasks[taskID]
	if !ok || (task.Deleted && !includeDeleted) {
		return nil
	}
	return task
}

func insertTask(projectID int, title string, priority int, assigneeID *int,
	internalNote string) *Task {
	task := &Task{ID: nextTaskID, ProjectID: projectID, Title: title, Priority: priority,
		Status: "todo", AssigneeID: assigneeID, InternalNote: internalNote, Version: 1}
	tasks[task.ID] = task
	nextTaskID += 1
	return task
}

func findComment(commentID int) *Comment {
	return comments[commentID]
}

func insertComment(taskID, authorID int, body string) *Comment {
	comment := &Comment{ID: nextCommentID, TaskID: taskID, AuthorID: authorID, Body: body}
	comments[comment.ID] = comment
	nextCommentID += 1
	return comment
}

func liveTasksOf(projectID int) []*Task {
	found := []*Task{}
	for _, id := range sortedKeys(tasks) {
		if tasks[id].ProjectID == projectID && !tasks[id].Deleted {
			found = append(found, tasks[id])
		}
	}
	return found
}

func taskCount(projectID int) int {
	return len(liveTasksOf(projectID))
}

func outboxPending() int {
	pending := 0
	for _, event := range outbox {
		if !event.Delivered {
			pending += 1
		}
	}
	return pending
}
