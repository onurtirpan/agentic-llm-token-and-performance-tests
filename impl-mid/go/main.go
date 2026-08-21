// Task Service, mid tier — net/http implementation.
package main

import (
	"cmp"
	"crypto/rand"
	"encoding/json"
	"io"
	"math"
	"net/http"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const (
	maxTitleLength = 80
	maxNameLength  = 60
	minPriority    = 1
	maxPriority    = 5
	defaultLimit   = 20
	maxLimit       = 100
	port           = "127.0.0.1:8080"
)

var (
	statusBonus = map[string]int{"todo": 0, "in_progress": 3, "done": 5, "archived": 0}
	transitions = [][2]string{
		{"todo", "in_progress"}, {"todo", "archived"}, {"in_progress", "todo"},
		{"in_progress", "done"}, {"done", "archived"},
	}
	projectSorts = []string{"id", "name", "taskCount"}
	taskSorts    = []string{"id", "title", "priority", "score", "status"}
)

type User struct {
	ID       int
	Username string
	Password string
	Role     string
}

type Project struct {
	ID      int
	Name    string
	OwnerID int
}

type Task struct {
	ID         int
	ProjectID  int
	Title      string
	Priority   int
	Status     string
	AssigneeID *int
	Score      int
}

type AppError struct {
	Status  int
	Code    string
	Message string
	Details []detail
}

func (e *AppError) Error() string { return e.Message }

type detail struct {
	Field   string `json:"field"`
	Message string `json:"message"`
}

type errorBody struct {
	Code      string   `json:"code"`
	Message   string   `json:"message"`
	RequestID string   `json:"requestId"`
	Details   []detail `json:"details"`
}

type errorResponse struct {
	Error errorBody `json:"error"`
}

type healthResponse struct {
	Status   string `json:"status"`
	Projects int    `json:"projects"`
	Tasks    int    `json:"tasks"`
}

type loginResponse struct {
	Token  string `json:"token"`
	UserID int    `json:"userId"`
	Role   string `json:"role"`
}

type meResponse struct {
	UserID   int    `json:"userId"`
	Username string `json:"username"`
	Role     string `json:"role"`
}

type projectResponse struct {
	ID        int    `json:"id"`
	Name      string `json:"name"`
	OwnerID   int    `json:"ownerId"`
	TaskCount int    `json:"taskCount"`
}

type taskResponse struct {
	ID         int    `json:"id"`
	ProjectID  int    `json:"projectId"`
	Title      string `json:"title"`
	Priority   int    `json:"priority"`
	Status     string `json:"status"`
	AssigneeID *int   `json:"assigneeId"`
	Score      int    `json:"score"`
}

type pageResponse[T any] struct {
	Items  []T `json:"items"`
	Total  int `json:"total"`
	Limit  int `json:"limit"`
	Offset int `json:"offset"`
}

type statsResponse struct {
	Projects       int            `json:"projects"`
	Tasks          int            `json:"tasks"`
	Users          int            `json:"users"`
	Sessions       int            `json:"sessions"`
	ByStatus       map[string]int `json:"byStatus"`
	AvgScore       float64        `json:"avgScore"`
	TopProjectName *string        `json:"topProjectName"`
}

type logLine struct {
	Level      string `json:"level"`
	RequestID  string `json:"requestId"`
	Method     string `json:"method"`
	Path       string `json:"path"`
	Status     int    `json:"status"`
	DurationMs int64  `json:"durationMs"`
	UserID     *int   `json:"userId"`
}

type observer struct {
	http.ResponseWriter
	requestID string
	status    int
	userID    *int
}

func (o *observer) WriteHeader(status int) {
	o.status = status
	o.ResponseWriter.WriteHeader(status)
}

var (
	users = map[int]*User{
		1: {ID: 1, Username: "admin", Password: "admin-secret", Role: "admin"},
		2: {ID: 2, Username: "alice", Password: "alice-secret", Role: "user"},
		3: {ID: 3, Username: "bob", Password: "bob-secret", Role: "user"},
	}
	sessions      = map[string]int{}
	projects      = map[int]*Project{}
	tasks         = map[int]*Task{}
	nextProjectID = 1
	nextTaskID    = 1
)

func computeScore(priority int, status string) int {
	baseScore := priority * 10
	return baseScore + statusBonus[status]
}

func taskCount(projectID int) int {
	count := 0
	for _, task := range tasks {
		if task.ProjectID == projectID {
			count += 1
		}
	}
	return count
}

func serializeProject(project *Project) projectResponse {
	return projectResponse{ID: project.ID, Name: project.Name, OwnerID: project.OwnerID,
		TaskCount: taskCount(project.ID)}
}

func serializeTask(task *Task) taskResponse {
	return taskResponse{ID: task.ID, ProjectID: task.ProjectID, Title: task.Title,
		Priority: task.Priority, Status: task.Status, AssigneeID: task.AssigneeID,
		Score: task.Score}
}

func badRequest() *AppError {
	return &AppError{Status: 400, Code: "bad_request", Message: "the request is malformed"}
}

func notFound() *AppError {
	return &AppError{Status: 404, Code: "not_found", Message: "the resource does not exist"}
}

func forbidden() *AppError {
	return &AppError{Status: 403, Code: "forbidden", Message: "you may not access this resource"}
}

func conflict() *AppError {
	return &AppError{Status: 409, Code: "conflict", Message: "the resource already exists"}
}

func invalid(details []detail) *AppError {
	slices.SortFunc(details, func(a, b detail) int {
		return cmp.Or(strings.Compare(a.Field, b.Field), strings.Compare(a.Message, b.Message))
	})
	return &AppError{Status: 422, Code: "validation_failed",
		Message: "the request body is not valid", Details: details}
}

func fail(field, message string) detail {
	return detail{Field: field, Message: message}
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func sortedKeys[T any](items map[int]T) []int {
	keys := make([]int, 0, len(items))
	for key := range items {
		keys = append(keys, key)
	}
	slices.Sort(keys)
	return keys
}

func readBody(r *http.Request) (map[string]json.RawMessage, *AppError) {
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, badRequest()
	}
	body := map[string]json.RawMessage{}
	if strings.TrimSpace(string(raw)) == "" {
		return body, nil
	}
	if json.Unmarshal(raw, &body) != nil {
		return nil, badRequest()
	}
	return body, nil
}

func readInt(body map[string]json.RawMessage, field string, def *int) (*int, *AppError) {
	raw, present := body[field]
	if !present {
		return def, nil
	}
	text := strings.TrimSpace(string(raw))
	if text == "null" {
		return nil, nil
	}
	value, err := strconv.Atoi(text)
	if err != nil {
		return nil, badRequest()
	}
	return &value, nil
}

func readString(body map[string]json.RawMessage, field string, errors *[]detail,
	maxLength int, required bool) (string, *AppError) {
	raw, present := body[field]
	if !present {
		if required {
			*errors = append(*errors, fail(field, field+" is required"))
		}
		return "", nil
	}
	var value *string
	if json.Unmarshal(raw, &value) != nil || value == nil {
		return "", badRequest()
	}
	if *value == "" {
		if required {
			*errors = append(*errors, fail(field, field+" is required"))
		}
	} else if utf8.RuneCountInString(*value) > maxLength {
		*errors = append(*errors, fail(field, field+" is too long"))
	}
	return *value, nil
}

func readPriority(body map[string]json.RawMessage, errors *[]detail) (int, *AppError) {
	zero := 0
	value, err := readInt(body, "priority", &zero)
	if err != nil {
		return 0, err
	}
	if value == nil || *value < minPriority || *value > maxPriority {
		*errors = append(*errors, fail("priority", "priority is out of range"))
	}
	if value == nil {
		return 0, nil
	}
	return *value, nil
}

func readUserRef(body map[string]json.RawMessage, field string, errors *[]detail,
	def *int) (*int, *AppError) {
	value, err := readInt(body, field, def)
	if err != nil {
		return nil, err
	}
	if value != nil && users[*value] == nil {
		*errors = append(*errors, fail(field, field+" is not a known user"))
	}
	return value, nil
}

func parseID(raw string) (int, *AppError) {
	id, err := strconv.Atoi(raw)
	if err != nil {
		return 0, badRequest()
	}
	return id, nil
}

func readPage(r *http.Request, allowed []string) (int, int, string, string, *AppError) {
	query := r.URL.Query()
	errors := []detail{}
	limit, offset := defaultLimit, 0
	sort, order := "id", "asc"
	if query.Has("sort") {
		sort = query.Get("sort")
	}
	if query.Has("order") {
		order = query.Get("order")
	}
	if query.Has("limit") {
		value, err := strconv.Atoi(query.Get("limit"))
		if err != nil {
			value = -1
		}
		limit = value
		if limit < 1 || limit > maxLimit {
			errors = append(errors, fail("limit", "limit is out of range"))
		}
	}
	if query.Has("offset") {
		value, err := strconv.Atoi(query.Get("offset"))
		if err != nil {
			value = -1
		}
		offset = value
		if offset < 0 {
			errors = append(errors, fail("offset", "offset is out of range"))
		}
	}
	if !slices.Contains(allowed, sort) {
		errors = append(errors, fail("sort", "sort is not a valid field"))
	}
	if order != "asc" && order != "desc" {
		errors = append(errors, fail("order", "order must be asc or desc"))
	}
	if len(errors) > 0 {
		return 0, 0, "", "", invalid(errors)
	}
	return limit, offset, sort, order, nil
}

func paginate[T any](rows []T, limit, offset int, sort, order string,
	compare func(a, b T, field string) int) pageResponse[T] {
	slices.SortStableFunc(rows, func(a, b T) int { return compare(a, b, "id") })
	slices.SortStableFunc(rows, func(a, b T) int {
		if order == "desc" {
			return compare(b, a, sort)
		}
		return compare(a, b, sort)
	})
	items := rows[min(offset, len(rows)):]
	if len(items) > limit {
		items = items[:limit]
	}
	return pageResponse[T]{Items: items, Total: len(rows), Limit: limit, Offset: offset}
}

func compareProject(a, b projectResponse, field string) int {
	switch field {
	case "name":
		return strings.Compare(a.Name, b.Name)
	case "taskCount":
		return a.TaskCount - b.TaskCount
	}
	return a.ID - b.ID
}

func compareTask(a, b taskResponse, field string) int {
	switch field {
	case "title":
		return strings.Compare(a.Title, b.Title)
	case "priority":
		return a.Priority - b.Priority
	case "score":
		return a.Score - b.Score
	case "status":
		return strings.Compare(a.Status, b.Status)
	}
	return a.ID - b.ID
}

func authenticate(w *observer, r *http.Request) (*User, *AppError) {
	header := r.Header.Get("Authorization")
	if strings.HasPrefix(header, "Bearer ") {
		if userID, ok := sessions[header[7:]]; ok {
			w.userID = &userID
			return users[userID], nil
		}
	}
	return nil, &AppError{Status: 401, Code: "unauthorized",
		Message: "authentication is required"}
}

func requireAdmin(user *User) *AppError {
	if user.Role != "admin" {
		return forbidden()
	}
	return nil
}

func reachableProject(projectID int, user *User) (*Project, *AppError) {
	project, ok := projects[projectID]
	if !ok {
		return nil, notFound()
	}
	if user.Role != "admin" && project.OwnerID != user.ID {
		return nil, forbidden()
	}
	return project, nil
}

func reachableTask(taskID int, user *User) (*Task, *AppError) {
	task, ok := tasks[taskID]
	if !ok {
		return nil, notFound()
	}
	if _, err := reachableProject(task.ProjectID, user); err != nil {
		return nil, err
	}
	return task, nil
}

func observe(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestID := r.Header.Get("X-Request-Id")
		if requestID == "" {
			requestID = rand.Text()
		}
		w.Header().Set("X-Request-Id", requestID)
		observed := &observer{ResponseWriter: w, requestID: requestID, status: http.StatusOK}
		started := time.Now()
		next.ServeHTTP(observed, r)
		level := "info"
		if observed.status >= 500 {
			level = "error"
		} else if observed.status >= 400 {
			level = "warn"
		}
		_ = json.NewEncoder(os.Stdout).Encode(logLine{
			Level:      level,
			RequestID:  requestID,
			Method:     r.Method,
			Path:       r.URL.Path,
			Status:     observed.status,
			DurationMs: time.Since(started).Milliseconds(),
			UserID:     observed.userID,
		})
	})
}

func route(handle func(*observer, *http.Request) *AppError) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		observed := w.(*observer)
		err := handle(observed, r)
		if err == nil {
			return
		}
		details := err.Details
		if details == nil {
			details = []detail{}
		}
		writeJSON(observed, err.Status, errorResponse{Error: errorBody{
			Code: err.Code, Message: err.Message, RequestID: observed.requestID,
			Details: details,
		}})
	}
}

func handleHealth(w *observer, r *http.Request) *AppError {
	writeJSON(w, http.StatusOK,
		healthResponse{Status: "ok", Projects: len(projects), Tasks: len(tasks)})
	return nil
}

func handleLogin(w *observer, r *http.Request) *AppError {
	body, err := readBody(r)
	if err != nil {
		return err
	}
	errors := []detail{}
	username, err := readString(body, "username", &errors, maxNameLength, true)
	if err != nil {
		return err
	}
	password, err := readString(body, "password", &errors, maxNameLength, true)
	if err != nil {
		return err
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	var found *User
	for _, id := range sortedKeys(users) {
		if users[id].Username == username && users[id].Password == password {
			found = users[id]
			break
		}
	}
	if found == nil {
		return &AppError{Status: 401, Code: "invalid_credentials",
			Message: "the username or password is wrong"}
	}
	token := rand.Text()
	sessions[token] = found.ID
	writeJSON(w, http.StatusOK,
		loginResponse{Token: token, UserID: found.ID, Role: found.Role})
	return nil
}

func handleLogout(w *observer, r *http.Request) *AppError {
	if _, err := authenticate(w, r); err != nil {
		return err
	}
	delete(sessions, r.Header.Get("Authorization")[7:])
	w.WriteHeader(http.StatusNoContent)
	return nil
}

func handleMe(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	writeJSON(w, http.StatusOK,
		meResponse{UserID: user.ID, Username: user.Username, Role: user.Role})
	return nil
}

func handleListProjects(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, projectSorts)
	if err != nil {
		return err
	}
	rows := []projectResponse{}
	for _, id := range sortedKeys(projects) {
		if project := projects[id]; user.Role == "admin" || project.OwnerID == user.ID {
			rows = append(rows, serializeProject(project))
		}
	}
	writeJSON(w, http.StatusOK, paginate(rows, limit, offset, sort, order, compareProject))
	return nil
}

func handleCreateProject(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	if err := requireAdmin(user); err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	errors := []detail{}
	name, err := readString(body, "name", &errors, maxNameLength, true)
	if err != nil {
		return err
	}
	ownerID, err := readUserRef(body, "ownerId", &errors, &user.ID)
	if err != nil {
		return err
	}
	if ownerID == nil {
		errors = append(errors, fail("ownerId", "ownerId is not a known user"))
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	for _, id := range sortedKeys(projects) {
		if projects[id].OwnerID == *ownerID && projects[id].Name == name {
			return conflict()
		}
	}
	project := &Project{ID: nextProjectID, Name: name, OwnerID: *ownerID}
	projects[nextProjectID] = project
	nextProjectID += 1
	writeJSON(w, http.StatusCreated, serializeProject(project))
	return nil
}

func handleGetProject(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user)
	if err != nil {
		return err
	}
	writeJSON(w, http.StatusOK, serializeProject(project))
	return nil
}

func handleUpdateProject(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	if err := requireAdmin(user); err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	if _, present := body["name"]; !present {
		writeJSON(w, http.StatusOK, serializeProject(project))
		return nil
	}
	errors := []detail{}
	name, err := readString(body, "name", &errors, maxNameLength, true)
	if err != nil {
		return err
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	for _, other := range sortedKeys(projects) {
		sibling := projects[other]
		if sibling.OwnerID == project.OwnerID && sibling.Name == name && sibling.ID != project.ID {
			return conflict()
		}
	}
	project.Name = name
	writeJSON(w, http.StatusOK, serializeProject(project))
	return nil
}

func handleDeleteProject(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	if err := requireAdmin(user); err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user)
	if err != nil {
		return err
	}
	for _, taskID := range sortedKeys(tasks) {
		if tasks[taskID].ProjectID == project.ID {
			delete(tasks, taskID)
		}
	}
	delete(projects, project.ID)
	w.WriteHeader(http.StatusNoContent)
	return nil
}

func handleListTasks(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, taskSorts)
	if err != nil {
		return err
	}
	rows := []taskResponse{}
	for _, taskID := range sortedKeys(tasks) {
		if task := tasks[taskID]; task.ProjectID == project.ID {
			rows = append(rows, serializeTask(task))
		}
	}
	writeJSON(w, http.StatusOK, paginate(rows, limit, offset, sort, order, compareTask))
	return nil
}

func handleCreateTask(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	errors := []detail{}
	title, err := readString(body, "title", &errors, maxTitleLength, true)
	if err != nil {
		return err
	}
	priority, err := readPriority(body, &errors)
	if err != nil {
		return err
	}
	assigneeID, err := readUserRef(body, "assigneeId", &errors, nil)
	if err != nil {
		return err
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	task := &Task{ID: nextTaskID, ProjectID: project.ID, Title: title, Priority: priority,
		Status: "todo", AssigneeID: assigneeID, Score: computeScore(priority, "todo")}
	tasks[nextTaskID] = task
	nextTaskID += 1
	writeJSON(w, http.StatusCreated, serializeTask(task))
	return nil
}

func handleGetTask(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user)
	if err != nil {
		return err
	}
	writeJSON(w, http.StatusOK, serializeTask(task))
	return nil
}

func handleReplaceTask(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	errors := []detail{}
	title, err := readString(body, "title", &errors, maxTitleLength, true)
	if err != nil {
		return err
	}
	priority, err := readPriority(body, &errors)
	if err != nil {
		return err
	}
	assigneeID, err := readUserRef(body, "assigneeId", &errors, nil)
	if err != nil {
		return err
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	task.Title = title
	task.Priority = priority
	task.AssigneeID = assigneeID
	task.Score = computeScore(priority, task.Status)
	writeJSON(w, http.StatusOK, serializeTask(task))
	return nil
}

func handleUpdateStatus(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	var status *string
	known := false
	if raw, present := body["status"]; present && json.Unmarshal(raw, &status) == nil {
		if status != nil {
			_, known = statusBonus[*status]
		}
	}
	if !known {
		return invalid([]detail{fail("status", "status is not valid")})
	}
	if !slices.Contains(transitions, [2]string{task.Status, *status}) {
		return &AppError{Status: 409, Code: "invalid_transition",
			Message: "the status change is not allowed"}
	}
	task.Status = *status
	task.Score = computeScore(task.Priority, *status)
	writeJSON(w, http.StatusOK, serializeTask(task))
	return nil
}

func handleDeleteTask(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user)
	if err != nil {
		return err
	}
	delete(tasks, task.ID)
	w.WriteHeader(http.StatusNoContent)
	return nil
}

func handleStats(w *observer, r *http.Request) *AppError {
	user, err := authenticate(w, r)
	if err != nil {
		return err
	}
	if err := requireAdmin(user); err != nil {
		return err
	}
	byStatus := map[string]int{"todo": 0, "in_progress": 0, "done": 0, "archived": 0}
	sumScore := 0
	for _, id := range sortedKeys(tasks) {
		byStatus[tasks[id].Status] += 1
		sumScore += tasks[id].Score
	}
	total := len(tasks)
	avgScore := 0.0
	if total > 0 {
		avgScore = math.Round(float64(sumScore)/float64(total)*100) / 100
	}
	var best *Project
	for _, id := range sortedKeys(projects) {
		if project := projects[id]; best == nil || taskCount(project.ID) > taskCount(best.ID) {
			best = project
		}
	}
	var topProjectName *string
	if best != nil {
		topProjectName = &best.Name
	}
	writeJSON(w, http.StatusOK, statsResponse{Projects: len(projects), Tasks: total,
		Users: len(users), Sessions: len(sessions), ByStatus: byStatus, AvgScore: avgScore,
		TopProjectName: topProjectName})
	return nil
}

func handleNotFound(w *observer, r *http.Request) *AppError {
	return notFound()
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", route(handleHealth))
	mux.HandleFunc("POST /auth/login", route(handleLogin))
	mux.HandleFunc("POST /auth/logout", route(handleLogout))
	mux.HandleFunc("GET /me", route(handleMe))
	mux.HandleFunc("GET /projects", route(handleListProjects))
	mux.HandleFunc("POST /projects", route(handleCreateProject))
	mux.HandleFunc("GET /projects/{id}", route(handleGetProject))
	mux.HandleFunc("PATCH /projects/{id}", route(handleUpdateProject))
	mux.HandleFunc("DELETE /projects/{id}", route(handleDeleteProject))
	mux.HandleFunc("GET /projects/{id}/tasks", route(handleListTasks))
	mux.HandleFunc("POST /projects/{id}/tasks", route(handleCreateTask))
	mux.HandleFunc("GET /tasks/{id}", route(handleGetTask))
	mux.HandleFunc("PUT /tasks/{id}", route(handleReplaceTask))
	mux.HandleFunc("DELETE /tasks/{id}", route(handleDeleteTask))
	mux.HandleFunc("PATCH /tasks/{id}/status", route(handleUpdateStatus))
	mux.HandleFunc("GET /stats", route(handleStats))
	mux.HandleFunc("/", route(handleNotFound))
	_ = http.ListenAndServe(port, observe(mux))
}
