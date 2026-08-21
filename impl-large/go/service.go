// Task Service, large tier — business rules, authorization and audit emission.
package main

import (
	"cmp"
	"encoding/json"
	"math"
	"net/url"
	"slices"
	"strconv"
	"strings"
	"unicode/utf8"
)

type pageResponse struct {
	Items  []row `json:"items"`
	Total  int   `json:"total"`
	Limit  int   `json:"limit"`
	Offset int   `json:"offset"`
}

type searchHit struct {
	Type  string `json:"type"`
	ID    int    `json:"id"`
	Label string `json:"label"`
}

type searchResponse struct {
	Results []searchHit `json:"results"`
	Total   int         `json:"total"`
}

type workloadGroup struct {
	Key        string `json:"key"`
	Tasks      int    `json:"tasks"`
	TotalScore int    `json:"totalScore"`
}

type workloadResponse struct {
	GroupBy string          `json:"groupBy"`
	Groups  []workloadGroup `json:"groups"`
}

type routeCount struct {
	Route string `json:"route"`
	Count int    `json:"count"`
}

type metricsResponse struct {
	Requests      int          `json:"requests"`
	ByStatus      ordered      `json:"byStatus"`
	ByRoute       []routeCount `json:"byRoute"`
	AuditEntries  int          `json:"auditEntries"`
	OutboxPending int          `json:"outboxPending"`
}

type statsResponse struct {
	Projects       int     `json:"projects"`
	Tasks          int     `json:"tasks"`
	Users          int     `json:"users"`
	Sessions       int     `json:"sessions"`
	Comments       int     `json:"comments"`
	ByStatus       ordered `json:"byStatus"`
	AvgScore       float64 `json:"avgScore"`
	TopProjectName *string `json:"topProjectName"`
	AuditEntries   int     `json:"auditEntries"`
	OutboxPending  int     `json:"outboxPending"`
}

// ------------------------------------------------------------------ serializers

func serializeUser(user *User) row {
	return row{"id": user.ID, "username": user.Username, "role": user.Role,
		"quota": user.Quota, "version": user.Version, "deleted": user.Deleted}
}

func serializeProject(project *Project) row {
	return row{"id": project.ID, "name": project.Name, "ownerId": project.OwnerID,
		"taskCount": taskCount(project.ID), "version": project.Version,
		"deleted": project.Deleted}
}

func serializeTask(task *Task, role string) row {
	body := row{"id": task.ID, "projectId": task.ProjectID, "title": task.Title,
		"priority": task.Priority, "status": task.Status, "assigneeId": task.AssigneeID}
	if role == "admin" {
		body["internalNote"] = task.InternalNote
	}
	body["version"] = task.Version
	body["deleted"] = task.Deleted
	body["score"] = computeScore(task.Priority, task.Status)
	return body
}

func serializeComment(comment *Comment) row {
	return row{"id": comment.ID, "taskId": comment.TaskID,
		"authorId": comment.AuthorID, "body": comment.Body}
}

func serializeAudit(entry *AuditEntry) row {
	return row{"seq": entry.Seq, "actorId": entry.ActorID, "action": entry.Action,
		"resource": entry.Resource, "resourceId": entry.ResourceID}
}

func serializeOutbox(event *OutboxEvent) row {
	return row{"seq": event.Seq, "name": event.Name, "resourceId": event.ResourceID,
		"delivered": event.Delivered}
}

// ----------------------------------------------------------------- access rules

func authenticate(header string) (*User, *Session, *AppError) {
	token := ""
	if after, found := strings.CutPrefix(header, "Bearer "); found {
		token = after
	}
	session, ok := sessions[token]
	if !ok {
		return nil, nil, unauthorized()
	}
	user := findUser(session.UserID, false)
	if user == nil {
		return nil, nil, unauthorized()
	}
	return user, session, nil
}

func chargeQuota(user *User, session *Session) (int, *AppError) {
	if session.Used >= user.Quota {
		return 0, quotaExceeded()
	}
	session.Used += 1
	return max(user.Quota-session.Used, 0), nil
}

func requireAdmin(user *User) *AppError {
	if user.Role != "admin" {
		return forbidden()
	}
	return nil
}

func reachableProject(projectID int, user *User, includeDeleted bool) (*Project, *AppError) {
	project := findProject(projectID, includeDeleted)
	if project == nil {
		return nil, notFound()
	}
	if user.Role != "admin" && project.OwnerID != user.ID {
		return nil, forbidden()
	}
	return project, nil
}

func reachableTask(taskID int, user *User, includeDeleted bool) (*Task, *AppError) {
	task := findTask(taskID, includeDeleted)
	if task == nil {
		return nil, notFound()
	}
	if _, err := reachableProject(task.ProjectID, user, true); err != nil {
		return nil, err
	}
	return task, nil
}

func checkIfMatch(header string, version int) *AppError {
	if header == "" {
		return preconditionRequired()
	}
	if header != strconv.Itoa(version) {
		return preconditionFailed()
	}
	return nil
}

// checkVersion is the bulk form of If-Match: the item states the version inline.
func checkVersion(stated *int, version int) *AppError {
	if stated == nil || *stated != version {
		return preconditionFailed()
	}
	return nil
}

func checkIncludeDeleted(query url.Values, user *User) (bool, *AppError) {
	if !query.Has("includeDeleted") {
		return false, nil
	}
	if user.Role != "admin" {
		return false, forbidden()
	}
	return query.Get("includeDeleted") == "true", nil
}

func checkBulkSize(count int, wellFormed bool) *AppError {
	if !wellFormed || count < 1 || count > maxBulkItems {
		return invalid([]detail{fail("operations", "operations is out of range")})
	}
	return nil
}

// ------------------------------------------------------------------- pagination

// paginate sorts by the tiebreak first, then stably by the requested field.
func paginate(rows []row, limit, offset int, sort, order string) pageResponse {
	tiebreak := "id"
	if len(rows) > 0 {
		if _, found := rows[0]["seq"]; found {
			tiebreak = "seq"
		}
	}
	slices.SortStableFunc(rows, func(a, b row) int {
		return compareAny(a[tiebreak], b[tiebreak])
	})
	slices.SortStableFunc(rows, func(a, b row) int {
		if order == "desc" {
			return compareAny(b[sort], a[sort])
		}
		return compareAny(a[sort], b[sort])
	})
	items := rows[min(offset, len(rows)):]
	if len(items) > limit {
		items = items[:limit]
	}
	return pageResponse{Items: items, Total: len(rows), Limit: limit, Offset: offset}
}

func compareAny(left, right any) int {
	switch first := left.(type) {
	case int:
		second, _ := right.(int)
		return cmp.Compare(first, second)
	case string:
		second, _ := right.(string)
		return strings.Compare(first, second)
	}
	return 0
}

// ------------------------------------------------------------------------- auth

func login(username, password, token string) (*User, *AppError) {
	user := findByUsername(username)
	if user == nil || user.Password != password {
		return nil, invalidCredentials()
	}
	sessions[token] = &Session{Token: token, UserID: user.ID}
	return user, nil
}

// --------------------------------------------------------------------- projects

func createProject(actor *User, name string, ownerID *int) (*Project, *AppError) {
	errors := []detail{}
	checkString(name, "name", maxNameLength, &errors)
	if ownerID == nil || findUser(*ownerID, false) == nil {
		errors = append(errors, fail("ownerId", "ownerId is not a known user"))
	}
	if len(errors) > 0 {
		return nil, invalid(errors)
	}
	for _, id := range sortedKeys(projects) {
		other := projects[id]
		if other.OwnerID == *ownerID && other.Name == name && !other.Deleted {
			return nil, conflict()
		}
	}
	project := insertProject(name, *ownerID)
	record(actor.ID, "create", "project", project.ID)
	return project, nil
}

func renameProject(actor *User, project *Project, name string) *AppError {
	errors := []detail{}
	checkString(name, "name", maxNameLength, &errors)
	if len(errors) > 0 {
		return invalid(errors)
	}
	for _, id := range sortedKeys(projects) {
		other := projects[id]
		if other.OwnerID == project.OwnerID && other.Name == name &&
			other.ID != project.ID && !other.Deleted {
			return conflict()
		}
	}
	project.Name = name
	project.Version += 1
	record(actor.ID, "update", "project", project.ID)
	return nil
}

func deleteProject(actor *User, project *Project) {
	project.Deleted = true
	project.Version += 1
	record(actor.ID, "delete", "project", project.ID)
	for _, task := range liveTasksOf(project.ID) {
		task.Deleted = true
		task.Version += 1
		record(actor.ID, "delete", "task", task.ID)
	}
}

func restoreProject(actor *User, project *Project) *AppError {
	if !project.Deleted {
		return conflict()
	}
	project.Deleted = false
	project.Version += 1
	record(actor.ID, "restore", "project", project.ID)
	return nil
}

// ------------------------------------------------------------------------ tasks

// readNote applies the admin-only rule that guards internalNote.
func readNote(actor *User, body map[string]json.RawMessage, current string,
	errors *[]detail) (string, *AppError) {
	raw, present := body["internalNote"]
	if !present {
		return current, nil
	}
	if actor.Role != "admin" {
		return "", forbidden()
	}
	var note *string
	if json.Unmarshal(raw, &note) != nil || note == nil {
		return "", badRequest()
	}
	if utf8.RuneCountInString(*note) > maxTitleLength {
		*errors = append(*errors, fail("internalNote", "internalNote is too long"))
	}
	return *note, nil
}

func createTask(actor *User, project *Project, title string, priority, assigneeID *int,
	note string, errors []detail) (*Task, *AppError) {
	checkString(title, "title", maxTitleLength, &errors)
	checkPriority(priority, &errors)
	if assigneeID != nil && findUser(*assigneeID, false) == nil {
		errors = append(errors, fail("assigneeId", "assigneeId is not a known user"))
	}
	if len(errors) > 0 {
		return nil, invalid(errors)
	}
	task := insertTask(project.ID, title, *priority, assigneeID, note)
	record(actor.ID, "create", "task", task.ID)
	return task, nil
}

func replaceTask(actor *User, task *Task, title string, priority, assigneeID *int,
	note string, errors []detail) *AppError {
	checkString(title, "title", maxTitleLength, &errors)
	checkPriority(priority, &errors)
	if assigneeID != nil && findUser(*assigneeID, false) == nil {
		errors = append(errors, fail("assigneeId", "assigneeId is not a known user"))
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	task.Title = title
	task.Priority = *priority
	task.AssigneeID = assigneeID
	task.InternalNote = note
	task.Version += 1
	record(actor.ID, "update", "task", task.ID)
	return nil
}

func moveStatus(actor *User, task *Task, status *string) *AppError {
	errors := []detail{}
	checkStatus(status, &errors)
	if len(errors) > 0 {
		return invalid(errors)
	}
	if !slices.Contains(transitions, [2]string{task.Status, *status}) {
		return invalidTransition()
	}
	task.Status = *status
	task.Version += 1
	record(actor.ID, "update", "task", task.ID)
	return nil
}

func deleteTask(actor *User, task *Task) {
	task.Deleted = true
	task.Version += 1
	record(actor.ID, "delete", "task", task.ID)
}

func restoreTask(actor *User, task *Task) *AppError {
	if !task.Deleted {
		return conflict()
	}
	task.Deleted = false
	task.Version += 1
	record(actor.ID, "restore", "task", task.ID)
	return nil
}

// --------------------------------------------------------------------- comments

func createComment(actor *User, task *Task, body string) (*Comment, *AppError) {
	errors := []detail{}
	checkString(body, "body", maxCommentLength, &errors)
	if len(errors) > 0 {
		return nil, invalid(errors)
	}
	comment := insertComment(task.ID, actor.ID, body)
	record(actor.ID, "create", "comment", comment.ID)
	return comment, nil
}

func removeComment(actor *User, comment *Comment) *AppError {
	if actor.Role != "admin" && comment.AuthorID != actor.ID {
		return forbidden()
	}
	delete(comments, comment.ID)
	record(actor.ID, "delete", "comment", comment.ID)
	return nil
}

// ------------------------------------------------------------------------ users

func createUser(actor *User, username, password string, role *string,
	quota *int) (*User, *AppError) {
	errors := []detail{}
	checkString(username, "username", maxNameLength, &errors)
	checkString(password, "password", maxNameLength, &errors)
	checkRole(role, &errors)
	checkQuota(quota, &errors)
	if len(errors) > 0 {
		return nil, invalid(errors)
	}
	if findByUsername(username) != nil {
		return nil, conflict()
	}
	user := insertUser(username, password, *role, *quota)
	record(actor.ID, "create", "user", user.ID)
	return user, nil
}

func updateUser(actor, user *User, role *string, quota *int) *AppError {
	errors := []detail{}
	checkRole(role, &errors)
	checkQuota(quota, &errors)
	if len(errors) > 0 {
		return invalid(errors)
	}
	user.Role = *role
	user.Quota = *quota
	user.Version += 1
	record(actor.ID, "update", "user", user.ID)
	return nil
}

func deleteUser(actor, user *User) *AppError {
	if user.ID == actor.ID {
		return conflict()
	}
	user.Deleted = true
	user.Version += 1
	record(actor.ID, "delete", "user", user.ID)
	return nil
}

// ---------------------------------------------------------- queries and reports

func visibleProjects(user *User, includeDeleted bool) []*Project {
	found := []*Project{}
	for _, id := range sortedKeys(projects) {
		project := projects[id]
		if (includeDeleted || !project.Deleted) &&
			(user.Role == "admin" || project.OwnerID == user.ID) {
			found = append(found, project)
		}
	}
	return found
}

func visibleTasks(user *User, includeDeleted bool) []*Task {
	allowed := map[int]bool{}
	for _, project := range visibleProjects(user, true) {
		allowed[project.ID] = true
	}
	found := []*Task{}
	for _, id := range sortedKeys(tasks) {
		task := tasks[id]
		if allowed[task.ProjectID] && (includeDeleted || !task.Deleted) {
			found = append(found, task)
		}
	}
	return found
}

func taskFilters(query url.Values, rows []*Task) ([]*Task, *AppError) {
	errors := []detail{}
	status := query.Get("status")
	assignee, err := strconv.Atoi(query.Get("assigneeId"))
	if query.Has("status") && !slices.Contains(statuses, status) {
		errors = append(errors, fail("status", "status is not valid"))
	}
	if query.Has("assigneeId") && err != nil {
		errors = append(errors, fail("assigneeId", "assigneeId is not a known user"))
	}
	if len(errors) > 0 {
		return nil, invalid(errors)
	}
	found := []*Task{}
	for _, task := range rows {
		if query.Has("status") && task.Status != status {
			continue
		}
		if query.Has("assigneeId") &&
			(task.AssigneeID == nil || *task.AssigneeID != assignee) {
			continue
		}
		found = append(found, task)
	}
	return found, nil
}

func search(user *User, query string) searchResponse {
	needle := strings.ToLower(query)
	results := []searchHit{}
	for _, project := range visibleProjects(user, false) {
		if strings.Contains(strings.ToLower(project.Name), needle) {
			results = append(results,
				searchHit{Type: "project", ID: project.ID, Label: project.Name})
		}
	}
	for _, task := range visibleTasks(user, false) {
		if strings.Contains(strings.ToLower(task.Title), needle) {
			results = append(results, searchHit{Type: "task", ID: task.ID, Label: task.Title})
		}
	}
	return searchResponse{Results: results, Total: len(results)}
}

func workload(user *User, groupBy string) workloadResponse {
	rows := visibleTasks(user, false)
	groups := []workloadGroup{}
	add := func(key string, picked []*Task) {
		totalScore := 0
		for _, task := range picked {
			totalScore += computeScore(task.Priority, task.Status)
		}
		groups = append(groups,
			workloadGroup{Key: key, Tasks: len(picked), TotalScore: totalScore})
	}
	pick := func(keep func(*Task) bool) []*Task {
		picked := []*Task{}
		for _, task := range rows {
			if keep(task) {
				picked = append(picked, task)
			}
		}
		return picked
	}
	switch groupBy {
	case "status":
		for _, status := range statuses {
			add(status, pick(func(task *Task) bool { return task.Status == status }))
		}
	case "assignee":
		named := []int{}
		for _, task := range rows {
			if task.AssigneeID != nil && !slices.Contains(named, *task.AssigneeID) {
				named = append(named, *task.AssigneeID)
			}
		}
		slices.Sort(named)
		for _, assignee := range named {
			add(strconv.Itoa(assignee), pick(func(task *Task) bool {
				return task.AssigneeID != nil && *task.AssigneeID == assignee
			}))
		}
		loose := pick(func(task *Task) bool { return task.AssigneeID == nil })
		if len(loose) > 0 {
			add("unassigned", loose)
		}
	default:
		for _, project := range visibleProjects(user, false) {
			add(project.Name, pick(func(task *Task) bool {
				return task.ProjectID == project.ID
			}))
		}
	}
	return workloadResponse{GroupBy: groupBy, Groups: groups}
}

func flushOutbox() int {
	flushed := 0
	for _, event := range outbox {
		if !event.Delivered {
			event.Delivered = true
			flushed += 1
		}
	}
	return flushed
}

func metrics() metricsResponse {
	codes := make([]int, 0, len(byStatus))
	for code := range byStatus {
		codes = append(codes, code)
	}
	slices.Sort(codes)
	statusCounts := ordered{}
	for _, code := range codes {
		statusCounts = append(statusCounts, member{strconv.Itoa(code), byStatus[code]})
	}
	matched := make([]string, 0, len(byRoute))
	for pattern := range byRoute {
		matched = append(matched, pattern)
	}
	slices.Sort(matched)
	routeCounts := []routeCount{}
	for _, pattern := range matched {
		routeCounts = append(routeCounts, routeCount{Route: pattern, Count: byRoute[pattern]})
	}
	return metricsResponse{Requests: requests, ByStatus: statusCounts, ByRoute: routeCounts,
		AuditEntries: len(audit), OutboxPending: outboxPending()}
}

func stats() statsResponse {
	live := liveTasks()
	counts := ordered{}
	for _, status := range statuses {
		total := 0
		for _, task := range live {
			if task.Status == status {
				total += 1
			}
		}
		counts = append(counts, member{status, total})
	}
	sumScore := 0
	for _, task := range live {
		sumScore += computeScore(task.Priority, task.Status)
	}
	avgScore := 0.0
	if len(live) > 0 {
		avgScore = math.Round(float64(sumScore)/float64(len(live))*100) / 100
	}
	var best *Project
	for _, id := range sortedKeys(projects) {
		project := projects[id]
		if project.Deleted {
			continue
		}
		if best == nil || taskCount(project.ID) > taskCount(best.ID) {
			best = project
		}
	}
	var topProjectName *string
	if best != nil {
		topProjectName = &best.Name
	}
	return statsResponse{
		Projects:       liveCount(projects, func(p *Project) bool { return !p.Deleted }),
		Tasks:          len(live),
		Users:          liveCount(users, func(u *User) bool { return !u.Deleted }),
		Sessions:       len(sessions),
		Comments:       len(comments),
		ByStatus:       counts,
		AvgScore:       avgScore,
		TopProjectName: topProjectName,
		AuditEntries:   len(audit),
		OutboxPending:  outboxPending(),
	}
}

// liveTasks lists every task that is not deleted, in ascending id order.
func liveTasks() []*Task {
	live := []*Task{}
	for _, id := range sortedKeys(tasks) {
		if !tasks[id].Deleted {
			live = append(live, tasks[id])
		}
	}
	return live
}
