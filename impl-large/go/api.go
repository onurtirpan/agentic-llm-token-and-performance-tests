// Task Service, large tier — HTTP routing, middleware and the entry point.
package main

import (
	"crypto/rand"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"
)

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
	Comments int    `json:"comments"`
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

type bulkResult struct {
	Index  int     `json:"index"`
	Status int     `json:"status"`
	ID     *int    `json:"id"`
	Error  *string `json:"error"`
}

type bulkResponse struct {
	Results []bulkResult `json:"results"`
}

type flushResponse struct {
	Flushed int `json:"flushed"`
}

type logLine struct {
	Level          string `json:"level"`
	RequestID      string `json:"requestId"`
	Method         string `json:"method"`
	Path           string `json:"path"`
	Status         int    `json:"status"`
	DurationMs     int64  `json:"durationMs"`
	UserID         *int   `json:"userId"`
	QuotaRemaining *int   `json:"quotaRemaining"`
	AuditSeq       int    `json:"auditSeq"`
}

// observer carries what the middleware must log and what every response header needs.
type observer struct {
	http.ResponseWriter
	requestID      string
	status         int
	matched        string
	userID         *int
	quotaRemaining *int
	replayed       bool
}

func (o *observer) WriteHeader(status int) {
	o.status = status
	if o.quotaRemaining != nil {
		o.Header().Set("X-Quota-Remaining", strconv.Itoa(*o.quotaRemaining))
	}
	if o.replayed {
		o.Header().Set("Idempotency-Replayed", "true")
	}
	o.ResponseWriter.WriteHeader(status)
}

// ------------------------------------------------------------------- middleware

func observe(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestID := r.Header.Get("X-Request-Id")
		if requestID == "" {
			requestID = rand.Text()
		}
		w.Header().Set("X-Request-Id", requestID)
		observed := &observer{ResponseWriter: w, requestID: requestID, status: http.StatusOK}
		before := len(audit)
		started := time.Now()
		next.ServeHTTP(observed, r)
		countRequest(observed.matched, observed.status)
		level := "info"
		if observed.status >= 500 {
			level = "error"
		} else if observed.status >= 400 {
			level = "warn"
		}
		_ = json.NewEncoder(os.Stdout).Encode(logLine{
			Level:          level,
			RequestID:      requestID,
			Method:         r.Method,
			Path:           r.URL.Path,
			Status:         observed.status,
			DurationMs:     time.Since(started).Milliseconds(),
			UserID:         observed.userID,
			QuotaRemaining: observed.quotaRemaining,
			AuditSeq:       len(audit) - before,
		})
	})
}

func route(handle func(*observer, *http.Request) *AppError) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		observed := w.(*observer)
		observed.matched = pattern(r)
		if err := handle(observed, r); err != nil {
			writeJSON(observed, err.Status, envelope(observed, err))
		}
	}
}

// pattern is the matched route pattern, never the concrete path.
func pattern(r *http.Request) string {
	if strings.Contains(r.Pattern, " ") {
		return r.Pattern
	}
	return r.Method + " " + r.Pattern
}

func envelope(w *observer, err *AppError) errorResponse {
	details := err.Details
	if details == nil {
		details = []detail{}
	}
	return errorResponse{Error: errorBody{Code: err.Code, Message: err.Message,
		RequestID: w.requestID, Details: details}}
}

// ---------------------------------------------------------------------- helpers

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

// respond writes one response, tagging it when the body carries a version.
func respond(w *observer, status int, body any) {
	if fields, ok := body.(row); ok {
		if version, found := fields["version"].(int); found {
			w.Header().Set("ETag", strconv.Itoa(version))
		}
	}
	writeJSON(w, status, body)
}

// begin authenticates, charges the quota, then checks the role. This order is fixed.
func begin(w *observer, r *http.Request, admin bool) (*User, *Session, *AppError) {
	user, session, err := authenticate(r.Header.Get("Authorization"))
	if err != nil {
		return nil, nil, err
	}
	w.userID = &user.ID
	remaining, err := chargeQuota(user, session)
	if err != nil {
		return nil, nil, err
	}
	w.quotaRemaining = &remaining
	if admin {
		if err := requireAdmin(user); err != nil {
			return nil, nil, err
		}
	}
	return user, session, nil
}

// idempotent runs produce once per Idempotency-Key, then replays the outcome.
func idempotent(w *observer, r *http.Request, session *Session,
	produce func() (int, any, *AppError)) *AppError {
	key := r.Header.Get("Idempotency-Key")
	slot := idempotencyKey{session.Token, key}
	if kept, found := idempotency[slot]; found && key != "" {
		w.replayed = true
		respond(w, kept.status, kept.body)
		return nil
	}
	status, body, err := produce()
	if err != nil {
		if key != "" {
			idempotency[slot] = recorded{err.Status, envelope(w, err)}
		}
		return err
	}
	if key != "" {
		idempotency[slot] = recorded{status, body}
	}
	respond(w, status, body)
	return nil
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

// text reads a string field. A present value of another type is malformed.
func text(body map[string]json.RawMessage, field, def string) (string, *AppError) {
	raw, present := body[field]
	if !present {
		return def, nil
	}
	var parsed *string
	if json.Unmarshal(raw, &parsed) != nil || parsed == nil {
		return "", badRequest()
	}
	return *parsed, nil
}

// whole reads an integer field. A present value of another type is malformed.
func whole(body map[string]json.RawMessage, field string, def *int) (*int, *AppError) {
	raw, present := body[field]
	if !present {
		return def, nil
	}
	var parsed *int
	if json.Unmarshal(raw, &parsed) != nil {
		return nil, badRequest()
	}
	return parsed, nil
}

// optionalText reports the field as a string, leaving a wrong type to validation.
func optionalText(body map[string]json.RawMessage, field string) (*string, bool) {
	raw, present := body[field]
	if !present {
		return nil, false
	}
	var parsed *string
	_ = json.Unmarshal(raw, &parsed)
	return parsed, true
}

// optionalInt reports the field as an integer, leaving a wrong type to validation.
func optionalInt(body map[string]json.RawMessage, field string) (*int, bool) {
	raw, present := body[field]
	if !present {
		return nil, false
	}
	var parsed *int
	_ = json.Unmarshal(raw, &parsed)
	return parsed, true
}

func parseID(raw string) (int, *AppError) {
	id, err := strconv.Atoi(raw)
	if err != nil {
		return 0, badRequest()
	}
	return id, nil
}

// digits reads a decimal integer, or -1 when the text is not one.
func digits(raw string) int {
	number, err := strconv.Atoi(raw)
	if err != nil {
		return -1
	}
	return number
}

func readPage(r *http.Request, allowed []string) (int, int, string, string, *AppError) {
	query := r.URL.Query()
	errors := []detail{}
	limit, offset := defaultLimit, 0
	sort, order := allowed[0], "asc"
	if query.Has("sort") {
		sort = query.Get("sort")
	}
	if query.Has("order") {
		order = query.Get("order")
	}
	if query.Has("limit") {
		limit = digits(query.Get("limit"))
		if limit < 1 || limit > maxLimit {
			errors = append(errors, fail("limit", "limit is out of range"))
		}
	}
	if query.Has("offset") {
		offset = digits(query.Get("offset"))
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

func ifMatch(r *http.Request, version int) *AppError {
	return checkIfMatch(r.Header.Get("If-Match"), version)
}

// ----------------------------------------------------------------- health, auth

func handleHealth(w *observer, r *http.Request) *AppError {
	respond(w, http.StatusOK, healthResponse{Status: "ok",
		Projects: liveCount(projects, func(p *Project) bool { return !p.Deleted }),
		Tasks:    liveCount(tasks, func(t *Task) bool { return !t.Deleted }),
		Comments: len(comments)})
	return nil
}

func handleLogin(w *observer, r *http.Request) *AppError {
	body, err := readBody(r)
	if err != nil {
		return err
	}
	username, err := text(body, "username", "")
	if err != nil {
		return err
	}
	password, err := text(body, "password", "")
	if err != nil {
		return err
	}
	errors := []detail{}
	if username == "" {
		errors = append(errors, fail("username", "username is required"))
	}
	if password == "" {
		errors = append(errors, fail("password", "password is required"))
	}
	if len(errors) > 0 {
		return invalid(errors)
	}
	token := rand.Text()
	user, err := login(username, password, token)
	if err != nil {
		return err
	}
	respond(w, http.StatusOK,
		loginResponse{Token: token, UserID: user.ID, Role: user.Role})
	return nil
}

func handleLogout(w *observer, r *http.Request) *AppError {
	_, session, err := begin(w, r, false)
	if err != nil {
		return err
	}
	delete(sessions, session.Token)
	w.WriteHeader(http.StatusNoContent)
	return nil
}

func handleMe(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	respond(w, http.StatusOK,
		meResponse{UserID: user.ID, Username: user.Username, Role: user.Role})
	return nil
}

// ------------------------------------------------------------------------ users

func handleListUsers(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, userSorts)
	if err != nil {
		return err
	}
	rows := []row{}
	for _, id := range sortedKeys(users) {
		if !users[id].Deleted {
			rows = append(rows, serializeUser(users[id]))
		}
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleCreateUser(w *observer, r *http.Request) *AppError {
	actor, session, err := begin(w, r, true)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	return idempotent(w, r, session, func() (int, any, *AppError) {
		username, err := text(body, "username", "")
		if err != nil {
			return 0, nil, err
		}
		password, err := text(body, "password", "")
		if err != nil {
			return 0, nil, err
		}
		role, hasRole := optionalText(body, "role")
		if !hasRole {
			role = pointer("user")
		}
		quota, hasQuota := optionalInt(body, "quota")
		if !hasQuota {
			quota = pointer(defaultQuota)
		}
		user, err := createUser(actor, username, password, role, quota)
		if err != nil {
			return 0, nil, err
		}
		return http.StatusCreated, serializeUser(user), nil
	})
}

func handleGetUser(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	user := findUser(id, false)
	if user == nil {
		return notFound()
	}
	respond(w, http.StatusOK, serializeUser(user))
	return nil
}

func handleUpdateUser(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, true)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	user := findUser(id, false)
	if user == nil {
		return notFound()
	}
	if err := ifMatch(r, user.Version); err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	role, hasRole := optionalText(body, "role")
	if !hasRole {
		role = pointer(user.Role)
	}
	quota, hasQuota := optionalInt(body, "quota")
	if !hasQuota {
		quota = pointer(user.Quota)
	}
	if err := updateUser(actor, user, role, quota); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeUser(user))
	return nil
}

func handleDeleteUser(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, true)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	user := findUser(id, false)
	if user == nil {
		return notFound()
	}
	if err := ifMatch(r, user.Version); err != nil {
		return err
	}
	if err := deleteUser(actor, user); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeUser(user))
	return nil
}

// --------------------------------------------------------------------- projects

func handleListProjects(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	include, err := checkIncludeDeleted(r.URL.Query(), user)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, projectSorts)
	if err != nil {
		return err
	}
	rows := []row{}
	for _, project := range visibleProjects(user, include) {
		rows = append(rows, serializeProject(project))
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleCreateProject(w *observer, r *http.Request) *AppError {
	actor, session, err := begin(w, r, true)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	return idempotent(w, r, session, func() (int, any, *AppError) {
		name, err := text(body, "name", "")
		if err != nil {
			return 0, nil, err
		}
		ownerID, err := whole(body, "ownerId", &actor.ID)
		if err != nil {
			return 0, nil, err
		}
		project, err := createProject(actor, name, ownerID)
		if err != nil {
			return 0, nil, err
		}
		return http.StatusCreated, serializeProject(project), nil
	})
}

func handleGetProject(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user, false)
	if err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeProject(project))
	return nil
}

func handleUpdateProject(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, true)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, actor, false)
	if err != nil {
		return err
	}
	if err := ifMatch(r, project.Version); err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	if _, present := body["name"]; present {
		name, err := text(body, "name", "")
		if err != nil {
			return err
		}
		if err := renameProject(actor, project, name); err != nil {
			return err
		}
	}
	respond(w, http.StatusOK, serializeProject(project))
	return nil
}

func handleDeleteProject(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, true)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, actor, false)
	if err != nil {
		return err
	}
	if err := ifMatch(r, project.Version); err != nil {
		return err
	}
	deleteProject(actor, project)
	respond(w, http.StatusOK, serializeProject(project))
	return nil
}

func handleRestoreProject(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, true)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, actor, true)
	if err != nil {
		return err
	}
	if err := ifMatch(r, project.Version); err != nil {
		return err
	}
	if err := restoreProject(actor, project); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeProject(project))
	return nil
}

// ------------------------------------------------------------------------ tasks

func handleListAllTasks(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	query := r.URL.Query()
	include, err := checkIncludeDeleted(query, user)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, taskSorts)
	if err != nil {
		return err
	}
	found, err := taskFilters(query, visibleTasks(user, include))
	if err != nil {
		return err
	}
	rows := []row{}
	for _, task := range found {
		rows = append(rows, serializeTask(task, user.Role))
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleListTasks(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, user, false)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, taskSorts)
	if err != nil {
		return err
	}
	rows := []row{}
	for _, task := range liveTasksOf(project.ID) {
		rows = append(rows, serializeTask(task, user.Role))
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleCreateTask(w *observer, r *http.Request) *AppError {
	actor, session, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	project, err := reachableProject(id, actor, false)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	return idempotent(w, r, session, func() (int, any, *AppError) {
		errors := []detail{}
		note, err := readNote(actor, body, "", &errors)
		if err != nil {
			return 0, nil, err
		}
		title, err := text(body, "title", "")
		if err != nil {
			return 0, nil, err
		}
		priority, err := whole(body, "priority", pointer(0))
		if err != nil {
			return 0, nil, err
		}
		assigneeID, err := whole(body, "assigneeId", nil)
		if err != nil {
			return 0, nil, err
		}
		task, err := createTask(actor, project, title, priority, assigneeID, note, errors)
		if err != nil {
			return 0, nil, err
		}
		return http.StatusCreated, serializeTask(task, actor.Role), nil
	})
}

func handleGetTask(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user, false)
	if err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeTask(task, user.Role))
	return nil
}

func handleReplaceTask(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, actor, false)
	if err != nil {
		return err
	}
	if err := ifMatch(r, task.Version); err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	errors := []detail{}
	note, err := readNote(actor, body, task.InternalNote, &errors)
	if err != nil {
		return err
	}
	title, err := text(body, "title", "")
	if err != nil {
		return err
	}
	priority, err := whole(body, "priority", pointer(0))
	if err != nil {
		return err
	}
	assigneeID, err := whole(body, "assigneeId", nil)
	if err != nil {
		return err
	}
	if err := replaceTask(actor, task, title, priority, assigneeID, note, errors); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeTask(task, actor.Role))
	return nil
}

func handleUpdateStatus(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, actor, false)
	if err != nil {
		return err
	}
	if err := ifMatch(r, task.Version); err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	status, _ := optionalText(body, "status")
	if err := moveStatus(actor, task, status); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeTask(task, actor.Role))
	return nil
}

func handleDeleteTask(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, actor, false)
	if err != nil {
		return err
	}
	if err := ifMatch(r, task.Version); err != nil {
		return err
	}
	deleteTask(actor, task)
	respond(w, http.StatusOK, serializeTask(task, actor.Role))
	return nil
}

func handleRestoreTask(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, actor, true)
	if err != nil {
		return err
	}
	if err := ifMatch(r, task.Version); err != nil {
		return err
	}
	if err := restoreTask(actor, task); err != nil {
		return err
	}
	respond(w, http.StatusOK, serializeTask(task, actor.Role))
	return nil
}

func handleBulkTasks(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	items := []json.RawMessage{}
	raw, present := body["operations"]
	wellFormed := present && json.Unmarshal(raw, &items) == nil
	if err := checkBulkSize(len(items), wellFormed); err != nil {
		return err
	}
	results := []bulkResult{}
	for index, entry := range items {
		item := map[string]json.RawMessage{}
		status, id, failure := 0, (*int)(nil), badRequest()
		if json.Unmarshal(entry, &item) == nil {
			status, id, failure = applyBulk(actor, item)
		}
		if failure != nil {
			results = append(results, bulkResult{Index: index, Status: failure.Status,
				Error: &failure.Code})
			continue
		}
		results = append(results, bulkResult{Index: index, Status: status, ID: id})
	}
	respond(w, http.StatusOK, bulkResponse{Results: results})
	return nil
}

func applyBulk(actor *User, item map[string]json.RawMessage) (int, *int, *AppError) {
	operation, _ := optionalText(item, "op")
	name := ""
	if operation != nil {
		name = *operation
	}
	switch name {
	case "create":
		projectID, err := whole(item, "projectId", pointer(0))
		if err != nil {
			return 0, nil, err
		}
		project, err := reachableProject(value(projectID), actor, false)
		if err != nil {
			return 0, nil, err
		}
		title, err := text(item, "title", "")
		if err != nil {
			return 0, nil, err
		}
		priority, err := whole(item, "priority", pointer(0))
		if err != nil {
			return 0, nil, err
		}
		task, err := createTask(actor, project, title, priority, nil, "", []detail{})
		if err != nil {
			return 0, nil, err
		}
		return http.StatusCreated, &task.ID, nil
	case "status":
		task, err := bulkTarget(actor, item)
		if err != nil {
			return 0, nil, err
		}
		status, _ := optionalText(item, "status")
		if err := moveStatus(actor, task, status); err != nil {
			return 0, nil, err
		}
		return http.StatusOK, &task.ID, nil
	case "delete":
		task, err := bulkTarget(actor, item)
		if err != nil {
			return 0, nil, err
		}
		deleteTask(actor, task)
		return http.StatusOK, &task.ID, nil
	}
	return 0, nil, invalid([]detail{fail("op", "op is not valid")})
}

// bulkTarget resolves the task an item names and checks the version it states.
func bulkTarget(actor *User, item map[string]json.RawMessage) (*Task, *AppError) {
	taskID, err := whole(item, "id", pointer(0))
	if err != nil {
		return nil, err
	}
	task, err := reachableTask(value(taskID), actor, false)
	if err != nil {
		return nil, err
	}
	version, err := whole(item, "version", nil)
	if err != nil {
		return nil, err
	}
	if err := checkVersion(version, task.Version); err != nil {
		return nil, err
	}
	return task, nil
}

// --------------------------------------------------------------------- comments

func handleListComments(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, user, false)
	if err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, commentSorts)
	if err != nil {
		return err
	}
	rows := []row{}
	for _, commentID := range sortedKeys(comments) {
		if comments[commentID].TaskID == task.ID {
			rows = append(rows, serializeComment(comments[commentID]))
		}
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleCreateComment(w *observer, r *http.Request) *AppError {
	actor, session, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	task, err := reachableTask(id, actor, false)
	if err != nil {
		return err
	}
	body, err := readBody(r)
	if err != nil {
		return err
	}
	return idempotent(w, r, session, func() (int, any, *AppError) {
		note, err := text(body, "body", "")
		if err != nil {
			return 0, nil, err
		}
		comment, err := createComment(actor, task, note)
		if err != nil {
			return 0, nil, err
		}
		return http.StatusCreated, serializeComment(comment), nil
	})
}

func handleDeleteComment(w *observer, r *http.Request) *AppError {
	actor, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	id, err := parseID(r.PathValue("id"))
	if err != nil {
		return err
	}
	comment := findComment(id)
	if comment == nil {
		return notFound()
	}
	if _, err := reachableTask(comment.TaskID, actor, true); err != nil {
		return err
	}
	if err := removeComment(actor, comment); err != nil {
		return err
	}
	w.WriteHeader(http.StatusNoContent)
	return nil
}

// ---------------------------------------------------- search, reports, telemetry

func handleSearch(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	query := r.URL.Query().Get("q")
	if query == "" {
		return invalid([]detail{fail("q", "q is required")})
	}
	respond(w, http.StatusOK, search(user, query))
	return nil
}

func handleWorkload(w *observer, r *http.Request) *AppError {
	user, _, err := begin(w, r, false)
	if err != nil {
		return err
	}
	groupBy := "status"
	if r.URL.Query().Has("groupBy") {
		groupBy = r.URL.Query().Get("groupBy")
	}
	if !slices.Contains(groupBys, groupBy) {
		return invalid([]detail{fail("groupBy", "groupBy is not valid")})
	}
	respond(w, http.StatusOK, workload(user, groupBy))
	return nil
}

func handleListAudit(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, seqSorts)
	if err != nil {
		return err
	}
	query := r.URL.Query()
	rows := []row{}
	for _, entry := range audit {
		if query.Has("actorId") && strconv.Itoa(entry.ActorID) != query.Get("actorId") {
			continue
		}
		if query.Has("resource") && entry.Resource != query.Get("resource") {
			continue
		}
		if query.Has("action") && entry.Action != query.Get("action") {
			continue
		}
		rows = append(rows, serializeAudit(entry))
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleListOutbox(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	limit, offset, sort, order, err := readPage(r, seqSorts)
	if err != nil {
		return err
	}
	query := r.URL.Query()
	rows := []row{}
	for _, event := range outbox {
		if query.Has("delivered") &&
			event.Delivered != (query.Get("delivered") == "true") {
			continue
		}
		rows = append(rows, serializeOutbox(event))
	}
	respond(w, http.StatusOK, paginate(rows, limit, offset, sort, order))
	return nil
}

func handleFlushOutbox(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	respond(w, http.StatusOK, flushResponse{Flushed: flushOutbox()})
	return nil
}

func handleMetrics(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	respond(w, http.StatusOK, metrics())
	return nil
}

func handleStats(w *observer, r *http.Request) *AppError {
	if _, _, err := begin(w, r, true); err != nil {
		return err
	}
	respond(w, http.StatusOK, stats())
	return nil
}

func handleNotFound(w *observer, r *http.Request) *AppError {
	return notFound()
}

func main() {
	seed()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", route(handleHealth))
	mux.HandleFunc("POST /auth/login", route(handleLogin))
	mux.HandleFunc("POST /auth/logout", route(handleLogout))
	mux.HandleFunc("GET /me", route(handleMe))
	mux.HandleFunc("GET /users", route(handleListUsers))
	mux.HandleFunc("POST /users", route(handleCreateUser))
	mux.HandleFunc("GET /users/{id}", route(handleGetUser))
	mux.HandleFunc("PATCH /users/{id}", route(handleUpdateUser))
	mux.HandleFunc("DELETE /users/{id}", route(handleDeleteUser))
	mux.HandleFunc("GET /projects", route(handleListProjects))
	mux.HandleFunc("POST /projects", route(handleCreateProject))
	mux.HandleFunc("GET /projects/{id}", route(handleGetProject))
	mux.HandleFunc("PATCH /projects/{id}", route(handleUpdateProject))
	mux.HandleFunc("DELETE /projects/{id}", route(handleDeleteProject))
	mux.HandleFunc("POST /projects/{id}/restore", route(handleRestoreProject))
	mux.HandleFunc("GET /projects/{id}/tasks", route(handleListTasks))
	mux.HandleFunc("POST /projects/{id}/tasks", route(handleCreateTask))
	mux.HandleFunc("GET /tasks", route(handleListAllTasks))
	mux.HandleFunc("POST /tasks/bulk", route(handleBulkTasks))
	mux.HandleFunc("GET /tasks/{id}", route(handleGetTask))
	mux.HandleFunc("PUT /tasks/{id}", route(handleReplaceTask))
	mux.HandleFunc("DELETE /tasks/{id}", route(handleDeleteTask))
	mux.HandleFunc("PATCH /tasks/{id}/status", route(handleUpdateStatus))
	mux.HandleFunc("POST /tasks/{id}/restore", route(handleRestoreTask))
	mux.HandleFunc("GET /tasks/{id}/comments", route(handleListComments))
	mux.HandleFunc("POST /tasks/{id}/comments", route(handleCreateComment))
	mux.HandleFunc("DELETE /comments/{id}", route(handleDeleteComment))
	mux.HandleFunc("GET /search", route(handleSearch))
	mux.HandleFunc("GET /reports/workload", route(handleWorkload))
	mux.HandleFunc("GET /audit", route(handleListAudit))
	mux.HandleFunc("GET /outbox", route(handleListOutbox))
	mux.HandleFunc("POST /outbox/flush", route(handleFlushOutbox))
	mux.HandleFunc("GET /metrics", route(handleMetrics))
	mux.HandleFunc("GET /stats", route(handleStats))
	mux.HandleFunc("/", route(handleNotFound))
	_ = http.ListenAndServe(port, observe(mux))
}
